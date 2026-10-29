#include <amrexplorer/analysis/Tiles.hpp>

#include <amrexplorer/io/PlotfileDataset.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>

namespace amrvis {
namespace {

int floorDivide(int value, int divisor) noexcept
{
    const auto quotient = value / divisor;
    return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}

std::optional<IntBox> intersect(
    const IntBox& left, const IntBox& right, int dimension) noexcept
{
    IntBox result = left;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        result.lower[i] = std::max(left.lower[i], right.lower[i]);
        result.upper[i] = std::min(left.upper[i], right.upper[i]);
        if (result.lower[i] > result.upper[i]) {
            return std::nullopt;
        }
    }
    return result;
}

IntBox grow(IntBox box, int cells, int dimension) noexcept
{
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        box.lower[i] -= cells;
        box.upper[i] += cells;
    }
    return box;
}

// The geometry a fill needs about one level: its domain, its refinement
// from the level below, and the periodic images of its grids.
struct LevelGeometry {
    IntBox domain;
    Int3 period;
    Int3 ratio{{1, 1, 1}};
};

std::vector<LevelGeometry> levelGeometry(const DatasetMetadata& metadata)
{
    const auto dimension = metadata.dimension;
    std::vector<LevelGeometry> levels;
    for (std::size_t index = 0; index < metadata.levels.size(); ++index) {
        const auto& level = metadata.levels[index];
        LevelGeometry geometry;
        geometry.domain = level.domain;
        for (int axis = 0; axis < dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            geometry.period[i] = level.domain.upper[i] - level.domain.lower[i] + 1;
            if (index > 0) {
                const auto& coarse = metadata.levels[index - 1];
                const auto coarseCells
                    = coarse.domain.upper[i] - coarse.domain.lower[i] + 1;
                if (coarseCells <= 0 || geometry.period[i] % coarseCells != 0) {
                    throw AnalysisError("level " + std::to_string(index)
                        + " is not an integer refinement of level "
                        + std::to_string(index - 1));
                }
                geometry.ratio[i] = geometry.period[i] / coarseCells;
            }
        }
        levels.push_back(geometry);
    }
    return levels;
}

// A dense patch over a box, x fastest, with per-cell validity.
struct Patch {
    IntBox box;
    int dimension = 0;
    std::array<std::int64_t, 3> extent{1, 1, 1};
    std::vector<double> values;
    std::vector<std::uint8_t> valid;
    std::vector<std::int8_t> source;

    Patch(const IntBox& b, int d) : box(b), dimension(d)
    {
        std::size_t cells = 1;
        for (int axis = 0; axis < d; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            extent[i] = static_cast<std::int64_t>(b.upper[i]) - b.lower[i] + 1;
            cells *= static_cast<std::size_t>(extent[i]);
        }
        values.assign(cells, std::numeric_limits<double>::quiet_NaN());
        valid.assign(cells, 0);
        source.assign(cells, -1);
    }

    [[nodiscard]] bool contains(const Int3& c) const noexcept
    {
        for (int axis = 0; axis < dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            if (c[i] < box.lower[i] || c[i] > box.upper[i]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::size_t offset(const Int3& c) const noexcept
    {
        std::size_t result = 0;
        std::size_t stride = 1;
        for (int axis = 0; axis < dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            result += stride * static_cast<std::size_t>(c[i] - box.lower[i]);
            stride *= static_cast<std::size_t>(extent[i]);
        }
        return result;
    }
};

template <typename Visit>
void forEachCell(const IntBox& box, int dimension, Visit&& visit)
{
    Int3 c;
    const auto lo = [&](std::size_t i) {
        return static_cast<int>(i) < dimension ? box.lower[i] : 0;
    };
    const auto hi = [&](std::size_t i) {
        return static_cast<int>(i) < dimension ? box.upper[i] : 0;
    };
    for (c[2] = lo(2); c[2] <= hi(2); ++c[2]) {
        for (c[1] = lo(1); c[1] <= hi(1); ++c[1]) {
            for (c[0] = lo(0); c[0] <= hi(0); ++c[0]) {
                visit(c);
            }
        }
    }
}

// The periodic images of a level's grids that can reach a box: offsets in
// whole domain periods on each periodic axis.
std::vector<Int3> periodicShifts(const LevelGeometry& level,
    const std::array<bool, 3>& periodic, int dimension)
{
    std::vector<Int3> shifts{Int3{{0, 0, 0}}};
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        if (!periodic[i]) {
            continue;
        }
        std::vector<Int3> grown;
        for (const auto& shift : shifts) {
            for (const int n : {-1, 0, 1}) {
                auto next = shift;
                next[i] = n * level.period[i];
                grown.push_back(next);
            }
        }
        shifts = std::move(grown);
    }
    return shifts;
}

IntBox shifted(IntBox box, const Int3& shift, int dimension) noexcept
{
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        box.lower[i] += shift[i];
        box.upper[i] += shift[i];
    }
    return box;
}

bool insideDomain(const LevelGeometry& level, const Int3& c,
    const std::array<bool, 3>& periodic, int dimension) noexcept
{
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        if (!periodic[i]
            && (c[i] < level.domain.lower[i] || c[i] > level.domain.upper[i])) {
            return false;
        }
    }
    return true;
}

