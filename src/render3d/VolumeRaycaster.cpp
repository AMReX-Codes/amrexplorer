#include <amrexplorer/render3d/VolumeRaycaster.hpp>

#include <amrexplorer/core/ValueMapping.hpp>

#include <algorithm>
#include <array>
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
#include <utility>
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
        // No special case for a fully opaque entry: the validator bounds
        // opacities to [0, 1], and std::pow(0.0, 1 / k) is exactly 0 for
        // every k, so the expression already yields 1.
        const auto opacity = static_cast<double>(transfer.opacities[index]);
        entry.stepOpacity = 1.0
            - std::pow(1.0 - opacity, 1.0 / static_cast<double>(samplesPerVoxel));
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

// The parameter span [tEnter, tExit] over which the ray is inside the grid's
// box; false when it misses. tEnter may be negative: the ray's origin sits
// outside the *domain*, which is not necessarily the grid's region, so a
// region reaching further toward the viewer starts behind it. An orthographic
// projection has no near plane -- t is a position along the view line, not a
// distance from an eye -- so marching from a negative tEnter is still front
// to back, and clipping it away would drop the front of such a grid.
// The frame-invariant half of clipToBox. An orthographic projection gives
// every pixel the same direction, so which axes the ray runs parallel to, and
// the reciprocal the slab test divides by, are properties of the frame -- the
// compiler cannot hoist the division itself, since x / y is not x * (1 / y)
// without -ffast-math.
struct SlabAxes {
    std::array<double, 3> inverseDirection{};
    std::array<bool, 3> parallel{};
};

SlabAxes slabAxesFor(const Real3& direction) noexcept
{
    SlabAxes axes;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        axes.parallel[axis] = std::abs(direction[axis]) < 1.0e-12;
        axes.inverseDirection[axis] = axes.parallel[axis]
            ? 0.0 : 1.0 / direction[axis];
    }
    return axes;
}

bool clipToBox(const Ray& ray, const SlabAxes& axes, const RealBox& box,
    double& tEnter, double& tExit) noexcept
{
    tEnter = -std::numeric_limits<double>::infinity();
    tExit = std::numeric_limits<double>::infinity();
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto origin = ray.origin[axis];
        if (axes.parallel[axis]) {
            // isfinite first: a NaN origin fails both comparisons below, so
            // an axis the ray runs perpendicular to would accept it and the
            // finite tEnter/tExit from the other axes would report a hit.
            // The march then floors a NaN, clamps it to voxel zero, and
            // paints the pixel from an arbitrary slab.
            if (!std::isfinite(origin) || origin < box.lower[axis]
                || origin > box.upper[axis]) {
                return false;
            }
            continue;
        }
        auto t0 = (box.lower[axis] - origin) * axes.inverseDirection[axis];
        auto t1 = (box.upper[axis] - origin) * axes.inverseDirection[axis];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
    }
    // Finite, not merely ordered: a NaN origin leaves both bounds untouched
    // -- std::max(-inf, NaN) is -inf and std::min(inf, NaN) is inf, because
    // every comparison against a NaN is false -- and the ray would report a
    // hit spanning the whole line. The parallel branch above rejects its own
    // NaN separately, because an axis that never divides never reaches here.
    return tExit > tEnter && std::isfinite(tEnter) && std::isfinite(tExit);
}

} // namespace

std::optional<int> transferEntryFor(double value, const VolumeRange& range,
    int entryCount) noexcept
{
    const auto resolved = resolveValueRange(
        range.minimum, range.maximum, range.logarithmic);
    if (entryCount < 1 || !resolved || !mappableValue(value, *resolved)) {
        return std::nullopt;
    }
    return valueSlot(value, *resolved, entryCount);
}

