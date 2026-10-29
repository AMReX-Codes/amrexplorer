#pragma once

#include <amrexplorer/analysis/LeafChunks.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Grids grown by ghost cells, for stencil operations such as gradients. One
// tile per FAB that has leaf cells in the region, matching the leaf-chunk plan
// item for item. Ghost cells are filled the way AMReX's FillPatch fills them:
//
// - from a same-level grid's valid cells where one covers the ghost cell
//   (periodic images included on periodic axes);
// - elsewhere inside the domain, by interpolation from the next coarser
//   level, whose own missing cells are filled the same way, recursing past
//   level l-1 whenever the ghost width exceeds what that level holds;
// - outside a non-periodic domain, not at all: the cell is marked invalid
//   and holds NaN. There is no boundary condition to apply, since a plotfile
//   does not record one.
//
// Interpolation is from coarse cell centers to the fine cell center, per
// axis, with the coarse cell's slope: "linear" takes central differences
// (one-sided at an invalid neighbor), which reproduces linear fields exactly
// across coarse-fine boundaries; "limited" applies the monotonized-central
// limiter AMReX's cell_cons_interp uses, which can flatten linear fields at
// extrema; "constant" copies the coarse value. Each tile carries a leaf mask
// over its valid region, so a reduction over masked tile cells counts each
// leaf cell once and agrees with the leaf-chunk iterator.

namespace amrvis {

class PlotfileDataset;

enum class TileInterpolation : std::uint8_t {
    Linear,
    Limited,
    Constant
};

struct TileRequest {
    LeafChunkRequest cells;
    int ghost = 1;
    std::array<bool, 3> periodic{false, false, false};
    TileInterpolation interpolation = TileInterpolation::Linear;
};

struct TileSelection {
    int ghost = 1;
    std::array<bool, 3> periodic{false, false, false};
    TileInterpolation interpolation = TileInterpolation::Linear;
};

struct TileItem {
    LeafChunkItem cells;
    IntBox grownBox;
    // Every FAB the ghost fill reads, as (level, grid). Read one at a time,
    // so what a read pins is the largest of their per-FAB estimates.
    std::vector<std::array<int, 2>> touched;
    std::uint64_t estimatedPinnedBytes = 0;
    std::shared_ptr<const TileSelection> selection;
};

struct TilePlan {
    std::shared_ptr<const LeafChunkSelection> cells;
    std::shared_ptr<const TileSelection> selection;
    std::vector<TileItem> items;
};

// One tile: dense arrays over the grown box in FAB order (x fastest).
struct Tile {
    TileItem item;
    int dimension = 0;
    Real3 cellSize;
    Real3 indexOrigin;
    std::array<std::size_t, 3> extent{1, 1, 1};
    // One per requested field, in the plan's dtype.
    std::vector<AnalysisArray> fields;
    // Whether each cell holds a value: false only for ghost cells outside a
    // non-periodic domain (or beyond what any level holds).
    std::vector<std::uint8_t> valid;
    // The level each value came from: the tile's own for valid and
    // same-level ghost cells, coarser for interpolated ones, -1 if invalid.
    std::vector<std::int8_t> sourceLevel;
    // Leaf cells in the region: true only inside the valid box.
    std::vector<std::uint8_t> leafMask;
};

// Throws AnalysisError as planLeafChunks does, for a negative ghost width,
// or when a FAB the ghost fill reads cannot fit in the cache budget.
[[nodiscard]] TilePlan planTiles(
    const PlotfileDataset& dataset, const TileRequest& request);

[[nodiscard]] std::shared_ptr<const Tile> readTile(PlotfileDataset& dataset,
    const TileItem& item, StopToken cancellation = {});

} // namespace amrvis
