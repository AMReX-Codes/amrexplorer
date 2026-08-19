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

using detail::floatOverflowThreshold;
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
    // Native cell counts, capped only where int stops holding them. Capping
    // an axis at the budget first would be wrong: the cbrt below shrinks the
    // whole grid by one ratio, so an axis already truncated to the budget
    // gets shrunk a second time and a thin region ends up spending a
    // fraction of what it was given. The product of three uncapped axes
    // overflows, which is why product() saturates.
    constexpr auto ceiling = 1.0e9;
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
            ? static_cast<int>(std::clamp(cells, 1.0, ceiling))
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
    // A region reaching past the domain is not refused. The grid already
    // says "no data here" with NaN, and a slice over the same region simply
    // leaves those pixels uncovered, so refusing would fail a rubber-band
    // selection the slice renders. The voxel budget bounds the grid whatever
    // the region's size, so there is nothing to protect against here.
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

    // The slice resolves its pixels with this too, so a voxel and a pixel
    // over the same region land on the same cell rather than a rounding step
    // apart. It is now the only encoding of the voxel geometry: the block
    // windows below bisect on it instead of inverting it.
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
    // The finest level that put a showable value in the grid, which is what
    // the field reports -- not the level that was asked for. Negative until
    // one does, since an empty grid has no contributing level to name and
    // must not claim the finest one merely because it was requested.
    auto contributingLevel = -1;
    for (int levelIndex = minimumLevel; levelIndex <= maximumLevel; ++levelIndex) {
        const auto& level = metadata.levels[static_cast<std::size_t>(levelIndex)];
        // Which blocks can hold a voxel centre, asked of the centres rather
        // than of the region. Deriving it from the region means nudging its
        // upper edge inward by one ulp, which is a fraction of a cell near
        // the origin but more than a whole cell far from it -- and a block
        // holding only the last voxel's cell would then never be a
        // candidate at all, so no window would ever be computed for it.
        // sampleIndex(voxelCentre(v)) is non-decreasing in v, so the first
        // and last voxel bound every cell this grid can sample.
        auto queryBox = level.domain;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto onAxis = static_cast<int>(axis);
            queryBox.lower[axis] = sampleIndex(level, onAxis, voxelCentre(axis, 0));
            queryBox.upper[axis] = sampleIndex(
                level, onAxis, voxelCentre(axis, dims[axis] - 1));
        }
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
            // Searched, not inverted. Inverting the centre formula in
            // floating point costs an error of ulp(coordinate)/pitch voxels:
            // nothing near the origin, and tens of voxels for a finely
            // resolved region far from it, which no fixed widening covers.
            // sampleIndex(voxelCentre(v)) is non-decreasing in v, so the
            // voxels landing in this block form a contiguous run whose ends
            // can be bisected with the very predicate the composition is
            // defined by -- exact at any distance from the origin, and every
            // voxel in [first, last] is then known to be inside the block.
            std::array<int, 3> first{};
            std::array<int, 3> last{};
            auto empty = false;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto cellAt = [&](int voxel) {
                    return sampleIndex(level, static_cast<int>(axis),
                        voxelCentre(axis, voxel));
                };
                // The first voxel whose cell has reached the block's lower
                // edge, then the first past its upper edge.
                auto low = 0;
                auto high = dims[axis];
                while (low < high) {
                    const auto middle = low + (high - low) / 2;
                    if (cellAt(middle) < block.box.lower[axis]) {
                        low = middle + 1;
                    } else {
                        high = middle;
                    }
                }
                first[axis] = low;
                high = dims[axis];
                while (low < high) {
                    const auto middle = low + (high - low) / 2;
                    if (cellAt(middle) <= block.box.upper[axis]) {
                        low = middle + 1;
                    } else {
                        high = middle;
                    }
                }
                last[axis] = low - 1;
                if (last[axis] < first[axis]) {
                    empty = true;
                    break;
                }
                auto& indices = cellIndex[axis];
                indices.resize(static_cast<std::size_t>(last[axis] - first[axis] + 1));
                for (int voxel = first[axis]; voxel <= last[axis]; ++voxel) {
                    indices[static_cast<std::size_t>(voxel - first[axis])]
                        = cellAt(voxel);
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
                if ((k - first[2]) % 32 == 0 && cancellation.stop_requested()) {
                    throw ReadCancelled();
                }
                const auto cellK = cellIndex[2][static_cast<std::size_t>(k - first[2])];
                for (int j = first[1]; j <= last[1]; ++j) {
                    const auto cellJ = cellIndex[1][static_cast<std::size_t>(j - first[1])];
                    auto* row = grid.values.data() + slabStride * static_cast<std::size_t>(k)
                        + rowStride * static_cast<std::size_t>(j);
                    for (int i = first[0]; i <= last[0]; ++i) {
                        const auto cellI = cellIndex[0][static_cast<std::size_t>(i - first[0])];
                        Int3 cell;
                        cell[0] = cellI;
                        cell[1] = cellJ;
                        cell[2] = cellK;
                        const auto value = fab.values[valueOffset(fab.box, cell, 3)];
                        // Range-checked before the cast, not after: converting
                        // a double past the overflow threshold is undefined,
                        // and the grid promises NaN for what it cannot hold.
                        const auto storable = std::isfinite(value)
                            && std::fabs(value) < floatOverflowThreshold;
                        row[static_cast<std::size_t>(i)]
                            = storable ? static_cast<float>(value) : quietNaN;
                        // A NaN is indistinguishable from an uncovered voxel,
                        // so a level that wrote only those contributed
                        // nothing the grid can show.
                        levelPainted = levelPainted || storable;
                    }
                }
            }
        }
        if (levelPainted) {
            contributingLevel = levelIndex;
        }
    }
    grid.maximumLevel = std::max(contributingLevel, 0);

    grid.coveredVoxels = static_cast<std::uint64_t>(std::count_if(
        grid.values.begin(), grid.values.end(),
        [](float value) { return !std::isnan(value); }));
    return result;
}

} // namespace amrvis
