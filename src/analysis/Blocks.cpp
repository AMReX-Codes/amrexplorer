#include <amrexplorer/analysis/Blocks.hpp>

#include <amrexplorer/analysis/LeafChunks.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace amrvis {
namespace {

std::pair<std::size_t, std::size_t> levelRange(
    const DatasetMetadata& metadata, std::optional<int> level)
{
    if (!level) {
        return {0, metadata.levels.size()};
    }
    if (*level < 0 || static_cast<std::size_t>(*level) >= metadata.levels.size()) {
        throw AnalysisError("level " + std::to_string(*level)
            + " does not exist; the plotfile has "
            + std::to_string(metadata.levels.size()) + " levels");
    }
    const auto index = static_cast<std::size_t>(*level);
    return {index, index + 1};
}

std::uint64_t boxCells(const IntBox& box, int dimension) noexcept
{
    std::uint64_t cells = 1;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        cells *= static_cast<std::uint64_t>(
            static_cast<std::int64_t>(box.upper[i]) - box.lower[i] + 1);
    }
    return cells;
}

// The valid cells of `box` that the next finer level covers, counted by
// marking so overlapping coarsened fine grids count once.
std::uint64_t coveredCells(const DatasetMetadata& metadata, std::size_t level,
    const IntBox& box)
{
    if (level + 1 >= metadata.levels.size()) {
        return 0;
    }
    const auto dimension = metadata.dimension;
    const auto& coarse = metadata.levels[level];
    const auto& fine = metadata.levels[level + 1];
    std::array<int, 3> ratio{1, 1, 1};
    std::array<std::int64_t, 3> extent{1, 1, 1};
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto coarseCells = static_cast<std::int64_t>(coarse.domain.upper[i])
            - coarse.domain.lower[i] + 1;
        const auto fineCells = static_cast<std::int64_t>(fine.domain.upper[i])
            - fine.domain.lower[i] + 1;
        if (coarseCells <= 0 || fineCells % coarseCells != 0) {
            throw AnalysisError("level " + std::to_string(level + 1)
                + " is not an integer refinement of level "
                + std::to_string(level));
        }
        ratio[i] = static_cast<int>(fineCells / coarseCells);
        extent[i] = static_cast<std::int64_t>(box.upper[i]) - box.lower[i] + 1;
    }
    const auto floorDivide = [](int value, int divisor) {
        const auto quotient = value / divisor;
        return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
    };
    std::vector<std::uint8_t> mask;
    std::uint64_t covered = 0;
    for (const auto& block : fine.blocks) {
        std::array<std::int64_t, 3> lo{0, 0, 0};
        std::array<std::int64_t, 3> hi{0, 0, 0};
        bool overlaps = true;
        for (int axis = 0; axis < dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            lo[i] = std::max(floorDivide(block.box.lower[i], ratio[i]),
                        box.lower[i]) - box.lower[i];
            hi[i] = std::min(floorDivide(block.box.upper[i], ratio[i]),
                        box.upper[i]) - box.lower[i];
            overlaps = overlaps && lo[i] <= hi[i];
        }
        if (!overlaps) {
            continue;
        }
        if (mask.empty()) {
            mask.assign(static_cast<std::size_t>(boxCells(box, dimension)), 0);
        }
        for (auto k = lo[2]; k <= hi[2]; ++k) {
            for (auto j = lo[1]; j <= hi[1]; ++j) {
                for (auto i = lo[0]; i <= hi[0]; ++i) {
                    auto& cell = mask[static_cast<std::size_t>(
                        (k * extent[1] + j) * extent[0] + i)];
                    covered += cell == 0 ? 1U : 0U;
                    cell = 1;
                }
            }
        }
    }
    return covered;
}

} // namespace