int raycastThreadCount(unsigned requested, int height) noexcept
{
    // Bounded rather than taken as given: one thread per row already costs
    // more in thread creation than a row is worth, and a request for tens of
    // thousands of them is a request to fail. Exposed so a caller reporting
    // its own timings names the count the render actually used.
    const auto hardware = std::max(1U, std::thread::hardware_concurrency());
    // Saturating, because std::clamp is undefined when lo exceeds hi: a
    // hardware_concurrency at or above 2^30 (a spoofed or virtualised value)
    // wraps 4 * hardware, and a ceiling of 0 would give clamp(x, 1, 0).
    constexpr auto unsignedMax = std::numeric_limits<unsigned>::max();
    const auto scaled = hardware <= unsignedMax / 4U ? 4U * hardware : unsignedMax;
    const auto ceiling = std::min(
        static_cast<unsigned>(std::max(1, height)), scaled);
    return static_cast<int>(
        std::clamp(requested != 0 ? requested : hardware, 1U, ceiling));
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
    // Before the loop, so an empty grid answers the token too: the contract
    // is "throws ReadCancelled like the render", and the render polls whether
    // or not it has a pixel to draw.
    if (cancellation.stop_requested()) {
        throw ReadCancelled();
    }
    for (std::size_t index = 0; index < grid.values.size(); ++index) {
        if (index % cancellationStride == 0 && index != 0
            && cancellation.stop_requested()) {
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
    for (const auto extent : grid.dims) {
        if (extent < 1) {
            throw std::invalid_argument("volume grid dimensions must be positive");
        }
    }
    // The budget first, because it is the cheaper refusal: a caller whose
    // dims are out of range should not have to have allocated the matching
    // storage to be told so. Note it bounds the grid's *total* voxels, not
    // the samples on any one ray: an elongated grid within the budget, say
    // {1, 1, 16777216}, still puts tens of millions of samples on a single
    // ray. What keeps that answerable is the poll inside the sample loop,
    // not this check.
    const auto voxelCount = volumeVoxelCount(grid.dims);
    if (voxelCount > maxVolumeVoxelBudget) {
        throw std::invalid_argument("volume grid exceeds the voxel budget");
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
    // The camera normalises by the largest extent, so an infinite span makes
    // every ray origin NaN (the rotated basis has exact zeros, and zero times
    // infinity is NaN). The region gets the same guard where its pitch is
    // computed.
    if (!settings.domain.finiteSpan(3)) {
        throw std::invalid_argument("camera domain must have finite positive extent");
    }
    for (const auto extent : settings.outputSize) {
        if (extent < 1 || extent > maxVolumeOutputDimension) {
            throw std::invalid_argument("output dimensions must be within [1, 4096]");
        }
    }
    const auto mapping = resolveValueRange(
        settings.range.minimum, settings.range.maximum, settings.range.logarithmic);
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
    const auto& lower = grid.region.lower;
    // The reciprocal, because the march divides by the pitch three times per
    // sample and the pitch is fixed for the frame. Unlike the range mapping,
    // where the reciprocal costs the exact palette-slot ties, what this
    // quotient can move is bounded: under Nearest it only feeds std::floor, so
    // the sole values it moves are samples landing exactly on a voxel plane,
    // an arbitrary tie either way. Under Linear it also feeds an interpolation
    // weight, where the rounding is no longer confined to ties -- but it is an
    // ulp of a weight, three orders below the 8-bit channel the weight ends up
    // in, so it cannot move a rendered pixel.
    std::array<double, 3> inversePitch{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto pitch = (grid.region.upper[axis] - grid.region.lower[axis])
            / static_cast<double>(grid.dims[axis]);
        // RealBox::valid rejects a degenerate or infinite box, so this is not
        // about an empty region. It catches the two ways a *valid* box still
        // fails to give a usable pitch: a denormal span divided by large dims
        // underflows to exactly zero (a step that never advances), and a span
        // between two finite bounds near the top of the range overflows to
        // infinity.
        if (!(pitch > 0.0) || !std::isfinite(pitch)) {
            throw std::invalid_argument(
                "volume grid region must have finite positive extent");
        }
        inversePitch[axis] = 1.0 / pitch;
    }
    const auto viewport = viewportFrame(width, height);
    const auto rays = rayField(settings.camera, viewport, settings.domain);
    // The span and centre checks above are necessary but not sufficient: a
    // domain near the top of the range combined with the smallest zoom still
    // scales a ray origin past what a double holds. Checking the field the
    // march actually uses covers every route to that, and does it once per
    // frame rather than per pixel.
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(rays.origin[axis]) || !std::isfinite(rays.direction[axis])
            || !std::isfinite(rays.perPixelX[axis])
            || !std::isfinite(rays.perPixelY[axis])) {
            throw std::invalid_argument(
                "the camera and domain do not give a usable ray for every pixel");
        }
    }
    // A position along a ray is only defined to about an ulp of its own
    // magnitude. The origins sit a couple of domain extents out, so a region
    // very much smaller than that has an entry and an exit parameter that
    // round to the same double: the slab test reports a miss and the frame
    // comes back empty, with no error and nothing to diagnose. The ratio has
    // to approach 1e16 before this bites -- a domain 1e16 wide holding a
    // unit-sized region -- which is a zoom no double-precision camera
    // normalised to that domain can express; ordinary zooms are many orders
    // short of it. Refusing says so; rendering nothing does not.
    double originMagnitude = 0.0;
    for (const double cornerX : {0.0, static_cast<double>(width)}) {
        for (const double cornerY : {0.0, static_cast<double>(height)}) {
            const auto corner = rayAt(rays, cornerX, cornerY);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                originMagnitude = std::max(
                    originMagnitude, std::abs(corner.origin[axis]));
            }
        }
    }
    // Scaled down before up: 8 * originMagnitude overflows to infinity for a
    // domain near the top of the range, and every region would then be
    // refused.
    const auto resolution = originMagnitude
        * std::numeric_limits<double>::epsilon() * 8.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!(grid.region.upper[axis] - grid.region.lower[axis] > resolution)) {
            throw std::invalid_argument(
                "the camera domain is too large beside the sampled region for the ray to resolve it");
        }
    }
    // The distance a ray travels to cross one voxel's worth of material: the
    // length of the direction measured in voxels, inverted. samplesPerVoxel
    // divides it, and the opacity correction in resolveEntries assumes it.
    //
    // Euclidean, not the sum of the per-axis crossing rates. The sum counts
    // voxels *entered*, which is a property of the grid's orientation rather
    // than of the material: a ray down the body diagonal of a cubic grid
    // enters three voxels per pitch of travel, so a uniform block would come
    // out as though it held 3x the material a face-on view sees, where the
    // physical path holds only sqrt(3)x. The norm below gives exactly the
    // pitch for any direction through a cubic grid, so the block's opacity
    // follows the distance light travels through it and nothing else.
    //
    // For an axis-aligned view the two agree, and both give that axis's own
    // pitch -- which is the point against using the *smallest* pitch: that
    // oversamples a coarse axis by the ratio between the two (an entry
    // authored at 0.1 composites to 0.97 on a 32:1 grid) and, for a region
    // far thinner than it is wide, steps the whole way across at the thin
    // axis's scale.
    // std::hypot rather than a hand-rolled sum of squares: the per-axis term
    // is a direction over a pitch, and squaring it underflows to zero for a
    // region whose pitch is enormous (a 1e307-wide box over four voxels) and
    // overflows for one whose pitch is tiny. hypot is exact where both are
    // representable and survives where the squares are not.
    const auto voxelsPerLength = std::hypot(
        rays.direction[0] * inversePitch[0],
        rays.direction[1] * inversePitch[1],
        rays.direction[2] * inversePitch[2]);
    const auto step = 1.0
        / (voxelsPerLength * static_cast<double>(settings.samplesPerVoxel));
    if (!(step > 0.0) || !std::isfinite(step)) {
        throw std::invalid_argument("the grid pitch does not give a usable sample step");
    }
    // A ray inside the box travels at most extent / |direction| along each
    // axis, and the norm above is at most the sum of the per-axis rates, so
    // the count below is at most samplesPerVoxel * sum(dims): the
    // ceiling is a backstop against rounding in that bound, not a working
    // limit. Reaching it needs an axis under the parallel threshold whose
    // pitch still dominates the norm; the march then stops early and the far
    // side of the volume is dropped -- not rendered coarsely. That is
    // acceptable only because the geometry that reaches it is already
    // degenerate; it is a guard against a march that will not end, not a
    // level-of-detail scheme.
    const auto sampleCeiling = static_cast<double>(settings.samplesPerVoxel)
        * (static_cast<double>(grid.dims[0]) + static_cast<double>(grid.dims[1])
            + static_cast<double>(grid.dims[2]))
        + 2.0;

    // Allocated only now: every refusal above is a property of the settings
    // and the grid, and the pixel buffer is 67 MB at the 4096x4096 cap. The
    // same cheapest-refusal-first argument the voxel budget makes.
    VolumeFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pixels.assign(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    frame.usedRange = settings.range;
    frame.metrics.gridDims = grid.dims;
    frame.metrics.coveredVoxels = grid.coveredVoxels;
    frame.metrics.sampledMaximumLevel = grid.maximumLevel;
    // Started here, not at entry: the validation above and the assign() just
    // made -- 67 MB of zeroed pixels at the output cap, a page-faulting
    // memset on a cold allocator -- are not ray marching, and a caller
    // comparing this against a server's would be comparing allocators.
    const auto started = std::chrono::steady_clock::now();
    const auto entries = resolveEntries(settings.transfer, settings.samplesPerVoxel);
    // At least two, by the transfer-function validator above: the march
    // indexes with valueSlot's result and does not re-check it.
    const auto entryCount = static_cast<int>(entries.size());
    const auto mappedRange = *mapping;
    // Front-to-back compositing stops once the ray is this opaque: whatever
    // lies behind can add at most 0.001 to any channel, a quarter of one
    // 8-bit level, so the pixel is what a full march would round to.
    constexpr double opaqueEnough = 0.999;
    const auto rowStride = static_cast<std::size_t>(width);
    const auto gridRowStride = static_cast<std::size_t>(grid.dims[0]);
    const auto slabStride = gridRowStride * static_cast<std::size_t>(grid.dims[1]);

    std::atomic<bool> cancelled{false};
    // Cancellation is polled inside the pixel loop *and* inside the sample
    // loop. Neither on its own is enough: polling only between rows leaves a
    // single-row render uninterruptible, and polling only between pixels
    // leaves a single ray uninterruptible -- a budget-legal grid of
    // {1, 1, 16777216} puts tens of millions of samples on one ray, which is
    // half a second of unpollable work for one pixel. Both strides keep the
    // atomic load off the per-item path while bounding the delay.
    constexpr int pixelCancellationStride = 64;
    constexpr std::int64_t sampleCancellationStride = 4096;
    const auto slabAxes = slabAxesFor(rays.direction);
    // One direction for the whole frame means one voxel-space step for the
    // whole frame; only where a ray enters the grid varies per pixel.
    std::array<double, 3> voxelStep{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        voxelStep[axis] = rays.direction[axis] * step * inversePitch[axis];
    }
    // Hoisted: one loop-invariant branch per sample rather than a policy read.
    const auto linear = settings.sampling == SamplingPolicy::Linear;
    // Trilinear over the eight voxel centres bracketing the sample.
    //
    // Voxel i is centred at i + 0.5 in this coordinate (Volume.hpp says so),
    // which is why the nearest rule above can just take the floor -- the half
    // voxel cancels. Interpolating has to put it back, or the picture moves
    // half a voxel and still looks entirely plausible.
    //
    // Outside the outermost centres the bracket collapses onto the edge voxel
    // and the weight goes to zero, so the outer half-voxel shell reads flat --
    // the same value the nearest rule gives there, which is what keeps a
    // uniform slab compositing to its analytic opacity rather than fading at
    // the boundary. A corner the range cannot map -- uncovered, or
    // non-positive under a logarithmic range -- takes the landed voxel's value
    // instead, the 3-D form of what the slice's bilinear sampler does at a
    // domain edge (SliceQuery.cpp): clamp the field to its coverage rather
    // than invent data, and never let one NaN erode a voxel-wide rind. Every
    // corner is then mappable, so the result is a convex combination of
    // mappable values and is mappable itself.
    // The eight corner values a sample interpolates, kept from one sample to
    // the next. At two or more samples per voxel a ray takes several steps
    // inside one cell -- four of them at the High preset -- and the corners do
    // not change while it does, so the fetches and the coverage tests are the
    // same work repeated. One of these lives per ray, not per frame, so the
    // march stays a pure function of the sample's index.
    //
    // Corners are kept as the grid holds them, uncovered ones included, with a
    // bit per corner saying which those were. Substitution happens on the way
    // out instead: what stands in for an uncovered corner is the value of the
    // voxel the sample landed in, and that changes within a cell, so a cell
    // stored already-substituted could not be reused across the sample that
    // changed it. This way every cell is reusable, boundaries included.
    struct CellCache {
        std::array<std::size_t, 3> low{};
        std::array<double, 8> corner{};
        unsigned uncovered = 0;
        bool loaded = false;
    };
    const auto linearValue
        = [&grid, &mappedRange, gridRowStride, slabStride](
              const std::array<double, 3>& at, double landed, CellCache& cache) {
              // Per axis: the two bracketing voxel indices and how far the
              // sample sits between them.
              std::array<std::array<std::size_t, 2>, 3> bracket{};
              std::array<double, 3> weight{};
              for (std::size_t axis = 0; axis < 3; ++axis) {
                  const auto centred = at[axis] - 0.5;
                  const auto limit = static_cast<double>(grid.dims[axis] - 1);
                  double low = 0.0;
                  double fraction = 0.0;
                  // Ordered so NaN takes the first branch's false arm, the way
                  // the clamp above does: it reads voxel zero rather than
                  // casting something undefined. A single-voxel axis has
                  // limit 0 and lands here too, with both ends on voxel zero.
                  if (centred > 0.0) {
                      if (centred < limit) {
                          low = std::floor(centred);
                          fraction = centred - low;
                      } else {
                          low = limit;
                      }
                  }
                  bracket[axis][0] = static_cast<std::size_t>(low);
                  bracket[axis][1]
                      = static_cast<std::size_t>(low < limit ? low + 1.0 : limit);
                  weight[axis] = fraction;
              }
              // The same cell as the last sample on this ray, and that one
              // needed no substitution: its corners still hold.
              // Read straight into the cache and interpolate out of it, so a
              // reused cell costs three comparisons and a fresh one costs no
              // copy on top of its fetches.
              if (!cache.loaded || cache.low[0] != bracket[0][0]
                  || cache.low[1] != bracket[1][0]
                  || cache.low[2] != bracket[2][0]) {
                  cache.uncovered = 0;
                  for (std::size_t k = 0; k < 2; ++k) {
                      for (std::size_t j = 0; j < 2; ++j) {
                          for (std::size_t i = 0; i < 2; ++i) {
                              const auto corner = i + 2 * j + 4 * k;
                              const auto offset = bracket[0][i]
                                  + gridRowStride * bracket[1][j]
                                  + slabStride * bracket[2][k];
                              const auto value
                                  = static_cast<double>(grid.values[offset]);
                              if (!mappableValue(value, mappedRange)) {
                                  cache.uncovered |= 1U << corner;
                              }
                              cache.corner[corner] = value;
                          }
                      }
                  }
                  cache.loaded = true;
                  cache.low = {bracket[0][0], bracket[1][0], bracket[2][0]};
              }
              // Seven interpolations along the axes in turn rather than
              // eight corners each weighted by a product: the same value, and
              // the weight products are what this loop spends its time on.
              // std::lerp rather than from + where * (to - from): it is
              // specified to return the endpoints exactly, which is what the
              // clamped shell above leans on when it claims to read flat.
              const auto between = [](double from, double to, double where) {
                  return std::lerp(from, to, where);
              };
              const auto blend = [&](const std::array<double, 8>& corner) {
                  const auto lowY
                      = between(between(corner[0], corner[1], weight[0]),
                          between(corner[2], corner[3], weight[0]), weight[1]);
                  const auto highY
                      = between(between(corner[4], corner[5], weight[0]),
                          between(corner[6], corner[7], weight[0]), weight[1]);
                  return between(lowY, highY, weight[2]);
              };
              if (cache.uncovered == 0) {
                  return blend(cache.corner);
              }
              // The uncommon path: a cell at the edge of what the levels
              // cover. The landed voxel stands in for the corners they do not.
              auto covered = cache.corner;
              for (std::size_t corner = 0; corner < 8; ++corner) {
                  if ((cache.uncovered & (1U << corner)) != 0) {
                      covered[corner] = landed;
                  }
              }
              return blend(covered);
          };

    const auto renderRow = [&](int row) {
        auto* pixels = frame.pixels.data() + rowStride * static_cast<std::size_t>(row);
        const auto pixelY = static_cast<double>(row) + 0.5;
        // The row's share of the origin, which the column loop would
        // otherwise rebuild per pixel. Still index-driven, so a row's pixels
        // do not depend on which thread renders them.
        Real3 rowOrigin;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            rowOrigin[axis] = rays.origin[axis] + pixelY * rays.perPixelY[axis];
        }
        for (int column = 0; column < width; ++column) {
            if (column % pixelCancellationStride == 0
                && (cancelled.load(std::memory_order_relaxed)
                    || cancellation.stop_requested())) {
                cancelled.store(true, std::memory_order_relaxed);
                return;
            }
            Ray ray;
            ray.direction = rays.direction;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                ray.origin[axis] = rowOrigin[axis]
                    + (static_cast<double>(column) + 0.5) * rays.perPixelX[axis];
            }
            double tEnter = 0.0;
            double tExit = 0.0;
            if (!clipToBox(ray, slabAxes, grid.region, tEnter, tExit)) {
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
            // The march runs in voxel coordinates rather than world ones:
            // converting the ray once per pixel leaves one multiply and one
            // add per axis per sample instead of the two multiplies, subtract
            // and add that going through world space costs. The sample index
            // still drives it, so the no-accumulation argument above holds.
            std::array<double, 3> voxelStart{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                voxelStart[axis] = (ray.origin[axis] + tEnter * ray.direction[axis]
                    - lower[axis]) * inversePitch[axis];
            }
            // Per ray: what the previous sample on this ray read.
            CellCache cell;
            double alpha = 0.0;
            double red = 0.0;
            double green = 0.0;
            double blue = 0.0;
            for (std::int64_t sample = 0; sample < sampleCount; ++sample) {
                if (sample % sampleCancellationStride == 0 && sample != 0
                    && (cancelled.load(std::memory_order_relaxed)
                        || cancellation.stop_requested())) {
                    cancelled.store(true, std::memory_order_relaxed);
                    return;
                }
                const auto offsetInSamples = static_cast<double>(sample) + 0.5;
                std::array<double, 3> at{};
                std::array<std::size_t, 3> index{};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    at[axis] = voxelStart[axis] + offsetInSamples * voxelStep[axis];
                    const auto voxel = std::floor(at[axis]);
                    // Clamped as a double, before the cast: casting a value
                    // past the int range is undefined, and std::clamp passes
                    // a NaN straight through to it. Both are reachable from a
                    // ray running nearly parallel to a very thin axis.
                    const auto limit = static_cast<double>(grid.dims[axis] - 1);
                    index[axis] = voxel > 0.0
                        ? static_cast<std::size_t>(voxel < limit ? voxel : limit)
                        : 0U;
                }
                const auto offset = index[0] + gridRowStride * index[1]
                    + slabStride * index[2];
                // The voxel the sample lands in decides whether there is a
                // sample at all. An uncovered one is not the field's to give
                // whatever its neighbours hold, so interpolation never fills
                // a hole in -- which is also what keeps an all-NaN grid
                // completely transparent.
                const auto nearest = static_cast<double>(grid.values[offset]);
                if (!mappableValue(nearest, mappedRange)) {
                    continue;
                }
                const auto value
                    = linear ? linearValue(at, nearest, cell) : nearest;
                const auto& entry = entries[static_cast<std::size_t>(
                    valueSlot(value, mappedRange, entryCount))];
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

    const auto threadCount = raycastThreadCount(settings.threadCount, height);
    std::exception_ptr failure;
    std::mutex failureMutex;
    // Rows are taken one at a time from a shared counter rather than dealt
    // out in contiguous bands. What a row costs varies enormously down the
    // frame -- rows that miss the domain return at clipToBox, rows through
    // the middle march the full depth, and an early-out ends a ray as soon as
    // it is opaque -- so a band of neighbouring rows is a band of similar
    // cost, and the thread holding the middle of the picture finishes long
    // after the ones holding the top and bottom. Handing out rows on demand
    // costs one relaxed increment each and lets every worker keep going until
    // the frame is done.
    //
    // The picture cannot change: renderRow is a pure function of its index
    // and writes only that row, so which worker takes it is not observable.
    std::atomic<int> nextRow{0};
    const auto renderShare = [&] {
        try {
            for (;;) {
                const auto row = nextRow.fetch_add(1, std::memory_order_relaxed);
                if (row >= height) {
                    return;
                }
                // No separate per-row poll: renderRow checks at column 0 of
                // every row and every stride within it, which subsumes one.
                renderRow(row);
                if (cancelled.load(std::memory_order_relaxed)) {
                    return;
                }
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
        renderShare();
    } else {
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(threadCount));
        try {
            for (int worker = 0; worker < threadCount; ++worker) {
                threads.emplace_back(renderShare);
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
