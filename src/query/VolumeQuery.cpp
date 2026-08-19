#include <amrexplorer/query/VolumeQuery.hpp>
#include <amrexplorer/query/detail/BlockLookup.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace amrvis {
namespace {

using detail::indexRangeOnAxis;
using detail::intersects;
using detail::requireBlockPayload;
using detail::sampleCentre;
using detail::valueOffset;

constexpr auto quietNaN = std::numeric_limits<float>::quiet_NaN();

// Saturating: a level large enough to overflow the product would otherwise
// wrap past the budget test in volumeGridDims and pass as if it fitted.
std::uint64_t product(const std::array<int, 3>& dims)
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

// The same product in double: no overflow, so the scaling below shrinks by
// the true ratio even when the integer product has saturated.
double realProduct(const std::array<int, 3>& dims)
{
    return static_cast<double>(dims[0]) * static_cast<double>(dims[1])
        * static_cast<double>(dims[2]);
}

// The cell index a voxel centre falls in, when the centre lands outside the
// block being painted. Out of band rather than -1: a level's index domain may
// start below zero, and those cells are as paintable as any other.
constexpr int noCell = std::numeric_limits<int>::min();

// The index box `region` covers at `level`, on the same half-open convention
// the slice and line planners use.
IntBox regionIndexBox(const RealBox& region, const DatasetMetadata& metadata,
    const LevelMetadata& level)
{
    auto result = level.domain;
    for (int axis = 0; axis < 3; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto [first, last] = indexRangeOnAxis(
            region.lower[i], region.upper[i], metadata, level, axis);
        result.lower[i] = first;
        result.upper[i] = last;
    }
    return result;
}

} // namespace

std::array<int, 3> volumeGridDims(const DatasetMetadata& metadata,
    const RealBox& region, int maximumLevel, std::uint64_t maximumVoxels)
{
    if (metadata.levels.empty()) {
        return {1, 1, 1};
    }
    const auto& level = metadata.levels[static_cast<std::size_t>(
        std::clamp(maximumLevel, 0, metadata.finestLevel))];
    // Bounded here, not by the caller: a server sizes an inbound request with
    // this before it has validated one, so an unchecked budget must not buy
    // a grid larger than the sampler would ever allocate.
    const auto budget = std::clamp<std::uint64_t>(
        maximumVoxels, 1, maxVolumeVoxelBudget);
    // Native cell counts, capped per axis by the whole budget: a thin region
    // may legitimately spend all of it on one axis. The product of three such
    // axes can overflow, which is why product() saturates.
    const auto ceiling = static_cast<double>(budget);
    // Only the axes the dataset has: a 2-D level's third cellSize is the
    // synthetic 1.0, which would invent a third dimension out of it.
    const auto sampled = static_cast<std::size_t>(
        std::clamp(metadata.dimension, 1, 3));
    std::array<int, 3> dims{1, 1, 1};
    for (std::size_t axis = 0; axis < sampled; ++axis) {
        const auto extent = region.upper[axis] - region.lower[axis];
        const auto cells = std::round(extent / level.cellSize[axis]);
        // A non-finite region has no cell count to round to, and clamp cannot
        // reject a NaN -- both of its comparisons are false -- so the cast
        // would be undefined. One voxel is the honest answer.
        dims[axis] = std::isfinite(cells)
            ? static_cast<int>(
                std::clamp(cells, 1.0, std::min(ceiling, 1.0e9)))
            : 1;
    }
    if (product(dims) > budget) {
        const auto scale = std::cbrt(static_cast<double>(budget)
            / realProduct(dims));
        for (auto& extent : dims) {
            // The nudge keeps an exact ratio (64 -> 8 is exactly 1/2 per
            // axis) from flooring to the size below when cbrt lands a hair
            // under. It is absolute, so it does nothing once an axis exceeds
            // ~8.4e6 and an ulp is the larger of the two; the trim below is
            // what keeps the result within budget either way.
            extent = std::max(1, static_cast<int>(
                std::floor(static_cast<double>(extent) * scale + 1.0e-9)));
        }
        // Rounding can leave the product a hair over: trim the largest axis.
        while (product(dims) > budget) {
            auto& largest = *std::max_element(dims.begin(), dims.end());
            if (largest <= 1) {
                break;
            }
            --largest;
        }
    }
    return dims;
}

