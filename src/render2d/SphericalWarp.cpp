#include <amrexplorer/render2d/SphericalWarp.hpp>

#include <amrexplorer/core/CoordinateSystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace amrvis {

WarpedRaster warpSpherical(
    const ImageBuffer& src, const RealBox& logicalRegion, int maxDimension)
{
    WarpedRaster out;

    const double r0 = logicalRegion.lower[0];
    const double r1 = logicalRegion.upper[0];
    const double t0 = logicalRegion.lower[1];
    const double t1 = logicalRegion.upper[1];

    out.displayRegion = sphericalDisplayBounds(logicalRegion);
    const double displayRLo = out.displayRegion.lower[0];
    const double displayRHi = out.displayRegion.upper[0];
    const double displayZLo = out.displayRegion.lower[1];
    const double displayZHi = out.displayRegion.upper[1];
    const double spanR = displayRHi - displayRLo;
    const double spanZ = displayZHi - displayZLo;

    if (src.width <= 0 || src.height <= 0 || !(r1 > r0) || !(t1 > t0)
        || !(spanR > 0.0) || !(spanZ > 0.0) || maxDimension < 1) {
        // Degenerate geometry: hand back the unwarped raster so callers still
        // render something recognizable rather than an empty pixmap.
        out.image = src;
        out.displayRegion = logicalRegion;
        return out;
    }

    // Square physical pitch fine enough to keep the source's finest detail:
    // the smaller of the radial cell size (dr) and the tangential cell arc
    // length at the outer radius (r1*dtheta). A finer-than-source pitch means
    // several output pixels cover each source cell, so the warp never leaves
    // gaps between sectors.
    const double dr = (r1 - r0) / static_cast<double>(src.width);
    const double dtheta = (t1 - t0) / static_cast<double>(src.height);
    double pitch = std::min(dr, r1 * dtheta);
    if (!(pitch > 0.0)) {
        pitch = std::max(spanR, spanZ);
    }

    int width = std::max(1, static_cast<int>(std::lround(spanR / pitch)));
    int height = std::max(1, static_cast<int>(std::lround(spanZ / pitch)));
    if (width > maxDimension || height > maxDimension) {
        const double scale = static_cast<double>(maxDimension)
            / static_cast<double>(std::max(width, height));
        width = std::max(1, static_cast<int>(std::lround(width * scale)));
        height = std::max(1, static_cast<int>(std::lround(height * scale)));
    }

    out.image.width = width;
    out.image.height = height;
    out.image.strideBytes = width * static_cast<int>(sizeof(std::uint32_t));
    // Zero-initialize to transparent (alpha 0); pixels outside the sector keep
    // this value.
    out.image.rgba.assign(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);

    const int srcW = src.width;
    const int srcH = src.height;
    for (int row = 0; row < height; ++row) {
        // Row 0 is the bottom, so display Z increases with the row index.
        const double displayZ = displayZLo
            + (static_cast<double>(row) + 0.5) / height * spanZ;
        for (int col = 0; col < width; ++col) {
            const double displayR = displayRLo
                + (static_cast<double>(col) + 0.5) / width * spanR;
            const auto rtheta = displayToSpherical(displayR, displayZ);
            const double r = rtheta[0];
            const double theta = rtheta[1];
            if (r < r0 || r > r1 || theta < t0 || theta > t1) {
                continue;  // outside the annular sector: stays transparent
            }
            int sx = static_cast<int>((r - r0) / (r1 - r0) * srcW);
            int sy = static_cast<int>((theta - t0) / (t1 - t0) * srcH);
            sx = std::clamp(sx, 0, srcW - 1);
            sy = std::clamp(sy, 0, srcH - 1);
            out.image.rgba[static_cast<std::size_t>(row) * static_cast<std::size_t>(width)
                    + static_cast<std::size_t>(col)]
                = src.rgba[static_cast<std::size_t>(sy) * static_cast<std::size_t>(srcW)
                    + static_cast<std::size_t>(sx)];
        }
    }

    return out;
}

} // namespace amrvis
