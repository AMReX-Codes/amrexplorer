#include <amrexplorer/render3d/VolumeRaycaster.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace amrvis {
namespace {

// A transfer-function entry resolved for compositing: linear colour
// components in [0, 1] and the opacity of one ray sample (the entry's
// per-voxel opacity spread over the samples that cross a voxel).
struct Entry {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double stepOpacity = 0.0;
};

std::vector<Entry> resolveEntries(
    const VolumeTransferFunction& transfer, int samplesPerVoxel)
{
    std::vector<Entry> entries(transfer.colors.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto color = transfer.colors[index];
        auto& entry = entries[index];
        entry.red = static_cast<double>((color >> 16U) & 0xFFU) / 255.0;
        entry.green = static_cast<double>((color >> 8U) & 0xFFU) / 255.0;
        entry.blue = static_cast<double>(color & 0xFFU) / 255.0;
        // Opacity correction: k samples of stepOpacity compose to the
        // entry's opacity, 1 - (1 - a_step)^k = a, so the picture does not
        // darken as the sampling gets finer.
        const auto opacity = static_cast<double>(transfer.opacities[index]);
        entry.stepOpacity = opacity >= 1.0 ? 1.0
            : 1.0 - std::pow(1.0 - opacity, 1.0 / static_cast<double>(samplesPerVoxel));
    }
    return entries;
}

std::uint32_t packPremultiplied(double alpha, double red, double green,
    double blue) noexcept
{
    const auto channel = [](double value) {
        return static_cast<std::uint32_t>(
            std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
    };
    return (channel(alpha) << 24U) | (channel(red) << 16U)
        | (channel(green) << 8U) | channel(blue);
}

// Saturating, as the sampler's product is: a dims product large enough to
// wrap would otherwise equal the size of a much smaller (or empty) values
// vector, pass the storage check below, and let the march index past its end.
std::uint64_t voxelCountOf(const std::array<int, 3>& dims) noexcept
{
    constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t result = 1;
    for (const auto extent : dims) {
        const auto value = static_cast<std::uint64_t>(extent);
        if (value != 0 && result > limit / value) {
            return limit;
        }
        result *= value;
    }
    return result;
}

// A range with its logarithms already taken. transferEntryFor is called once
// per ray sample, and std::log of the two bounds in there is two library
// calls per sample the compiler cannot hoist for itself (they set errno) --
// the same reason renderScalarPlane resolves its bounds before its pixel
// loop.
struct ResolvedRange {
    double minimum = 0.0;
    double span = 1.0;
    bool logarithmic = false;
};

// nullopt for a range no value can be mapped through: non-finite bounds, an
// empty or unordered span, an infinite span (every value would land in the
// bottom entry), or a logarithmic range reaching to zero.
std::optional<ResolvedRange> resolveRange(const VolumeRange& range) noexcept
{
    if (!std::isfinite(range.minimum) || !std::isfinite(range.maximum)
        || (range.logarithmic && !(range.minimum > 0.0))) {
        return std::nullopt;
    }
    ResolvedRange resolved;
    resolved.logarithmic = range.logarithmic;
    resolved.minimum = range.logarithmic ? std::log(range.minimum) : range.minimum;
    const auto maximum = range.logarithmic ? std::log(range.maximum) : range.maximum;
    // Hoisting the subtraction is exact -- same operands, same result -- but
    // turning the division below into a reciprocal multiply is not, and the
    // difference is visible in the picture. renderScalarPlane makes the same
    // trade and explains it at length; the two must agree slot for slot.
    resolved.span = maximum - resolved.minimum;
    if (!(resolved.span > 0.0) || !std::isfinite(resolved.span)) {
        return std::nullopt;
    }
    return resolved;
}

// The entry a mappable value takes: the caller has already rejected values
// the range cannot map.
int entryForResolved(double value, const ResolvedRange& range,
    int entryCount) noexcept
{
    const auto mapped = range.logarithmic ? std::log(value) : value;
    const auto normalized = (mapped - range.minimum) / range.span;
    if (!(normalized > 0.0)) {
        return 0;
    }
    if (!(normalized < 1.0)) {
        return entryCount - 1;
    }
    return static_cast<int>(normalized * static_cast<double>(entryCount - 1));
}

bool mappableValue(double value, const ResolvedRange& range) noexcept
{
    return std::isfinite(value) && !(range.logarithmic && !(value > 0.0));
}

struct RayGeometry {
    Real3 lower;
    std::array<double, 3> pitch{};
    std::array<int, 3> dims{};
};

// The parameter span [tEnter, tExit] over which the ray is inside the grid's
// box; false when it misses. tEnter may be negative: the ray's origin sits
// outside the *domain*, which is not necessarily the grid's region, so a
// region reaching further toward the viewer starts behind it. An orthographic
// projection has no near plane -- t is a position along the view line, not a
// distance from an eye -- so marching from a negative tEnter is still front
// to back, and clipping it away would drop the front of such a grid.
bool clipToBox(const Ray& ray, const RealBox& box, double& tEnter,
    double& tExit) noexcept
{
    tEnter = -std::numeric_limits<double>::infinity();
    tExit = std::numeric_limits<double>::infinity();
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto direction = ray.direction[axis];
        const auto origin = ray.origin[axis];
        if (std::abs(direction) < 1.0e-12) {
            if (origin < box.lower[axis] || origin > box.upper[axis]) {
                return false;
            }
            continue;
        }
        auto t0 = (box.lower[axis] - origin) / direction;
        auto t1 = (box.upper[axis] - origin) / direction;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
    }
    return tExit > tEnter;
}

} // namespace

