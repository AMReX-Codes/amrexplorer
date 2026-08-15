#include <amrexplorer/query/SliceQuery.hpp>
#include <amrexplorer/query/detail/BlockLookup.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace amrvis {
namespace {

using detail::LoadedBlock;
using detail::intersects;
using detail::contains;
using detail::physicalToIndex;
using detail::valueOffset;

// The blocks of one level that intersect the planning region. Levels are
// processed finest first so the composed per-point lookup resolves fine
// over coarse by construction.
struct LevelBlocks {
    int levelIndex = 0;
    std::vector<LoadedBlock> blocks;
};

// A uniform bin grid over one level's loaded blocks, on the two plane axes, for
// O(1)-average point->block lookup. The composited-value lookup below runs once
// per output pixel (five times per pixel for linear sampling), and a linear scan
// of every intersecting block per pixel was O(pixels * blocks) -- seconds on a
// full-resolution view of a block-heavy fine level. Blocks within an AMReX level
// are non-overlapping, so a point lands in at most one; the grid narrows the scan
// to the one tile the point falls in.
//
// A block that spans several tiles is listed in each, so every block covering a
// point shares that point's tile and the bucket scan sees all candidates. Scans
// run in ascending block index (buckets are filled in block order), so a
// malformed overlapping catalog resolves to the smallest index -- identical to
// the first-match order of the linear scan this replaces.
class BlockGrid {
public:
    BlockGrid(const std::vector<LoadedBlock>& blocks,
        const std::array<int, 2>& axes)
        : m_axis0(axes[0])
        , m_axis1(axes[1])
    {
        if (blocks.empty()) {
            return;
        }
        const auto a0 = static_cast<std::size_t>(m_axis0);
        const auto a1 = static_cast<std::size_t>(m_axis1);
        m_lo0 = std::numeric_limits<std::int64_t>::max();
        m_lo1 = std::numeric_limits<std::int64_t>::max();
        m_hi0 = std::numeric_limits<std::int64_t>::min();
        m_hi1 = std::numeric_limits<std::int64_t>::min();
        for (const auto& block : blocks) {
            m_lo0 = std::min(m_lo0,
                static_cast<std::int64_t>(block.validBox.lower[a0]));
            m_lo1 = std::min(m_lo1,
                static_cast<std::int64_t>(block.validBox.lower[a1]));
            m_hi0 = std::max(m_hi0,
                static_cast<std::int64_t>(block.validBox.upper[a0]));
            m_hi1 = std::max(m_hi1,
                static_cast<std::int64_t>(block.validBox.upper[a1]));
        }
        const auto span0 = m_hi0 - m_lo0 + 1;
        const auto span1 = m_hi1 - m_lo1 + 1;
        // ~sqrt(blocks) tiles per axis, capped so the bucket array is bounded.
        // llround(sqrt(n)) >= 1 for n >= 1 (the empty case returned above), so
        // only the upper cap is needed.
        constexpr std::int64_t maxTilesPerAxis = 256;
        const auto target = std::min<std::int64_t>(maxTilesPerAxis,
            std::llround(std::sqrt(static_cast<double>(blocks.size()))));
        m_tile0 = std::max<std::int64_t>(1, (span0 + target - 1) / target);
        m_tile1 = std::max<std::int64_t>(1, (span1 + target - 1) / target);
        m_n0 = static_cast<int>((span0 + m_tile0 - 1) / m_tile0);
        m_n1 = static_cast<int>((span1 + m_tile1 - 1) / m_tile1);
        const auto tileCount
            = static_cast<std::size_t>(m_n0) * static_cast<std::size_t>(m_n1);
        // Non-overlapping blocks (the level invariant) put ~one block in each
        // tile, so the fill is O(blocks + tiles). validateMetadata does not
        // forbid overlap, though, and a degenerate catalog of many large
        // overlapping boxes would push billions of (block, tile) entries -- an
        // unbounded, GB-scale allocation off a small header. Cap the total and
        // fall back to a plain linear scan (bounded memory, identical result)
        // rather than build an enormous index.
        const auto maxEntries = 8 * (blocks.size() + tileCount);
        m_buckets.assign(tileCount, {});
        std::size_t entries = 0;
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const auto& box = blocks[index].validBox;
            // Box coordinates are within [m_lo, m_hi], so these tile indices
            // never overflow the narrowing cast in tile0()/tile1(); the loop
            // bounds are hoisted out of the inner condition.
            const auto t0Last = tile0(box.upper[a0]);
            const auto t1Last = tile1(box.upper[a1]);
            for (int t1 = tile1(box.lower[a1]); t1 <= t1Last; ++t1) {
                for (int t0 = tile0(box.lower[a0]); t0 <= t0Last; ++t0) {
                    if (++entries > maxEntries) {
                        m_buckets.clear();
                        m_buckets.shrink_to_fit();
                        m_linearScan = true;
                        return;
                    }
                    m_buckets[bucket(t0, t1)].push_back(static_cast<int>(index));
                }
            }
        }
    }

