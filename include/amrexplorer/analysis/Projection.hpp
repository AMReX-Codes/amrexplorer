#pragma once

#include <amrexplorer/core/StopToken.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Exact projection of leaf cells onto a uniform image. Unlike a
// face-on view made by summing sampled pixels, every leaf cell contributes
// its value times the volume it shares with each pixel's column, so nothing
// is resampled and the projected total is conservative by construction:
//
//   sum over pixels of image * pixelArea
//     == sum over leaf cells of value * |cell intersect slab|
//
// where the slab is the image rectangle times the line-of-sight bounds.
// Selection is by fractional overlap, not by cell center: a boundary cell
// contributes the part of it inside the slab.

namespace amrvis {

class PlotfileDataset;

struct ProjectionRequest {
    std::string field;
    // When set, the image is the weighted average integral(f w) / integral(w)
    // along each column instead of the column integral of f.
    std::optional<std::string> weight;
    // The line-of-sight axis; the image axes are the other two in increasing
    // order (y, z for x; x, z for y; x, y for z).
    int axis = 2;
    // Image bounds (u0, v0, u1, v1) on the image axes.
    std::array<double, 4> rect{};
    // Line-of-sight bounds.
    std::array<double, 2> axisBounds{};
    // Pixels along u and v.
    std::array<int, 2> resolution{0, 0};
};

struct ProjectionResult {
    int width = 0;
    int height = 0;
    // Row-major over (v, u): pixel (i, j) is at j * width + i, with j = 0 the
    // row at v0. Without a weight: the pixel-averaged column integral, (1 /
    // pixelArea) times the integral of f over the pixel's column, in units of
    // the field times native length. With one: the weighted average.
    std::vector<double> image;
    // The column integral of the weight (pixel-averaged), when one was given.
    std::vector<double> weightImage;
    // The fraction of each pixel's column (pixelArea * pathLength) that leaf
    // cells fill: 1 inside the domain, less where the column leaves it.
    std::vector<double> coverage;
    double pixelArea = 0.0;
    double pathLength = 0.0;
    std::uint64_t cells = 0;
    // The direct sum over leaf cells of value * overlap volume (or of value *
    // weight * overlap volume, with a weight), for checking conservation.
    double directIntegral = 0.0;
};

// Throws AnalysisError for a geometry the projection does not support (it
// needs a 3-D Cartesian plotfile), an unknown field, or invalid bounds.
[[nodiscard]] ProjectionResult projectLeafCells(PlotfileDataset& dataset,
    const ProjectionRequest& request, StopToken cancellation = {});

// The exact cell average of a field over each pixel of a plane normal to
// `axis` at `coordinate`: the leaf cells the plane passes through -- on each
// level, the cell whose index along the axis is floor((coordinate - x0) /
// dx), so a plane on a cell face takes the cell above it -- each weighted by
// the area it shares with the pixel. Unlike a sampled slice (SliceQuery),
// nothing is read at a point: a pixel covering several fine cells gets
// their area-weighted mean.
struct CellAverageSliceRequest {
    std::string field;
    int axis = 2;
    double coordinate = 0.0;
    std::array<double, 4> rect{};
    std::array<int, 2> resolution{0, 0};
};

struct CellAverageSliceResult {
    int width = 0;
    int height = 0;
    // Row-major over (v, u), j = 0 at v0. The area-weighted sum divided by
    // the pixel area -- so a pixel only partly inside the domain is diluted
    // by the uncovered part, as in Kangaroo -- and NaN where no cell meets
    // the pixel.
    std::vector<double> image;
    // The fraction of each pixel's area the plane's leaf cells cover.
    std::vector<double> coverage;
    // The finest level contributing to each pixel, -1 where none does.
    std::vector<std::int16_t> finestLevel;
    double pixelArea = 0.0;
};

[[nodiscard]] CellAverageSliceResult sliceCellAverage(PlotfileDataset& dataset,
    const CellAverageSliceRequest& request, StopToken cancellation = {});

} // namespace amrvis
