#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/MappedGrid.hpp>
#include <amrexplorer/render2d/ImageBuffer.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace amrvis {

struct MappedWarpedRaster {
    // ARGB32 raster in physical display space, row 0 = bottom (the
    // renderScalarPlane convention, so the downstream vertical flip still
    // applies). Pixels no cell covers are fully transparent.
    ImageBuffer image;
    // Physical bounds of `image`, indexed on the dataset axes the slice
    // spans (like the Cartesian display region); the normal axis keeps the
    // logical region's bounds.
    RealBox displayRegion;
    // Per display pixel, parallel to image.rgba: the source raster pixel
    // (row-major, row 0 = bottom) drawn there, or -1. Shared so the GUI state
    // and the cache path never copy it. Null when the warp fell back.
    std::shared_ptr<const std::vector<std::int32_t>> sourceIndex;
};

// The node bounding box of a mapped plane on the dataset axes `axes` (the
// normal axis keeps nodes.physicalRegion's bounds): the display region a
// warp of it spans, also for a refresh that draws no raster. Nullopt when
// the plane is unusable (node count not matching a W+1 x H+1 plane of at
// least one cell, a non-finite node, or zero extent), the cases in which
// warpMappedGrid falls back.
[[nodiscard]] std::optional<RealBox> mappedGridDisplayBounds(
    const MappedGridPlane& nodes, std::array<int, 2> axes);

// Draws a logically-rectangular raster on its mapped grid: every raster cell
// becomes the quadrilateral of its four node positions, rasterized forward
// into a physically uniform output whose pitch along each axis is that
// axis's raster pitch over `supersample` (clamped to at least 1), capped at
// maxDimension. The pitch is anisotropic like the raster's own, so a
// display pixel has the physical shape of a native cell and the view's
// Physical Size stretch applies to it unchanged; a thin domain keeps its
// short axis resolved. Forward filling needs no inverse of the mapping and
// leaves no gap between cells however thin they are. `axes` are the dataset
// axes of nodes.a and nodes.b. Degenerate input (a node plane that does not fit the raster,
// non-finite or zero-extent node bounds) hands back the source raster with
// displayRegion = nodes.physicalRegion and no source index.
[[nodiscard]] MappedWarpedRaster warpMappedGrid(const ImageBuffer& src,
    const MappedGridPlane& nodes, std::array<int, 2> axes, int maxDimension,
    int supersample);

// The physical position of a fractional raster-pixel coordinate (col, row;
// integer values are the cell corners): bilinear over the containing cell's
// four nodes, clamped to the plane. Overlays anchored in raster-pixel space
// (contours, glyphs, box outlines) map through this.
[[nodiscard]] std::array<double, 2> mappedDisplayPosition(
    const MappedGridPlane& nodes, double col, double row);

} // namespace amrvis