std::vector<BlockInfo> listBlocks(
    const PlotfileDataset& dataset, std::optional<int> level)
{
    const auto& metadata = dataset.metadata();
    const auto [first, last] = levelRange(metadata, level);
    std::vector<BlockInfo> blocks;
    for (auto index = first; index < last; ++index) {
        const auto& levelData = metadata.levels[index];
        for (std::size_t grid = 0; grid < levelData.blocks.size(); ++grid) {
            const auto& block = levelData.blocks[grid];
            BlockInfo info;
            info.level = static_cast<int>(index);
            info.grid = static_cast<int>(grid);
            info.box = block.box;
            for (int axis = 0; axis < metadata.dimension; ++axis) {
                // Cell faces, not centers: the box the block occupies.
                const auto i = static_cast<std::size_t>(axis);
                info.bounds.lower[i] = levelData.indexOrigin[i]
                    + static_cast<double>(block.box.lower[i]) * levelData.cellSize[i];
                info.bounds.upper[i] = levelData.indexOrigin[i]
                    + static_cast<double>(block.box.upper[i] + 1)
                        * levelData.cellSize[i];
            }
            info.cells = boxCells(block.box, metadata.dimension);
            info.coveredCells = coveredCells(metadata, index, block.box);
            info.file = block.filePath;
            info.offset = block.fileOffset;
            blocks.push_back(std::move(info));
        }
    }
    return blocks;
}

std::vector<BlockExtrema> blockStatistics(PlotfileDataset& dataset,
    const std::vector<std::string>& fields, std::optional<int> level,
    std::optional<int> grid, StopToken cancellation)
{
    const auto& metadata = dataset.metadata();
    std::vector<FieldId> ids;
    for (const auto& name : fields) {
        const auto found = std::find_if(metadata.fields.begin(),
            metadata.fields.end(),
            [&](const FieldMetadata& field) { return field.name == name; });
        if (found == metadata.fields.end()) {
            throw AnalysisError("unknown field '" + name + "'");
        }
        ids.push_back(FieldId{static_cast<std::uint32_t>(
            std::distance(metadata.fields.begin(), found))});
    }
    const auto [first, last] = levelRange(metadata, level);
    std::vector<BlockExtrema> result;
    for (auto index = first; index < last; ++index) {
        const auto& levelData = metadata.levels[index];
        std::size_t gridFirst = 0;
        std::size_t gridLast = levelData.blocks.size();
        if (grid) {
            if (*grid < 0 || static_cast<std::size_t>(*grid) >= gridLast) {
                throw AnalysisError("level " + std::to_string(index)
                    + " has no grid " + std::to_string(*grid));
            }
            gridFirst = static_cast<std::size_t>(*grid);
            gridLast = gridFirst + 1;
        }
        for (auto g = gridFirst; g < gridLast; ++g) {
            BlockExtrema extrema;
            extrema.level = static_cast<int>(index);
            extrema.grid = static_cast<int>(g);
            for (const auto field : ids) {
                if (cancellation.stop_requested()) {
                    throw ReadCancelled();
                }
                BlockRequest request;
                request.dataset = dataset.id();
                request.level = static_cast<int>(index);
                request.gridIndex = static_cast<int>(g);
                request.field = field;
                const auto access = dataset.requestBlock(request, cancellation);
                const auto& block = *access.handle;
                auto lo = std::numeric_limits<double>::infinity();
                auto hi = -std::numeric_limits<double>::infinity();
                bool any = false;
                const auto scan = [&](auto values) {
                    for (const auto value : values) {
                        const auto v = static_cast<double>(value);
                        if (std::isnan(v)) {
                            continue;
                        }
                        any = true;
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                    }
                };
                if (const auto single = block.values.singleValues(); !single.empty()) {
                    scan(single);
                } else {
                    scan(block.values.doubleValues());
                }
                if (!any) {
                    lo = hi = std::numeric_limits<double>::quiet_NaN();
                }
                extrema.boxes.push_back(block.box);
                extrema.includesGhostCells.push_back(
                    block.box != levelData.blocks[g].box);
                extrema.minimum.push_back(lo);
                extrema.maximum.push_back(hi);
            }
            result.push_back(std::move(extrema));
        }
    }
    return result;
}

} // namespace amrvis
