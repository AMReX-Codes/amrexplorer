#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/render2d/ImageBuffer.hpp>
#include <amrexplorer/render2d/MappedGridWarp.hpp>

#include <array>
#include <optional>

namespace amrvis {

// The window a warp of the (r, theta) sector `logicalRegion` draws for
// `window`: the request on axes 0 and 1 (R, Z) as given, even where it
// reaches past the sector, or the sector's bounding box
// (sphericalDisplayBounds) when the request is not a valid box on those axes
// (the default request). Axis 2 keeps logicalRegion's bounds. Nullopt when the
// sector is unusable (non-positive r or theta extent, a non-finite bound, or
// a bounding box of zero extent), the cases in which warpSphericalRZ falls
// back.
[[nodiscard]] std::optional<RealBox> sphericalWindow(
    const RealBox& logicalRegion, const RealBox& window);

// Sub-quads per (r, theta) cell along theta: enough that the chord of a
// constant-r arc at `outerRadius` stays within a quarter of a pixel of the
// arc at `pixelsPerLength`, between 1 and 64. Constant-theta edges are
// straight rays and are never subdivided.
[[nodiscard]] int sphericalThetaSubdivisions(
    double dtheta, double outerRadius, double pixelsPerLength);

// Draws an (r, theta) raster on its physical (R, Z) wedge the way
// warpMappedGrid draws a mapped grid: every cell becomes the quadrilateral of
// its four corners R = r sin(theta), Z = r cos(theta), rasterized forward with
// 3x3 coverage antialiasing into an output of `pixels` covering `window` (see
// sphericalWindow), with the covering cells' mean colour and the covered
// fraction as alpha, so the arcs and the sector's boundary are smooth at any
// zoom. Cells are subdivided along theta (sphericalThetaSubdivisions) so the
// arcs are followed, not chorded. A zero pixel count on both axes draws the
// raster's natural size: a square pitch a quarter of the smaller of the
// radial cell size and the outer tangential cell arc, capped at 4096 pixels
// per axis. The
// result's displayRegion is the window drawn, mappedBounds the sector's
// bounding box, and sourceIndex names the (r, theta) raster pixel at each
// pixel's centre. Degenerate input (see sphericalWindow) hands back the source
// raster with displayRegion = logicalRegion and no source index.
[[nodiscard]] MappedWarpedRaster warpSphericalRZ(const ImageBuffer& src,
    const RealBox& logicalRegion, const RealBox& window,
    std::array<int, 2> pixels);

} // namespace amrvis