// The fill itself, shared by planning (which only records the FABs a fill
// would read) and reading (which also copies and interpolates values).
class Filler {
public:
    Filler(const DatasetMetadata& metadata, const TileSelection& selection,
        PlotfileDataset* dataset, std::optional<FieldId> field,
        StopToken cancellation)
        : m_metadata(metadata)
        , m_levels(levelGeometry(metadata))
        , m_selection(selection)
        , m_dataset(dataset)
        , m_field(field)
        , m_cancellation(std::move(cancellation))
    {
    }

    Patch fill(int level, const IntBox& box)
    {
        const auto dimension = m_metadata.dimension;
        Patch patch(box, dimension);
        const auto& geometry = m_levels[static_cast<std::size_t>(level)];
        const auto shifts = periodicShifts(geometry, m_selection.periodic, dimension);
        const auto& grids = m_metadata.levels[static_cast<std::size_t>(level)].blocks;
        for (std::size_t grid = 0; grid < grids.size(); ++grid) {
            const auto& valid = grids[grid].box;
            std::optional<PlotfileDataset::BlockAccess> access;
            for (const auto& shift : shifts) {
                const auto overlap
                    = intersect(shifted(valid, shift, dimension), box, dimension);
                if (!overlap) {
                    continue;
                }
                m_touched.insert({level, static_cast<int>(grid)});
                if (!m_dataset) {
                    forEachCell(*overlap, dimension, [&](const Int3& c) {
                        patch.valid[patch.offset(c)] = 1;
                    });
                    continue;
                }
                if (!access) {
                    if (m_cancellation.stop_requested()) {
                        throw ReadCancelled();
                    }
                    BlockRequest request;
                    request.dataset = m_dataset->id();
                    request.level = level;
                    request.gridIndex = static_cast<int>(grid);
                    request.field = *m_field;
                    access = m_dataset->requestBlock(request, m_cancellation);
                }
                const auto& block = *access->handle;
                const auto blockCell = [&](const Int3& c) {
                    std::size_t result = 0;
                    std::size_t stride = 1;
                    for (int axis = 0; axis < dimension; ++axis) {
                        const auto i = static_cast<std::size_t>(axis);
                        result += stride
                            * static_cast<std::size_t>(
                                c[i] - shift[i] - block.box.lower[i]);
                        stride *= static_cast<std::size_t>(
                            static_cast<std::int64_t>(block.box.upper[i])
                            - block.box.lower[i] + 1);
                    }
                    return result;
                };
                forEachCell(*overlap, dimension, [&](const Int3& c) {
                    const auto at = patch.offset(c);
                    patch.values[at] = block.values[blockCell(c)];
                    patch.valid[at] = 1;
                    patch.source[at] = static_cast<std::int8_t>(level);
                });
            }
        }
        if (level == 0) {
            return patch;
        }

        // Cells no grid of this level holds, inside the domain: interpolate
        // them from the next coarser level, filled the same way.
        IntBox missing;
        bool anyMissing = false;
        forEachCell(box, dimension, [&](const Int3& c) {
            if (patch.valid[patch.offset(c)]
                || !insideDomain(geometry, c, m_selection.periodic, dimension)) {
                return;
            }
            if (!anyMissing) {
                missing.lower = missing.upper = c;
                anyMissing = true;
            }
            for (std::size_t i = 0; i < 3; ++i) {
                missing.lower[i] = std::min(missing.lower[i], c[i]);
                missing.upper[i] = std::max(missing.upper[i], c[i]);
            }
        });
        if (!anyMissing) {
            return patch;
        }
        const auto& ratio = geometry.ratio;
        IntBox coarseBox = missing;
        for (int axis = 0; axis < dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            coarseBox.lower[i] = floorDivide(missing.lower[i], ratio[i]);
            coarseBox.upper[i] = floorDivide(missing.upper[i], ratio[i]);
        }
        // One coarse cell either side, for the slopes.
        const auto coarse = fill(level - 1, grow(coarseBox, 1, dimension));
        if (!m_dataset) {
            return patch;
        }
        forEachCell(missing, dimension, [&](const Int3& c) {
            const auto at = patch.offset(c);
            if (patch.valid[at]
                || !insideDomain(geometry, c, m_selection.periodic, dimension)) {
                return;
            }
            Int3 parent;
            for (int axis = 0; axis < dimension; ++axis) {
                const auto i = static_cast<std::size_t>(axis);
                parent[i] = floorDivide(c[i], ratio[i]);
            }
            const auto parentAt = coarse.offset(parent);
            if (!coarse.valid[parentAt]) {
                return;
            }
            const auto center = coarse.values[parentAt];
            auto value = center;
            for (int axis = 0; axis < dimension; ++axis) {
                const auto i = static_cast<std::size_t>(axis);
                // The fine center's offset from the coarse center, in coarse
                // cells: within (-1/2, 1/2).
                const auto offset = (static_cast<double>(c[i]) + 0.5)
                        / static_cast<double>(ratio[i])
                    - (static_cast<double>(parent[i]) + 0.5);
                value += slope(coarse, parent, i, center) * offset;
            }
            patch.values[at] = value;
            patch.valid[at] = 1;
            patch.source[at] = coarse.source[parentAt];
        });
        return patch;
    }