    // Index into `blocks` of the block containing `point`, or -1 if none does.
    // `blocks` must be the same vector the grid was built from.
    [[nodiscard]] int find(const std::vector<LoadedBlock>& blocks,
        const Int3& point, int dimension) const
    {
        if (m_linearScan) {
            for (std::size_t index = 0; index < blocks.size(); ++index) {
                if (contains(blocks[index].validBox, point, dimension)) {
                    return static_cast<int>(index);
                }
            }
            return -1;
        }
        const auto p0 = static_cast<std::int64_t>(
            point[static_cast<std::size_t>(m_axis0)]);
        const auto p1 = static_cast<std::int64_t>(
            point[static_cast<std::size_t>(m_axis1)]);
        // Reject out-of-bounds points in int64 *before* tiling. A point far
        // above the bounding box would otherwise overflow the narrowing cast
        // in tile0()/tile1() to a negative int and slip past a bare upper-tile
        // check, indexing m_buckets wildly out of range. The empty-level case
        // (inverted default bounds, m_hi < m_lo) is rejected here too.
        if (p0 < m_lo0 || p0 > m_hi0 || p1 < m_lo1 || p1 > m_hi1) {
            return -1;
        }
        for (const auto index : m_buckets[bucket(tile0(p0), tile1(p1))]) {
            if (contains(blocks[static_cast<std::size_t>(index)].validBox,
                    point, dimension)) {
                return index;
            }
        }
        return -1;
    }

private:
    // Precondition: coordinate is within [m_lo, m_hi] on its axis, so the
    // quotient is in [0, m_n - 1] and the narrowing cast cannot overflow.
    [[nodiscard]] int tile0(std::int64_t coordinate) const noexcept
    {
        return static_cast<int>((coordinate - m_lo0) / m_tile0);
    }
    [[nodiscard]] int tile1(std::int64_t coordinate) const noexcept
    {
        return static_cast<int>((coordinate - m_lo1) / m_tile1);
    }
    [[nodiscard]] std::size_t bucket(int t0, int t1) const noexcept
    {
        return static_cast<std::size_t>(t1) * static_cast<std::size_t>(m_n0)
            + static_cast<std::size_t>(t0);
    }

    int m_axis0 = 0;
    int m_axis1 = 1;
    // Inverted default range so an empty grid rejects every point in find().
    std::int64_t m_lo0 = 0;
    std::int64_t m_lo1 = 0;
    std::int64_t m_hi0 = -1;
    std::int64_t m_hi1 = -1;
    std::int64_t m_tile0 = 1;
    std::int64_t m_tile1 = 1;
    int m_n0 = 0;
    int m_n1 = 0;
    bool m_linearScan = false;
    std::vector<std::vector<int>> m_buckets;
};


// The index box a physical region covers at one level. Piecewise sampling
// passes the visible region; linear sampling passes a halo-expanded region
// so the blocks of the bracketing cell centers are loaded too.
IntBox requestIndexBox(const RealBox& region, const SliceRequest& request,
    const DatasetMetadata& metadata, const LevelMetadata& level,
    const std::array<int, 2>& axes)
{
    auto result = level.domain;
    for (const auto axis : axes) {
        const auto i = static_cast<std::size_t>(axis);
        result.lower[i] = physicalToIndex(
            region.lower[i], metadata, level, axis);
        result.upper[i] = physicalToIndex(
            std::nextafter(region.upper[i],
                -std::numeric_limits<double>::infinity()),
            metadata, level, axis);
    }
    if (metadata.dimension == 3) {
        const auto normal = static_cast<std::size_t>(request.normalDirection);
        const auto index = physicalToIndex(
            request.physicalPosition, metadata, level, request.normalDirection);
        result.lower[normal] = index;
        result.upper[normal] = index;
    }
    return result;
}

} // namespace

