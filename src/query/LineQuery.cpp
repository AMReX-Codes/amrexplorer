#include <amrexplorer/query/LineQuery.hpp>
#include <amrexplorer/query/detail/BlockLookup.hpp>

#include <algorithm>
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

using detail::BlockGrid;
using detail::LoadedBlock;
using detail::intersects;
using detail::lookupBlockValue;
using detail::physicalToIndex;

// A covering cell hit during the line walk: which level won, the cell index,
// and the cell-centered value there.
struct Cover {
    int level = 0;
    Int3 point{};
    float value = 0.0F;
};



} // namespace

LineQueryResult LineQuery::execute(
    const LineRequest& request, StopToken cancellation)
{
    const auto& metadata = m_dataset.metadata();
    const auto errors = validateLineRequest(request, metadata.dimension);
    if (!errors.empty()) {
        throw std::invalid_argument(errors.front());
    }
    if (request.dataset != m_dataset.id()) {
        throw std::invalid_argument("line request targets a different dataset");
    }
    if (request.field.value >= metadata.fields.size()) {
        throw std::invalid_argument("line field is unavailable");
    }
    if (request.component != 0) {
        throw std::invalid_argument("the initial plotfile fields are scalar");
    }

    const auto maximumLevel = std::min(request.maximumLevel, metadata.finestLevel);
    const auto minimumLevel = request.composition == CompositionPolicy::ExactLevel
        ? maximumLevel : 0;
    const auto lineAxis = static_cast<std::size_t>(request.axis);
    const auto& samplingLevel = metadata.levels[static_cast<std::size_t>(maximumLevel)];

    // Physical extent of the line along its axis: the viewport region when the
    // caller supplied one (a subregion line plot), otherwise the maximumLevel's
    // full domain.
    const auto domainExtent = static_cast<std::int64_t>(samplingLevel.domain.upper[lineAxis])
        - samplingLevel.domain.lower[lineAxis] + 1;
    if (domainExtent <= 0
        || static_cast<std::uint64_t>(domainExtent)
            > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::overflow_error("line sample count exceeds addressable memory");
    }
    const auto bounds = sampleBounds(
        samplingLevel, samplingLevel.domain, metadata.dimension);
    const auto domainStart = bounds.lower[lineAxis];
    const auto domainEnd = bounds.upper[lineAxis];
    const auto physicalStart = request.region.has_value()
        ? request.region->lower[lineAxis] : domainStart;
    const auto physicalEnd = request.region.has_value()
        ? request.region->upper[lineAxis] : domainEnd;
    const auto finestCellSize = samplingLevel.cellSize[lineAxis];

    LineQueryResult result;
    result.line.axis = request.axis;
    result.line.positionsAreIndices = !metadata.hasPhysicalGeometry;
    const auto maxSamples = static_cast<std::size_t>(domainExtent);
    result.line.positions.reserve(maxSamples);
    result.line.values.reserve(maxSamples);
    result.line.valid.reserve(maxSamples);
    result.line.sourceLevel.reserve(maxSamples);

    // Pre-load every block the line crosses, per participating level, and index
    // each level's blocks for O(1)-average point lookup during the walk. The
    // grid bins on the line axis (the walk varies only that coordinate; the
    // off-axes are pinned to the line's fixed cell) plus one other axis to give
    // BlockGrid two distinct axes.
    std::vector<std::vector<LoadedBlock>> loadedByLevel(
        static_cast<std::size_t>(maximumLevel) + 1);
    std::vector<BlockGrid> gridByLevel(
        static_cast<std::size_t>(maximumLevel) + 1);
    const std::array<int, 2> gridAxes{
        request.axis, (request.axis + 1) % metadata.dimension};
    for (int levelIndex = minimumLevel; levelIndex <= maximumLevel; ++levelIndex) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto& level = metadata.levels[static_cast<std::size_t>(levelIndex)];
        auto lineBox = level.domain;
        for (int axis = 0; axis < metadata.dimension; ++axis) {
            if (axis == request.axis) {
                continue;
            }
            const auto i = static_cast<std::size_t>(axis);
            const auto index = physicalToIndex(
                request.fixedCoordinates[i], metadata, level, axis);
            lineBox.lower[i] = index;
            lineBox.upper[i] = index;
        }
        // ...and only the stretch of the line axis the caller asked about. The
        // walk below already honours request.region physically, but without
        // this the planning stayed full-width: a line plot zoomed into a small
        // fraction of the domain still read every block the full-domain line
        // crosses. Clamped in index space, at this level's resolution, so the
        // block intersection test is comparing like with like.
        if (request.region.has_value()) {
            const auto first = physicalToIndex(
                physicalStart, metadata, level, request.axis);
            const auto last = physicalToIndex(
                physicalEnd, metadata, level, request.axis);
            lineBox.lower[lineAxis] = std::max(
                lineBox.lower[lineAxis], std::min(first, last));
            lineBox.upper[lineAxis] = std::min(
                lineBox.upper[lineAxis], std::max(first, last));
            if (lineBox.lower[lineAxis] > lineBox.upper[lineAxis]) {
                continue;  // the region misses this level entirely
            }
        }
        auto& loaded = loadedByLevel[static_cast<std::size_t>(levelIndex)];
        for (std::size_t grid = 0; grid < level.blocks.size(); ++grid) {
            const auto& block = level.blocks[grid];
            if (!intersects(block.box, lineBox, metadata.dimension)) {
                continue;
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
            loaded.push_back({block.box, std::move(access.handle)});
        }
        gridByLevel[static_cast<std::size_t>(levelIndex)]
            = BlockGrid(loaded, gridAxes);
    }

    // Find the finest level covering position x (fine overrides coarse).
    const auto findCovering = [&](double x, Cover& cover) -> bool {
        for (int levelIndex = maximumLevel; levelIndex >= minimumLevel; --levelIndex) {
            const auto& level = metadata.levels[static_cast<std::size_t>(levelIndex)];
            Int3 point{};
            for (int axis = 0; axis < metadata.dimension; ++axis) {
                point[static_cast<std::size_t>(axis)] = physicalToIndex(
                    axis == request.axis
                        ? x
                        : request.fixedCoordinates[static_cast<std::size_t>(axis)],
                    metadata, level, axis);
            }
            const auto entry = static_cast<std::size_t>(levelIndex);
            if (const auto value = lookupBlockValue(
                    gridByLevel[entry], loadedByLevel[entry], point,
                    metadata.dimension)) {
                cover.level = levelIndex;
                cover.point = point;
                cover.value = static_cast<float>(*value);
                return true;
            }
        }
        return false;
    };

    // Walk the line at native resolution: emit the finest covering cell, then
    // step to its far boundary. One sample per actual cell means no flat coarse
    // steps, so a multi-level composite draws as a smooth line over non-uniform
    // points. Uncovered stretches (ExactLevel outside coverage, or out of
    // domain) become invalid samples so the polyline breaks there.
    auto x = physicalStart;
    // Relative to the cell size: an absolute 1e-9 epsilon truncates domains
    // whose physical extent is near that scale (micro/nano-scale SI-unit
    // geometries), dropping the tail samples or yielding an empty line with no
    // error.
    const double endEpsilon = 1e-9 * finestCellSize;
    while (x < physicalEnd - endEpsilon) {
        if ((result.line.positions.size() & 31U) == 0U
            && cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        Cover cover;
        if (findCovering(x, cover)) {
            const auto& level = metadata.levels[static_cast<std::size_t>(cover.level)];
            const auto center = samplePosition(
                level, request.axis, cover.point[lineAxis]);
            if (center >= physicalStart - endEpsilon && center <= physicalEnd + endEpsilon) {
                result.line.positions.push_back(metadata.hasPhysicalGeometry
                    ? center
                    : static_cast<double>(cover.point[lineAxis]));
                result.line.values.push_back(cover.value);
                result.line.valid.push_back(1);
                result.line.sourceLevel.push_back(static_cast<std::int16_t>(cover.level));
            }
            // Step just past the far boundary of this cell into the next one.
            // The step is half of the *finest* cell: small enough to never skip
            // a finer patch that begins at the boundary (it lands inside the
            // first finest cell past the edge), yet an absolute length that
            // clears one ULP of x for any addressable domain. A step scaled to
            // *this* cell instead -- the old 1e-9 * cellSize nudge -- falls below
            // an ULP of x once |x|/dx exceeds ~4.5e6: the addition rounds back,
            // floor re-derives the same cell, and the walk spins forever
            // (line-query-walk-stalls-at-large-offset).
            x = center + 0.5 * level.cellSize[lineAxis] + 0.5 * finestCellSize;
            // Hard progress guarantee for the pathological tail: if round-off
            // still failed to advance the derived index, jump to the next cell
            // center by index (exact in index space) so the loop cannot stall.
            if (physicalToIndex(x, metadata, level, request.axis)
                <= cover.point[lineAxis]) {
                x = samplePosition(level, request.axis, cover.point[lineAxis] + 1);
            }
        } else {
            Int3 point{};
            for (int axis = 0; axis < metadata.dimension; ++axis) {
                point[static_cast<std::size_t>(axis)] = physicalToIndex(
                    axis == request.axis
                        ? x
                        : request.fixedCoordinates[static_cast<std::size_t>(axis)],
                    metadata, samplingLevel, axis);
            }
            const auto center = samplePosition(
                samplingLevel, request.axis, point[lineAxis]);
            if (center >= physicalStart - endEpsilon && center <= physicalEnd + endEpsilon) {
                result.line.positions.push_back(metadata.hasPhysicalGeometry
                    ? center
                    : static_cast<double>(point[lineAxis]));
                result.line.values.push_back(0.0F);
                result.line.valid.push_back(0);
                result.line.sourceLevel.push_back(-1);
            }
            // Advance to the next finest cell center by index. This branch
            // always samples at the finest level, so index + 1 is exact and
            // immune to the large-|x|/dx stall described in the covered branch.
            x = samplePosition(samplingLevel, request.axis, point[lineAxis] + 1);
        }
    }
    return result;
}

} // namespace amrvis