    [[nodiscard]] const std::set<std::array<int, 2>>& touched() const noexcept
    {
        return m_touched;
    }

private:
    double slope(const Patch& coarse, const Int3& parent, std::size_t axis,
        double center) const
    {
        if (m_selection.interpolation == TileInterpolation::Constant) {
            return 0.0;
        }
        const auto neighbor = [&](int direction) -> std::optional<double> {
            auto c = parent;
            c[axis] += direction;
            if (!coarse.contains(c)) {
                return std::nullopt;
            }
            const auto at = coarse.offset(c);
            if (!coarse.valid[at]) {
                return std::nullopt;
            }
            return coarse.values[at];
        };
        const auto left = neighbor(-1);
        const auto right = neighbor(1);
        if (m_selection.interpolation == TileInterpolation::Limited) {
            // AMReX's monotonized-central limiter; zero at an extremum and
            // where a neighbor is missing.
            if (!left || !right) {
                return 0.0;
            }
            const auto dl = center - *left;
            const auto dr = *right - center;
            const auto dc = 0.5 * (*right - *left);
            if (dl * dr <= 0.0) {
                return 0.0;
            }
            return std::copysign(
                std::min({std::abs(dc), 2.0 * std::abs(dl), 2.0 * std::abs(dr)}),
                dc);
        }
        if (left && right) {
            return 0.5 * (*right - *left);
        }
        if (right) {
            return *right - center;
        }
        if (left) {
            return center - *left;
        }
        return 0.0;
    }

    const DatasetMetadata& m_metadata;
    std::vector<LevelGeometry> m_levels;
    const TileSelection& m_selection;
    PlotfileDataset* m_dataset;
    std::optional<FieldId> m_field;
    StopToken m_cancellation;
    std::set<std::array<int, 2>> m_touched;
};

} // namespace