SliceQueryResult SliceQuery::execute(
    const SliceRequest& request, StopToken cancellation)
{
    const auto& metadata = m_dataset.metadata();
    const auto errors = validateSliceRequest(request, metadata.dimension);
    if (!errors.empty()) {
        throw std::invalid_argument(errors.front());
    }
    if (request.dataset != m_dataset.id()) {
        throw std::invalid_argument("slice request targets a different dataset");
    }
    if (request.field.value >= metadata.fields.size()) {
        throw std::invalid_argument("slice field is unavailable");
    }
    if (request.component != 0) {
        throw std::invalid_argument("the initial plotfile fields are scalar");
    }

    const auto width = static_cast<std::uint64_t>(request.outputSize[0]);
    const auto height = static_cast<std::uint64_t>(request.outputSize[1]);
    if (height != 0 && width > std::numeric_limits<std::size_t>::max() / height) {
        throw std::overflow_error("slice output dimensions exceed addressable memory");
    }
    const auto pixelCount = static_cast<std::size_t>(width * height);

    SliceQueryResult result;
    result.plane.width = request.outputSize[0];
    result.plane.height = request.outputSize[1];
    result.plane.physicalRegion = request.visibleRegion;
    result.plane.values.assign(pixelCount, 0.0F);
    result.plane.valid.assign(pixelCount, 0);
    result.plane.sourceLevel.assign(pixelCount, -1);

    const auto axes = planeAxes(metadata.dimension, request.normalDirection);
    const auto maximumLevel = std::min(request.maximumLevel, metadata.finestLevel);
    const auto minimumLevel = request.composition == CompositionPolicy::ExactLevel
        ? maximumLevel : 0;

    // Block planning region. Linear sampling interpolates between the cell
    // centers bracketing each pixel, which can sit up to one covering-level
    // cell outside the visible region, so its plan adds a one-cell halo of
    // the coarsest participating level (the largest those cells can be).
    // Piecewise sampling reads exactly the cells the pixel centers land in.
    auto planningRegion = request.visibleRegion;
    if (request.sampling == SamplingPolicy::Linear) {
        const auto& coarsest =
            metadata.levels[static_cast<std::size_t>(minimumLevel)];
        for (const auto axis : axes) {
            const auto i = static_cast<std::size_t>(axis);
            planningRegion.lower[i] -= coarsest.cellSize[i];
            planningRegion.upper[i] += coarsest.cellSize[i];
        }
    }

    std::vector<LevelBlocks> levels;
    result.gridBoxesIncluded = request.includeGridBoxes;
    auto collectGridBoxes = request.includeGridBoxes;
    for (int levelIndex = maximumLevel; levelIndex >= minimumLevel; --levelIndex) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto& level = metadata.levels[static_cast<std::size_t>(levelIndex)];
        const auto queryBox = requestIndexBox(
            planningRegion, request, metadata, level, axes);
        LevelBlocks levelBlocks{levelIndex, {}};
        for (std::size_t grid = 0; grid < level.blocks.size(); ++grid) {
            const auto& block = level.blocks[grid];
            if (!intersects(block.box, queryBox, metadata.dimension)) {
                continue;
            }
            if (collectGridBoxes) {
                auto physicalBox = sampleBounds(
                    level, block.box, metadata.dimension);
                bool intersectsSlice = true;
                if (metadata.dimension == 3) {
                    const auto normal
                        = static_cast<std::size_t>(request.normalDirection);
                    intersectsSlice = request.physicalPosition
                        >= physicalBox.lower[normal]
                        && request.physicalPosition < physicalBox.upper[normal];
                }
                if (intersectsSlice) {
                    auto nondegenerate = true;
                    for (const auto axis : axes) {
                        const auto index = static_cast<std::size_t>(axis);
                        physicalBox.lower[index] = std::max(
                            physicalBox.lower[index],
                            request.visibleRegion.lower[index]);
                        physicalBox.upper[index] = std::min(
                            physicalBox.upper[index],
                            request.visibleRegion.upper[index]);
                        nondegenerate = nondegenerate
                            && physicalBox.lower[index]
                                < physicalBox.upper[index];
                    }
                    if (nondegenerate) {
                        if (result.gridBoxes.size()
                            >= request.maximumGridBoxes) {
                            result.gridBoxesTruncated = true;
                            collectGridBoxes = false;
                        } else {
                            result.gridBoxes.push_back(
                                SliceGridBox{levelIndex, physicalBox});
                        }
                    }
                }
            }
            ++result.metrics.candidateBlocks;
            BlockRequest blockRequest;
            blockRequest.dataset = request.dataset;
            blockRequest.level = levelIndex;
            blockRequest.gridIndex = static_cast<int>(grid);
            blockRequest.field = request.field;
            auto access = m_dataset.requestBlock(blockRequest, cancellation);
            if (access.cacheHit) {
                ++result.metrics.cacheHits;
            } else {
                ++result.metrics.blocksRead;
                result.metrics.payloadBytesRead += access.io.bytesRead;
            }
            levelBlocks.blocks.push_back({block.box, std::move(access.handle)});
        }
        levels.push_back(std::move(levelBlocks));
    }

    // One point->block index per participating level, parallel to `levels`.
    std::vector<BlockGrid> levelGrids;
    levelGrids.reserve(levels.size());
    for (const auto& levelBlocks : levels) {
        levelGrids.emplace_back(levelBlocks.blocks, axes);
    }

    // The composed piecewise-constant field at a physical point: the finest
    // participating level with a block covering the point's cell wins.
    // Returns the value and the covering level, or nothing when the point
    // is outside every grid.
    const auto valueAt = [&metadata, &levels, &levelGrids](const Real3& position)
        -> std::optional<std::pair<double, int>> {
        for (std::size_t entry = 0; entry < levels.size(); ++entry) {
            const auto& levelBlocks = levels[entry];
            const auto& level =
                metadata.levels[static_cast<std::size_t>(levelBlocks.levelIndex)];
            Int3 point;
            for (int axis = 0; axis < metadata.dimension; ++axis) {
                point[static_cast<std::size_t>(axis)] = physicalToIndex(
                    position[static_cast<std::size_t>(axis)], metadata, level, axis);
            }
            const auto blockIndex = levelGrids[entry].find(
                levelBlocks.blocks, point, metadata.dimension);
            if (blockIndex < 0) {
                continue;
            }
            const auto& block =
                levelBlocks.blocks[static_cast<std::size_t>(blockIndex)];
            const auto offset =
                valueOffset(block.data->box, point, metadata.dimension);
            if (offset >= block.data->values.size()) {
                throw std::runtime_error("composed FAB index exceeds loaded block");
            }
            return std::pair{block.data->values[offset], levelBlocks.levelIndex};
        }
        return std::nullopt;
    };

    // Bilinear interpolation of the composed field at the four sample
    // positions bracketing the position on its covering level.  Each corner
    // is evaluated with the composed lookup, so samples blend fine and coarse
    // values and stay smooth across AMR boundaries, and a globally linear
    // field is reproduced exactly at interior pixels.
    const auto linearSample = [&metadata, &axes, &valueAt](const Real3& position)
        -> std::optional<std::pair<double, int>> {
        const auto own = valueAt(position);
        if (!own) {
            return std::nullopt;
        }
        const auto coveringLevel = own->second;
        const auto& level =
            metadata.levels[static_cast<std::size_t>(coveringLevel)];

        // Bracketing sample coordinates, interpolation weight, and the
        // bracket slot containing the position, per plane axis.
        std::array<std::array<double, 2>, 2> centers{};
        std::array<double, 2> weights{};
        std::array<int, 2> ownIndex{};
        for (std::size_t planeAxis = 0; planeAxis < 2; ++planeAxis) {
            const auto axis = static_cast<std::size_t>(axes[planeAxis]);
            const auto cellSize = level.cellSize[axis];
            const auto ownSample = sampleIndex(
                level, static_cast<int>(axis), position[axis]);
            const auto ownCenter = samplePosition(
                level, static_cast<int>(axis), ownSample);
            const auto low =
                position[axis] < ownCenter ? ownSample - 1 : ownSample;
            centers[planeAxis][0] = samplePosition(
                level, static_cast<int>(axis), low);
            centers[planeAxis][1] = samplePosition(
                level, static_cast<int>(axis), low + 1);
            weights[planeAxis] =
                (position[axis] - centers[planeAxis][0]) / cellSize;
            ownIndex[planeAxis] = position[axis] < ownCenter ? 1 : 0;
        }

        // Corner samples at the bracketing positions (sharing the position's
        // normal coordinate in 3-D). A corner outside every grid takes the
        // nearest covered corner's value — x-aligned first, then y-aligned,
        // then the position's own sample — clamping the field to the domain
        // edge instead of inventing data.
        std::array<std::array<double, 2>, 2> corner{};
        std::array<std::array<bool, 2>, 2> covered{};
        for (std::size_t xSide = 0; xSide < 2; ++xSide) {
            for (std::size_t ySide = 0; ySide < 2; ++ySide) {
                Real3 point = position;
                point[static_cast<std::size_t>(axes[0])] = centers[0][xSide];
                point[static_cast<std::size_t>(axes[1])] = centers[1][ySide];
                const auto sample = valueAt(point);
                covered[xSide][ySide] = sample.has_value();
                corner[xSide][ySide] = sample ? sample->first : 0.0;
            }
        }
        const auto ownX = static_cast<std::size_t>(ownIndex[0]);
        const auto ownY = static_cast<std::size_t>(ownIndex[1]);
        for (std::size_t xSide = 0; xSide < 2; ++xSide) {
            for (std::size_t ySide = 0; ySide < 2; ++ySide) {
                if (covered[xSide][ySide]) {
                    continue;
                }
                if (covered[ownX][ySide]) {
                    corner[xSide][ySide] = corner[ownX][ySide];
                } else if (covered[xSide][ownY]) {
                    corner[xSide][ySide] = corner[xSide][ownY];
                } else if (covered[ownX][ownY]) {
                    corner[xSide][ySide] = corner[ownX][ownY];
                } else {
                    corner[xSide][ySide] = own->first;
                }
            }
        }

        const auto xWeight = weights[0];
        const auto yWeight = weights[1];
        const auto value = (1.0 - xWeight) * (1.0 - yWeight) * corner[0][0]
            + xWeight * (1.0 - yWeight) * corner[1][0]
            + (1.0 - xWeight) * yWeight * corner[0][1]
            + xWeight * yWeight * corner[1][1];
        return std::pair{value, coveringLevel};
    };

    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto linear = request.sampling == SamplingPolicy::Linear;
    for (int outputY = 0; outputY < request.outputSize[1]; ++outputY) {
        if ((outputY & 31) == 0 && cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        for (int outputX = 0; outputX < request.outputSize[0]; ++outputX) {
            const auto output = static_cast<std::size_t>(outputX)
                + static_cast<std::size_t>(request.outputSize[0])
                    * static_cast<std::size_t>(outputY);

            Real3 position;
            position[xAxis] = request.visibleRegion.lower[xAxis]
                + (static_cast<double>(outputX) + 0.5)
                    * (request.visibleRegion.upper[xAxis]
                        - request.visibleRegion.lower[xAxis])
                    / static_cast<double>(request.outputSize[0]);
            position[yAxis] = request.visibleRegion.lower[yAxis]
                + (static_cast<double>(outputY) + 0.5)
                    * (request.visibleRegion.upper[yAxis]
                        - request.visibleRegion.lower[yAxis])
                    / static_cast<double>(request.outputSize[1]);
            if (metadata.dimension == 3) {
                position[static_cast<std::size_t>(request.normalDirection)] =
                    request.physicalPosition;
            }

            const auto sample = linear ? linearSample(position) : valueAt(position);
            if (!sample) {
                continue;
            }
            result.plane.values[output] = static_cast<float>(sample->first);
            result.plane.valid[output] = 1;
            result.plane.sourceLevel[output] =
                static_cast<std::int16_t>(sample->second);
        }
    }
    return result;
}

} // namespace amrvis
