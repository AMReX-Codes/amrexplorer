#include <amrexplorer/render2d/SphericalWarp.hpp>

#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/render2d/detail/QuadRasterizer.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace amrvis {

namespace {

constexpr std::array<int, 2> inPlaneAxes{0, 1};

// The sector's bounding box, or nothing when the sector cannot be drawn.
std::optional<RealBox> sectorBounds(const RealBox& logicalRegion)
{
    const double r0 = logicalRegion.lower[0];
    const double r1 = logicalRegion.upper[0];
    const double t0 = logicalRegion.lower[1];
    const double t1 = logicalRegion.upper[1];
    if (!std::isfinite(r0) || !std::isfinite(r1) || !std::isfinite(t0)
        || !std::isfinite(t1) || !(r1 > r0) || !(t1 > t0)) {
        return std::nullopt;
    }
    auto bounds = sphericalDisplayBounds(logicalRegion);
    bounds.lower[2] = logicalRegion.lower[2];
    bounds.upper[2] = logicalRegion.upper[2];
    if (!detail::validOnAxes(bounds, inPlaneAxes)) {
        return std::nullopt;
    }
    return bounds;
}

} // namespace

std::optional<RealBox> sphericalWindow(
    const RealBox& logicalRegion, const RealBox& window)
{
    const auto bounds = sectorBounds(logicalRegion);
    if (!bounds || !detail::validOnAxes(window, inPlaneAxes)) {
        return bounds;
    }
    RealBox drawn = *bounds;
    for (const auto axis : inPlaneAxes) {
        const auto i = static_cast<std::size_t>(axis);
        drawn.lower[i] = window.lower[i];
        drawn.upper[i] = window.upper[i];
    }
    return drawn;
}

int sphericalThetaSubdivisions(
    double dtheta, double outerRadius, double pixelsPerLength)
{
    // A chord of angle alpha at radius r sits r(1 - cos(alpha/2)), about
    // r alpha^2 / 8, inside the arc; a quarter pixel allows
    // alpha <= sqrt(2 / (r s)) with s pixels per length.
    constexpr int maximum = 64;
    if (!(dtheta > 0.0) || !(outerRadius > 0.0) || !(pixelsPerLength > 0.0)
        || !std::isfinite(dtheta) || !std::isfinite(outerRadius)
        || !std::isfinite(pixelsPerLength)) {
        return 1;
    }
    const double wanted = std::ceil(dtheta * std::sqrt(outerRadius * pixelsPerLength / 2.0));
    if (!(wanted > 1.0)) {
        return 1;
    }
    return static_cast<int>(std::min(static_cast<double>(maximum), wanted));
}

MappedWarpedRaster warpSphericalRZ(const ImageBuffer& src,
    const RealBox& logicalRegion, const RealBox& window,
    std::array<int, 2> pixels)
{
    MappedWarpedRaster out;
    const auto fallback = [&]() {
        out.image = src;
        out.displayRegion = logicalRegion;
        out.mappedBounds = logicalRegion;
        out.sourceIndex.reset();
        return out;
    };

    const int srcW = src.width;
    const int srcH = src.height;
    const auto pixelCount = static_cast<std::size_t>(std::max(0, srcW))
        * static_cast<std::size_t>(std::max(0, srcH));
    const auto bounds = sectorBounds(logicalRegion);
    const auto drawn = sphericalWindow(logicalRegion, window);
    if (srcW <= 0 || srcH <= 0 || !bounds || !drawn || src.rgba.size() < pixelCount) {
        return fallback();
    }
    const double r0 = logicalRegion.lower[0];
    const double t0 = logicalRegion.lower[1];
    const double r1 = logicalRegion.upper[0];
    const double dr = (logicalRegion.upper[0] - r0) / static_cast<double>(srcW);
    const double dtheta = (logicalRegion.upper[1] - t0) / static_cast<double>(srcH);
    const double minR = drawn->lower[0];
    const double minZ = drawn->lower[1];
    const double spanR = drawn->upper[0] - minR;
    const double spanZ = drawn->upper[1] - minZ;

    // The output is the caller's screen window at its pixel count. With no
    // count the raster's natural size: a square pitch a quarter of the
    // smaller of the radial cell size and the outer tangential cell arc,
    // capped at the slice output cap so the image stays a size a display
    // would show.
    constexpr int maximumPixels = 16384;
    constexpr int naturalCap = 4096;
    constexpr double naturalPixelsPerCell = 4.0;
    int width = pixels[0];
    int height = pixels[1];
    if (width <= 0 && height <= 0) {
        double pitch = std::min(dr, r1 * dtheta) / naturalPixelsPerCell;
        if (!(pitch > 0.0) || !std::isfinite(pitch)) {
            pitch = std::max(spanR, spanZ);
        }
        width = std::max(1, static_cast<int>(std::lround(spanR / pitch)));
        height = std::max(1, static_cast<int>(std::lround(spanZ / pitch)));
        if (width > naturalCap || height > naturalCap) {
            const double scale = static_cast<double>(naturalCap)
                / static_cast<double>(std::max(width, height));
            width = std::max(1, static_cast<int>(std::lround(width * scale)));
            height = std::max(1, static_cast<int>(std::lround(height * scale)));
        }
    }
    width = std::clamp(width, 1, maximumPixels);
    height = std::clamp(height, 1, maximumPixels);

    const double scaleR = static_cast<double>(width) / spanR;
    const double scaleZ = static_cast<double>(height) / spanZ;
    const double pixelsPerLength = std::max(scaleR, scaleZ);
    const int along = sphericalThetaSubdivisions(dtheta, r1, pixelsPerLength);
    // An arc bulges at most its sagitta past the chord of its cell's corners.
    const double cullPad = r1 * dtheta * dtheta / 8.0 * pixelsPerLength + 1.0;
    const auto corner = [&](double col, double row) {
        const auto display = sphericalToDisplay(r0 + col * dr, t0 + row * dtheta);
        return detail::Point{(display[0] - minR) * scaleR, (display[1] - minZ) * scaleZ};
    };
    auto drawnRaster = detail::rasterizeCells(
        src, width, height, {1, along}, cullPad, corner);

    out.displayRegion = *drawn;
    out.mappedBounds = *bounds;
    out.image = std::move(drawnRaster.image);
    out.sourceIndex = std::move(drawnRaster.sourceIndex);
    return out;
}

} // namespace amrvis
