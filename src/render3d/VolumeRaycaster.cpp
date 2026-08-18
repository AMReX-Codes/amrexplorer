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

struct RayGeometry {
    Real3 lower;
    std::array<double, 3> pitch{};
    std::array<int, 3> dims{};
};

// The parameter span [tEnter, tExit] over which the ray is inside the grid's
// box; false when it misses.
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
    return tExit > tEnter && tExit > 0.0;
}

} // namespace

std::optional<int> transferEntryFor(double value, const VolumeRange& range,
    int entryCount) noexcept
{
    if (entryCount < 1 || !std::isfinite(value)
        || (range.logarithmic && !(value > 0.0))) {
        return std::nullopt;
    }
    const auto mapped = range.logarithmic ? std::log(value) : value;
    const auto minimum = range.logarithmic ? std::log(range.minimum) : range.minimum;
    const auto maximum = range.logarithmic ? std::log(range.maximum) : range.maximum;
    // The division is deliberately not hoisted into a reciprocal multiply:
    // renderScalarPlane keeps it, and the two must land in the same slot for
    // a value at the maximum or on a tie (see its comment).
    const auto normalized = (mapped - minimum) / (maximum - minimum);
    if (!(normalized > 0.0)) {
        return 0;
    }
    if (!(normalized < 1.0)) {
        return entryCount - 1;
    }
    return static_cast<int>(normalized * static_cast<double>(entryCount - 1));
}

std::optional<std::pair<double, double>> volumeGridRange(
    const VolumeGrid& grid, bool logarithmic)
{
    auto minimum = std::numeric_limits<double>::infinity();
    auto maximum = -std::numeric_limits<double>::infinity();
    for (const auto value : grid.values) {
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
    std::uint64_t voxelCount = 1;
    for (const auto extent : grid.dims) {
        if (extent < 1) {
            throw std::invalid_argument("volume grid dimensions must be positive");
        }
        voxelCount *= static_cast<std::uint64_t>(extent);
    }
    if (grid.values.size() != voxelCount) {
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
    if (!std::isfinite(settings.range.minimum) || !std::isfinite(settings.range.maximum)
        || !(settings.range.minimum < settings.range.maximum)
        || (settings.range.logarithmic && !(settings.range.minimum > 0.0))) {
        throw std::invalid_argument("volume range must be finite, ordered, and positive when logarithmic");
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
    const auto entryCount = static_cast<int>(entries.size());
    RayGeometry geometry;
    geometry.lower = grid.region.lower;
    geometry.dims = grid.dims;
    double minimumPitch = std::numeric_limits<double>::infinity();
    for (std::size_t axis = 0; axis < 3; ++axis) {
        geometry.pitch[axis] = (grid.region.upper[axis] - grid.region.lower[axis])
            / static_cast<double>(grid.dims[axis]);
        minimumPitch = std::min(minimumPitch, geometry.pitch[axis]);
    }
    const auto step = minimumPitch / static_cast<double>(settings.samplesPerVoxel);
    const auto viewport = viewportFrame(width, height);
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
            const auto ray = pixelRay(settings.camera, viewport, settings.domain,
                static_cast<double>(column) + 0.5, pixelY);
            double tEnter = 0.0;
            double tExit = 0.0;
            if (!clipToBox(ray, grid.region, tEnter, tExit)) {
                continue;
            }
            tEnter = std::max(tEnter, 0.0);
            double alpha = 0.0;
            double red = 0.0;
            double green = 0.0;
            double blue = 0.0;
            for (double t = tEnter + 0.5 * step; t < tExit; t += step) {
                std::array<std::size_t, 3> index{};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const auto position = ray.origin[axis] + t * ray.direction[axis];
                    index[axis] = static_cast<std::size_t>(std::clamp(
                        static_cast<int>(std::floor(
                            (position - geometry.lower[axis]) / geometry.pitch[axis])),
                        0, geometry.dims[axis] - 1));
                }
                const auto offset = index[0] + gridRowStride * index[1]
                    + slabStride * index[2];
                const auto value = static_cast<double>(grid.values[offset]);
                const auto entryIndex = transferEntryFor(value, settings.range, entryCount);
                if (!entryIndex) {
                    continue;
                }
                const auto& entry = entries[static_cast<std::size_t>(*entryIndex)];
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

    const auto requested = settings.threadCount != 0
        ? settings.threadCount : std::thread::hardware_concurrency();
    const auto threadCount = static_cast<int>(std::clamp<unsigned>(
        requested, 1U, static_cast<unsigned>(std::max(1, height))));
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
        for (int band = 0; band < threadCount; ++band) {
            const auto begin = band * rowsPerThread;
            const auto end = std::min(height, begin + rowsPerThread);
            if (begin >= end) {
                break;
            }
            threads.emplace_back(renderBand, begin, end);
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
    frame.metrics.renderMilliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    return frame;
}

} // namespace amrvis