TilePlan planTiles(const PlotfileDataset& dataset, const TileRequest& request)
{
    if (request.ghost < 0 || request.ghost > 64) {
        throw AnalysisError("ghost width must be between 0 and 64 cells");
    }
    if (request.cells.fields.empty()) {
        throw AnalysisError("a tile plan needs at least one field");
    }
    const auto cells = planLeafChunks(dataset, request.cells);
    auto selection = std::make_shared<TileSelection>();
    selection->ghost = request.ghost;
    selection->periodic = request.periodic;
    selection->interpolation = request.interpolation;
    const auto& metadata = dataset.metadata();
    const auto dimension = metadata.dimension;
    const auto budget = dataset.cacheMetrics().budgetBytes;
    TilePlan plan;
    plan.cells = cells.selection;
    plan.selection = selection;
    for (const auto& item : cells.items) {
        TileItem tile;
        tile.cells = item;
        tile.grownBox = grow(item.box, request.ghost, dimension);
        tile.selection = selection;
        Filler filler(metadata, *selection, nullptr, std::nullopt, {});
        static_cast<void>(filler.fill(item.level, tile.grownBox));
        for (const auto& touched : filler.touched()) {
            tile.touched.push_back(touched);
            tile.estimatedPinnedBytes = std::max(tile.estimatedPinnedBytes,
                estimateLeafChunkPinnedBytes(
                    dataset, cells.selection->fields, touched[0], touched[1]));
        }
        if (tile.estimatedPinnedBytes > budget) {
            throw AnalysisError("a tile of level " + std::to_string(item.level)
                + " grid " + std::to_string(item.grid) + " needs up to "
                + std::to_string(tile.estimatedPinnedBytes)
                + " bytes of block cache for one FAB its ghost fill reads, "
                  "more than the "
                + std::to_string(budget) + "-byte budget; raise cache_bytes");
        }
        plan.items.push_back(std::move(tile));
    }
    return plan;
}

std::shared_ptr<const Tile> readTile(
    PlotfileDataset& dataset, const TileItem& item, StopToken cancellation)
{
    if (!item.cells.selection || item.cells.selection->dataset != dataset.id()
        || !item.selection) {
        throw AnalysisError("tile " + std::to_string(item.cells.index)
            + " was planned on a different dataset; plan again on this one");
    }
    const auto& metadata = dataset.metadata();
    const auto dimension = metadata.dimension;
    const auto& level = metadata.levels[static_cast<std::size_t>(item.cells.level)];
    auto tile = std::make_shared<Tile>();
    tile->item = item;
    tile->dimension = dimension;
    tile->cellSize = level.cellSize;
    tile->indexOrigin = level.indexOrigin;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        tile->extent[i] = static_cast<std::size_t>(
            static_cast<std::int64_t>(item.grownBox.upper[i])
            - item.grownBox.lower[i] + 1);
    }

    const auto& selection = *item.cells.selection;
    bool first = true;
    for (const auto field : selection.fields) {
        Filler filler(metadata, *item.selection, &dataset, field, cancellation);
        auto patch = filler.fill(item.cells.level, item.grownBox);
        if (first) {
            tile->valid = patch.valid;
            tile->sourceLevel = patch.source;
            first = false;
        }
        if (selection.dtype == AnalysisValueType::Float32) {
            tile->fields.emplace_back(
                std::vector<float>(patch.values.begin(), patch.values.end()));
        } else {
            tile->fields.emplace_back(std::move(patch.values));
        }
    }
    // The leaf cells: the plan item's selection, placed in the grown box.
    Patch leaf(item.grownBox, dimension);
    forEachCell(item.cells.selectedBox, dimension, [&](const Int3& c) {
        leaf.valid[leaf.offset(c)] = 1;
    });
    for (const auto& cover : item.cells.covered) {
        forEachCell(cover, dimension, [&](const Int3& c) {
            leaf.valid[leaf.offset(c)] = 0;
        });
    }
    tile->leafMask = std::move(leaf.valid);
    return tile;
}

} // namespace amrvis
