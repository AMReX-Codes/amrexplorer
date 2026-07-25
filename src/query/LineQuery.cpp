#include <amrexplorer/query/LineQuery.hpp>

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

struct LoadedBlock {
    IntBox validBox;
    PlotfileDataset::BlockCache::Handle data;
};

// A covering cell hit during the line walk: which level won, the cell index,
// and the cell-centered value there.
struct Cover {
    int level = 0;
    Int3 point{};
    float value = 0.0F;
};

bool intersects(const IntBox& left, const IntBox& right, int dimension)
{
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        if (left.upper[i] < right.lower[i] || right.upper[i] < left.lower[i]) {
            return false;
        }
    }
    return true;
}

bool contains(const IntBox& box, const Int3& point, int dimension)
{
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        if (point[i] < box.lower[i] || point[i] > box.upper[i]) {
            return false;
        }
    }
    return true;
}

int physicalToIndex(double position, const DatasetMetadata& metadata,
    const LevelMetadata& level, int axis)
{
    (void) metadata;
    return sampleIndex(level, axis, position);
}

std::size_t valueOffset(const IntBox& box, const Int3& point, int dimension)
{
    const auto extent = [&box](std::size_t axis) {
        const auto value = static_cast<std::int64_t>(box.upper[axis])
            - box.lower[axis] + 1;
        if (value <= 0) {
            throw std::overflow_error("FAB extent is not positive");
        }
        return static_cast<std::uint64_t>(value);
    };
    const auto relative = [&box, &point](std::size_t axis) {
        const auto value = static_cast<std::int64_t>(point[axis]) - box.lower[axis];
        if (value < 0) {
            throw std::overflow_error("FAB point precedes its indexed box");
        }
        return static_cast<std::uint64_t>(value);
    };
    const auto nx = extent(0);
    const auto x = relative(0);
    if (dimension == 1) {
        return static_cast<std::size_t>(x);
    }
    const auto ny = extent(1);
    const auto y = relative(1);
    if (dimension == 2) {
        if (y > (std::numeric_limits<std::uint64_t>::max() - x) / nx) {
            throw std::overflow_error("2-D FAB offset overflows");
        }
        const auto offset = x + nx * y;
        if (offset > std::numeric_limits<std::size_t>::max()) {
            throw std::overflow_error("2-D FAB offset exceeds addressable memory");
        }
        return static_cast<std::size_t>(offset);
    }
    const auto z = relative(2);
    if (z > (std::numeric_limits<std::uint64_t>::max() - y) / ny) {
        throw std::overflow_error("3-D FAB row offset overflows");
    }
    const auto row = y + ny * z;
    if (row > (std::numeric_limits<std::uint64_t>::max() - x) / nx) {
        throw std::overflow_error("3-D FAB offset overflows");
    }
    const auto offset = x + nx * row;
    if (offset > std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("3-D FAB offset exceeds addressable memory");
    }
    return static_cast<std::size_t>(offset);
}

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

    // Pre-load every block the line crosses, per participating level.
    std::vector<std::vector<LoadedBlock>> loadedByLevel(
        static_cast<std::size_t>(maximumLevel) + 1);
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
            for (const auto& block : loadedByLevel[static_cast<std::size_t>(levelIndex)]) {
                if (!contains(block.validBox, point, metadata.dimension)) {
                    continue;
                }
                const auto offset = valueOffset(block.data->box, point, metadata.dimension);
                if (offset >= block.data->values.size()) {
                    throw std::runtime_error("composed FAB index exceeds loaded block");
                }
                cover.level = levelIndex;
                cover.point = point;
                cover.value = static_cast<float>(block.data->values[offset]);
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
    constexpr double endEpsilon = 1e-9;
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
            // Advance to the far boundary of this cell, then nudge a tiny
            // fraction of a cell into the next one. The next iteration
            // re-derives the cell index via floor((x - probLo) / cellSize);
            // landing exactly on a boundary makes that floor rounding-sensitive,
            // and for a non-zero physical origin with a non-dyadic cell size
            // (prob_lo + cellSize) - prob_lo can round below cellSize, so the
            // same cell is selected again and x never advances -> infinite loop.
            // The nudge is far below a cell, so it never skips one (including a
            // finer cell across a coarse/fine boundary in a composite).
            constexpr double cellStepNudge = 1e-9;
            x = center + 0.5 * level.cellSize[lineAxis]
                + cellStepNudge * level.cellSize[lineAxis];
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
            // Advance past the far boundary into the next cell; see the covered
            // branch above for why a bare boundary advance can stall the walk.
            constexpr double cellStepNudge = 1e-9;
            x = center + 0.5 * finestCellSize
                + cellStepNudge * finestCellSize;
        }
    }
    return result;
}

} // namespace amrvis
