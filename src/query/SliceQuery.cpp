#include <amrexplorer/query/SliceQuery.hpp>

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

struct LoadedBlock {
    IntBox validBox;
    PlotfileDataset::BlockCache::Handle data;
};

// The blocks of one level that intersect the planning region. Levels are
// processed finest first so the composed per-point lookup resolves fine
// over coarse by construction.
struct LevelBlocks {
    int levelIndex = 0;
    std::vector<LoadedBlock> blocks;
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

// The index box a physical region covers at one level. Piecewise sampling
// passes the visible region; linear sampling passes a halo-expanded region
// so the blocks of the bracketing cell centers are loaded too.
IntBox requestIndexBox(const RealBox& region, const SliceRequest& request,
    const DatasetMetadata& metadata, const LevelMetadata& level,
    const std::array<int, 2>& planeAxes)
{
    auto result = level.domain;
    for (const auto axis : planeAxes) {
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

std::array<int, 2> planeAxes(int dimension, int normalDirection)
{
    if (dimension == 2) {
        return {0, 1};
    }
    std::array<int, 2> axes{};
    std::size_t next = 0;
    for (int axis = 0; axis < 3; ++axis) {
        if (axis != normalDirection) {
            axes[next++] = axis;
        }
    }
    return axes;
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

    // The composed piecewise-constant field at a physical point: the finest
    // participating level with a block covering the point's cell wins.
    // Returns the value and the covering level, or nothing when the point
    // is outside every grid.
    const auto valueAt = [&metadata, &levels](const Real3& position)
        -> std::optional<std::pair<double, int>> {
        for (const auto& levelBlocks : levels) {
            const auto& level =
                metadata.levels[static_cast<std::size_t>(levelBlocks.levelIndex)];
            Int3 point;
            for (int axis = 0; axis < metadata.dimension; ++axis) {
                point[static_cast<std::size_t>(axis)] = physicalToIndex(
                    position[static_cast<std::size_t>(axis)], metadata, level, axis);
            }
            for (const auto& block : levelBlocks.blocks) {
                if (!contains(block.validBox, point, metadata.dimension)) {
                    continue;
                }
                const auto offset =
                    valueOffset(block.data->box, point, metadata.dimension);
                if (offset >= block.data->values.size()) {
                    throw std::runtime_error("composed FAB index exceeds loaded block");
                }
                return std::pair{block.data->values[offset], levelBlocks.levelIndex};
            }
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
