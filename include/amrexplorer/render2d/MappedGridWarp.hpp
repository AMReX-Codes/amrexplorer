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
    // applies). Pixels no cell covers are fully transparent; a pixel a cell
    // edge crosses carries the covered fraction as alpha and the covering
    // cells' mean colour.
    ImageBuffer image;
    // Physical bounds of `image` -- the window it was drawn for, which may
    // reach past the nodes -- indexed on the dataset axes the slice spans
    // (like the Cartesian display region); the normal axis keeps the logical
    // region's bounds.
    RealBox displayRegion;
    // The node bounding box of the whole plane on the same axes.
    RealBox mappedBounds;
    // Per display pixel, parallel to image.rgba: the source raster pixel
    // (row-major, row 0 = bottom) drawn at the pixel's centre, or -1. Shared
    // so the GUI state and the cache path never copy it. Null when the warp
    // fell back.
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

// The window a warp of `nodes` draws for `window`: the request on the
// in-plane `axes` as given, even where it reaches past the nodes, or the node
// bounding box when the request is not a valid box on those axes (the
// default request). The normal axis keeps nodes.physicalRegion's bounds.
// Nullopt when the plane is unusable (as mappedGridDisplayBounds).
[[nodiscard]] std::optional<RealBox> mappedGridWindow(
    const MappedGridPlane& nodes, std::array<int, 2> axes,
    const RealBox& window);

// Draws a logically-rectangular raster on its mapped grid: every raster cell
// becomes the quadrilateral of its four node positions, rasterized forward
// into an output of `pixels` covering `window` (see mappedGridWindow; a zero
// pixel count falls back to the raster's own size on that axis). The output
// is meant to be the screen itself -- the view's visible window at its
// device-pixel size -- so cell edges are rasterized where they are seen and
// stay straight at any zoom. Each pixel takes 3x3 subsamples: the covering
// cells' mean colour, with the covered fraction as alpha, so an edge crossing
// a pixel is antialiased and the domain's boundary fades rather than steps.
// Forward filling needs no inverse of the mapping and leaves no gap between
// cells however thin they are; cells outside the window are skipped, and a
// window no cell reaches draws a transparent image. `axes` are the dataset
// axes of nodes.a and nodes.b. Degenerate input (a node plane that does not
// fit the raster, non-finite or zero-extent node bounds) hands back the
// source raster with displayRegion = nodes.physicalRegion and no source index.
[[nodiscard]] MappedWarpedRaster warpMappedGrid(const ImageBuffer& src,
    const MappedGridPlane& nodes, std::array<int, 2> axes,
    const RealBox& window, std::array<int, 2> pixels);

// The physical position of a fractional raster-pixel coordinate (col, row;
// integer values are the cell corners): bilinear over the containing cell's
// four nodes, clamped to the plane. Overlays anchored in raster-pixel space
// (contours, glyphs, box outlines) map through this.
[[nodiscard]] std::array<double, 2> mappedDisplayPosition(
    const MappedGridPlane& nodes, double col, double row);

} // namespace amrvis