std::optional<int> transferEntryFor(double value, const VolumeRange& range,
    int entryCount) noexcept
{
    const auto resolved = resolveRange(range);
    if (entryCount < 1 || !resolved || !mappableValue(value, *resolved)) {
        return std::nullopt;
    }
    return entryForResolved(value, *resolved, entryCount);
}

std::optional<std::pair<double, double>> volumeGridRange(
    const VolumeGrid& grid, bool logarithmic, StopToken cancellation)
{
    auto minimum = std::numeric_limits<double>::infinity();
    auto maximum = -std::numeric_limits<double>::infinity();
    // The grid runs to the 512^3 budget, half a gigabyte of floats. A scan
    // that size with no way out is a frozen window locally and a server
    // ignoring a client's cancel remotely, so it is polled like the march.
    constexpr std::size_t cancellationStride = 1U << 16U;
    for (std::size_t index = 0; index < grid.values.size(); ++index) {
        if (index % cancellationStride == 0 && cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto value = grid.values[index];
        if (!std::isfinite(value) || (logarithmic && !(value > 0.0F))) {
            continue;
        }
        minimum = std::min(minimum, static_cast<double>(value));
        maximum = std::max(maximum, static_cast<double>(value));
    }
    if (!(minimum <= maximum)) {
        return std::nullopt;
    }
    return std::pair{minimum, maximum};
}

VolumeFrame raycastVolume(const VolumeGrid& grid,
    const RaycastSettings& settings, StopToken cancellation)
{
    const auto started = std::chrono::steady_clock::now();
    for (const auto extent : grid.dims) {
        if (extent < 1) {
            throw std::invalid_argument("volume grid dimensions must be positive");
        }
    }
    if (grid.values.size() != voxelCountOf(grid.dims)) {
        throw std::invalid_argument("volume grid storage does not match its dimensions");
    }
    if (!grid.region.valid(3)) {
        throw std::invalid_argument("volume grid region must have finite positive extent");
    }
    if (!settings.domain.valid(3)) {
        throw std::invalid_argument("camera domain must have finite positive extent");
    }
    for (const auto extent : settings.outputSize) {
        if (extent < 1 || extent > maxVolumeOutputDimension) {
            throw std::invalid_argument("output dimensions must be within [1, 4096]");
        }
    }
    const auto mapping = resolveRange(settings.range);
    if (!mapping) {
        throw std::invalid_argument("volume range must be finite with a finite span, ordered, and positive when logarithmic");
    }
    if (const auto errors = validateVolumeTransferFunction(settings.transfer);
        !errors.empty()) {
        throw std::invalid_argument(errors.front());
    }
    if (settings.samplesPerVoxel < 1
        || settings.samplesPerVoxel > maxVolumeSamplesPerVoxel) {
        throw std::invalid_argument("samples per voxel must be within [1, 8]");
    }
    if (!std::isfinite(settings.camera.azimuth) || !std::isfinite(settings.camera.elevation)
        || !(settings.camera.zoom >= minVolumeZoom)
        || !(settings.camera.zoom <= maxVolumeZoom)) {
        throw std::invalid_argument("camera is not finite or its zoom is out of range");
    }

    const auto width = settings.outputSize[0];
    const auto height = settings.outputSize[1];
    VolumeFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pixels.assign(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    frame.usedRange = settings.range;
    frame.metrics.gridDims = grid.dims;
    frame.metrics.coveredVoxels = grid.coveredVoxels;
    frame.metrics.sampledMaximumLevel = grid.maximumLevel;

    const auto entries = resolveEntries(settings.transfer, settings.samplesPerVoxel);
    // At least two, by the transfer-function validator above: the march
    // indexes with entryForResolved's result and does not re-check it.
    const auto entryCount = static_cast<int>(entries.size());
    const auto mappedRange = *mapping;
    RayGeometry geometry;
    geometry.lower = grid.region.lower;
    geometry.dims = grid.dims;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        geometry.pitch[axis] = (grid.region.upper[axis] - grid.region.lower[axis])
            / static_cast<double>(grid.dims[axis]);
        // RealBox::valid accepts a degenerate box, which would give a zero
        // pitch and a zero step -- a march that never advances.
        if (!(geometry.pitch[axis] > 0.0) || !std::isfinite(geometry.pitch[axis])) {
            throw std::invalid_argument(
                "volume grid region must have finite positive extent");
        }
    }
    const auto viewport = viewportFrame(width, height);
    const auto rays = rayField(settings.camera, viewport, settings.domain);
    // How many voxels a ray enters per unit length: it crosses a new one
    // every time it meets one of the three families of voxel planes, and it
    // meets |direction| / pitch of each family per unit length. The
    // reciprocal is the mean distance a ray spends in one voxel, which is
    // what samplesPerVoxel divides and what the opacity correction assumes.
    // For an axis-aligned view of a cubic grid that is just the pitch; using
    // the smallest pitch instead would oversample a coarse axis by the ratio
    // between the two -- an entry authored at opacity 0.1 composites to 0.97
    // on a 32:1 grid -- and, for a region far thinner than it is wide, would
    // take samples the whole way across in steps sized for the thin axis.
    double crossingsPerLength = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        crossingsPerLength += std::abs(rays.direction[axis]) / geometry.pitch[axis];
    }
    const auto step = 1.0
        / (crossingsPerLength * static_cast<double>(settings.samplesPerVoxel));
    if (!(step > 0.0) || !std::isfinite(step)) {
        throw std::invalid_argument("the grid pitch does not give a usable sample step");
    }
    // A ray inside the box travels at most extent / |direction| along each
    // axis, so the count below is at most samplesPerVoxel * sum(dims): the
    // ceiling is a backstop, not a working limit, and only a region orders of
    // magnitude thinner than the domain can reach it (there it renders
    // approximately rather than marching for hours).
    const auto sampleCeiling = static_cast<double>(settings.samplesPerVoxel)
        * (static_cast<double>(grid.dims[0]) + static_cast<double>(grid.dims[1])
            + static_cast<double>(grid.dims[2]))
        + 2.0;
    // Front-to-back compositing stops once the ray is this opaque: whatever
    // lies behind can add at most 0.001 to any channel, a quarter of one
    // 8-bit level, so the pixel is what a full march would round to.
    constexpr double opaqueEnough = 0.999;
    const auto rowStride = static_cast<std::size_t>(width);
    const auto gridRowStride = static_cast<std::size_t>(grid.dims[0]);
    const auto slabStride = gridRowStride * static_cast<std::size_t>(grid.dims[1]);

    const auto renderRow = [&](int row) {
        auto* pixels = frame.pixels.data() + rowStride * static_cast<std::size_t>(row);
        const auto pixelY = static_cast<double>(row) + 0.5;
        for (int column = 0; column < width; ++column) {
            const auto ray = rayAt(rays, static_cast<double>(column) + 0.5, pixelY);
            double tEnter = 0.0;
            double tExit = 0.0;
            if (!clipToBox(ray, grid.region, tEnter, tExit)) {
                continue;
            }
            // A count, not an accumulation: `t += step` stops advancing once
            // step falls below an ulp of t, which a grid thin beside its
            // domain can arrange, and that march would never end. Computing t
            // from the index also keeps the sample positions free of the
            // drift a long accumulation collects.
            const auto ratio = (tExit - tEnter) / step;
            if (!(ratio > 0.0)) {
                continue;
            }
            const auto sampleCount = static_cast<std::int64_t>(
                std::min(std::floor(ratio + 0.5), sampleCeiling));
            double alpha = 0.0;
            double red = 0.0;
            double green = 0.0;
            double blue = 0.0;
            for (std::int64_t sample = 0; sample < sampleCount; ++sample) {
                const auto t = tEnter + (static_cast<double>(sample) + 0.5) * step;
                std::array<std::size_t, 3> index{};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const auto position = ray.origin[axis] + t * ray.direction[axis];
                    const auto voxel = std::floor(
                        (position - geometry.lower[axis]) / geometry.pitch[axis]);
                    // Clamped as a double, before the cast: casting a value
                    // past the int range is undefined, and std::clamp passes
                    // a NaN straight through to it. Both are reachable from a
                    // ray running nearly parallel to a very thin axis.
                    const auto limit = static_cast<double>(geometry.dims[axis] - 1);
                    index[axis] = voxel > 0.0
                        ? static_cast<std::size_t>(voxel < limit ? voxel : limit)
                        : 0U;
                }
                const auto offset = index[0] + gridRowStride * index[1]
                    + slabStride * index[2];
                const auto value = static_cast<double>(grid.values[offset]);
                if (!mappableValue(value, mappedRange)) {
                    continue;
                }
                const auto& entry = entries[static_cast<std::size_t>(
                    entryForResolved(value, mappedRange, entryCount))];
                if (!(entry.stepOpacity > 0.0)) {
                    continue;
                }
                const auto weight = (1.0 - alpha) * entry.stepOpacity;
                red += weight * entry.red;
                green += weight * entry.green;
                blue += weight * entry.blue;
                alpha += weight;
                if (alpha >= opaqueEnough) {
                    break;
                }
            }
            if (alpha > 0.0) {
                pixels[static_cast<std::size_t>(column)]
                    = packPremultiplied(alpha, red, green, blue);
            }
        }
    };

    // Bounded like every other setting rather than taken as given: one thread
    // per row already costs more in thread creation than the rows are worth,
    // and a request for tens of thousands of them is a request to fail.
    const auto hardware = std::max(1U, std::thread::hardware_concurrency());
    const auto ceiling = std::min(
        static_cast<unsigned>(std::max(1, height)), 4U * hardware);
    const auto requested = settings.threadCount != 0
        ? settings.threadCount : hardware;
    const auto threadCount = static_cast<int>(
        std::clamp(requested, 1U, ceiling));
    const auto rowsPerThread = (height + threadCount - 1) / threadCount;
    std::atomic<bool> cancelled{false};
    std::exception_ptr failure;
    std::mutex failureMutex;
    const auto renderBand = [&](int begin, int end) {
        try {
            for (int row = begin; row < end; ++row) {
                if ((row - begin) % 16 == 0
                    && (cancelled.load(std::memory_order_relaxed)
                        || cancellation.stop_requested())) {
                    cancelled.store(true, std::memory_order_relaxed);
                    return;
                }
                renderRow(row);
            }
        } catch (...) {
            const std::lock_guard<std::mutex> lock(failureMutex);
            if (!failure) {
                failure = std::current_exception();
            }
            cancelled.store(true, std::memory_order_relaxed);
        }
    };
    if (threadCount == 1) {
        renderBand(0, height);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(threadCount));
        try {
            for (int band = 0; band < threadCount; ++band) {
                const auto begin = band * rowsPerThread;
                const auto end = std::min(height, begin + rowsPerThread);
                if (begin >= end) {
                    break;
                }
                threads.emplace_back(renderBand, begin, end);
            }
        } catch (...) {
            // std::thread construction can fail once earlier bands are
            // already running -- a process thread limit, a loaded machine.
            // Stopping and joining them here is what keeps ~vector<thread>
            // from meeting a joinable thread and calling std::terminate.
            cancelled.store(true, std::memory_order_relaxed);
            for (auto& thread : threads) {
                thread.join();
            }
            throw;
        }
        for (auto& thread : threads) {
            thread.join();
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (cancelled.load()) {
        throw ReadCancelled();
    }
    // Microseconds, not milliseconds: a small viewport renders in well under
    // one, and a whole-millisecond metric reports those as 0 -- which reads
    // as "not measured" rather than "fast".
    frame.metrics.renderMicroseconds = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count()));
    return frame;
}

} // namespace amrvis