VolumeQueryResult VolumeQuery::execute(
    const VolumeSampleRequest& request, StopToken cancellation)
{
    const auto& metadata = m_dataset.metadata();
    const auto errors = validateVolumeSampleRequest(request, metadata.dimension);
    if (!errors.empty()) {
        throw std::invalid_argument(errors.front());
    }
    if (request.dataset != m_dataset.id()) {
        throw std::invalid_argument("volume request targets a different dataset");
    }
    if (request.field.value >= metadata.fields.size()) {
        throw std::invalid_argument("volume field is unavailable");
    }
    if (request.component != 0) {
        throw std::invalid_argument("the initial plotfile fields are scalar");
    }
    // The region must lie within the domain (a hair of tolerance for a
    // region computed from the domain itself).
    const auto domain = datasetSampleBounds(metadata);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto tolerance = 1.0e-9
            * (domain.upper[axis] - domain.lower[axis]);
        if (request.region.lower[axis] < domain.lower[axis] - tolerance
            || request.region.upper[axis] > domain.upper[axis] + tolerance) {
            throw std::invalid_argument("volume region lies outside the dataset");
        }
    }

    const auto maximumLevel = std::min(request.maximumLevel, metadata.finestLevel);
    const auto minimumLevel = request.composition == CompositionPolicy::ExactLevel
        ? maximumLevel : 0;
    const auto dims = volumeGridDims(
        metadata, request.region, maximumLevel, request.maximumVoxels);

    VolumeQueryResult result;
    auto& grid = result.grid;
    grid.dims = dims;
    grid.region = request.region;
    grid.values.assign(static_cast<std::size_t>(product(dims)), quietNaN);

    std::array<double, 3> pitch{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        pitch[axis] = (request.region.upper[axis] - request.region.lower[axis])
            / static_cast<double>(dims[axis]);
    }
    // The slice resolves its pixels with this too, so a voxel and a pixel
    // over the same region land on the same cell rather than a rounding step
    // apart. `pitch` only brackets the candidate range below.
    const auto voxelCentre = [&](std::size_t axis, int index) {
        return sampleCentre(request.region.lower[axis],
            request.region.upper[axis], index, dims[axis]);
    };

    // Coarse to fine, so a finer level's cells overwrite the coarser ones
    // beneath them; within a level the grids run backwards, so on a
    // (malformed) overlap the lowest grid index wins, as the slice lookup's
    // first match does.
    // Reused by every block: bounded by dims, so after the first block the
    // resize is a no-op rather than three allocations per candidate.
    std::array<std::vector<int>, 3> cellIndex;
    // The finest level that put a value in the grid, which is what the field
    // reports -- not the level that was asked for. Nothing painted means no
    // level was finer than the coarsest that took part.
    auto contributingLevel = minimumLevel;
    for (int levelIndex = minimumLevel; levelIndex <= maximumLevel; ++levelIndex) {
        const auto& level = metadata.levels[static_cast<std::size_t>(levelIndex)];
        const auto queryBox = regionIndexBox(request.region, metadata, level);
        auto levelPainted = false;
        for (std::size_t reverse = level.blocks.size(); reverse > 0; --reverse) {
            if (cancellation.stop_requested()) {
                throw ReadCancelled();
            }
            const auto gridIndex = reverse - 1;
            const auto& block = level.blocks[gridIndex];
            if (!intersects(block.box, queryBox, 3)) {
                continue;
            }
            ++result.metrics.candidateBlocks;

            // Which voxels this block can paint, settled before it is read:
            // the range needs the box and the grid, not the payload, and on a
            // budget-scaled grid most blocks of a fine level hold no voxel
            // centre at all. Reading those would evict blocks that do.
            //
            // The voxels whose centres fall inside the block's cells: per
            // axis, the index range of centres within the block's physical
            // bounds, and each centre's cell index (or noCell when the centre
            // lands outside the valid box).
            //
            // The range is inverted from the centre formula in floating point,
            // so it can land a voxel off either end; it is widened by one per
            // side and sampleIndex below decides, since that -- not the
            // interval -- is what the composition is defined by. Too wide only
            // costs a rejected candidate, while too narrow drops a voxel the
            // block covers and no other block will paint.
            const auto bounds = sampleBounds(level, block.box, 3);
            std::array<int, 3> first{};
            std::array<int, 3> last{};
            auto empty = false;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto lower = (bounds.lower[axis] - request.region.lower[axis])
                    / pitch[axis] - 0.5;
                const auto upper = (std::nextafter(bounds.upper[axis],
                        -std::numeric_limits<double>::infinity())
                    - request.region.lower[axis]) / pitch[axis] - 0.5;
                first[axis] = static_cast<int>(
                    std::clamp(std::ceil(lower) - 1.0, 0.0,
                        static_cast<double>(dims[axis])));
                last[axis] = static_cast<int>(
                    std::clamp(std::floor(upper) + 1.0, -1.0,
                        static_cast<double>(dims[axis] - 1)));
                if (last[axis] < first[axis]) {
                    empty = true;
                    break;
                }
                auto& indices = cellIndex[axis];
                indices.resize(static_cast<std::size_t>(last[axis] - first[axis] + 1));
                auto any = false;
                for (int voxel = first[axis]; voxel <= last[axis]; ++voxel) {
                    const auto cell = sampleIndex(level, static_cast<int>(axis),
                        voxelCentre(axis, voxel));
                    const auto inside = cell >= block.box.lower[axis]
                        && cell <= block.box.upper[axis];
                    indices[static_cast<std::size_t>(voxel - first[axis])]
                        = inside ? cell : noCell;
                    any = any || inside;
                }
                // Widening the range costs a candidate per side, and on a
                // coarse grid the whole widened range can miss the block. No
                // index on an axis means no voxel to paint, so the read is
                // skipped rather than charged and thrown away.
                if (!any) {
                    empty = true;
                    break;
                }
            }
            if (empty) {
                continue;
            }

            BlockRequest blockRequest;
            blockRequest.dataset = request.dataset;
            blockRequest.level = levelIndex;
            blockRequest.gridIndex = static_cast<int>(gridIndex);
            blockRequest.field = request.field;
            auto access = m_dataset.requestBlock(blockRequest, cancellation);
            if (access.cacheHit) {
                ++result.metrics.cacheHits;
            } else {
                ++result.metrics.blocksRead;
                result.metrics.payloadBytesRead += access.io.bytesRead;
            }
            const auto& fab = *access.handle;
            requireBlockPayload(fab, block.box, 3);

            const auto rowStride = static_cast<std::size_t>(dims[0]);
            const auto slabStride = rowStride * static_cast<std::size_t>(dims[1]);
            for (int k = first[2]; k <= last[2]; ++k) {
                if ((k - first[2]) % 32 == 31 && cancellation.stop_requested()) {
                    throw ReadCancelled();
                }
                const auto cellK = cellIndex[2][static_cast<std::size_t>(k - first[2])];
                if (cellK == noCell) {
                    continue;
                }
                for (int j = first[1]; j <= last[1]; ++j) {
                    const auto cellJ = cellIndex[1][static_cast<std::size_t>(j - first[1])];
                    if (cellJ == noCell) {
                        continue;
                    }
                    auto* row = grid.values.data() + slabStride * static_cast<std::size_t>(k)
                        + rowStride * static_cast<std::size_t>(j);
                    for (int i = first[0]; i <= last[0]; ++i) {
                        const auto cellI = cellIndex[0][static_cast<std::size_t>(i - first[0])];
                        if (cellI == noCell) {
                            continue;
                        }
                        Int3 cell;
                        cell[0] = cellI;
                        cell[1] = cellJ;
                        cell[2] = cellK;
                        const auto value = fab.values[valueOffset(fab.box, cell, 3)];
                        // Range-checked before the cast, not after: converting
                        // a double beyond float's range is undefined, and the
                        // grid promises NaN for every value it cannot hold.
                        row[static_cast<std::size_t>(i)] = std::isfinite(value)
                                && std::fabs(value) <= static_cast<double>(
                                    std::numeric_limits<float>::max())
                            ? static_cast<float>(value) : quietNaN;
                        levelPainted = true;
                    }
                }
            }
        }
        if (levelPainted) {
            contributingLevel = levelIndex;
        }
    }
    grid.maximumLevel = contributingLevel;

    grid.coveredVoxels = static_cast<std::uint64_t>(std::count_if(
        grid.values.begin(), grid.values.end(),
        [](float value) { return !std::isnan(value); }));
    return result;
}

} // namespace amrvis
