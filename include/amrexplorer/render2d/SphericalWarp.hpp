#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/render2d/ImageBuffer.hpp>

namespace amrvis {

struct WarpedRaster {
    // ARGB32 raster in physical (R, Z) display space, row 0 = bottom (the same
    // convention as renderScalarPlane, so the downstream vertical flip still
    // applies). Pixels outside the annular sector are fully transparent.
    ImageBuffer image;
    // Physical (R, Z) bounds of image; only in-plane axes 0 and 1 are set.
    RealBox displayRegion;
};

// Resample a logically-rectangular (r, theta) raster into physical (R, Z)
// display space. src is the rendered slice (row 0 = bottom); logicalRegion
// gives its physical bounds with axis 0 = r and axis 1 = theta. supersample
// (clamped to at least 1) sets how many output pixels span each source cell:
// higher values trace the curved cell boundaries more smoothly at the cost of
// a larger raster. The output pixel pitch is square so an isotropic view
// transform preserves the R:Z aspect ratio, and output dimensions are capped at
// maxDimension. Degenerate input (non-positive extents) falls back to the
// unwarped raster.
[[nodiscard]] WarpedRaster warpSpherical(const ImageBuffer& src,
    const RealBox& logicalRegion, int maxDimension, int supersample);

} // namespace amrvis
