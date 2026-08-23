#pragma once

#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/core/Geometry.hpp>

#include <QPointF>
#include <QRectF>

#include <array>

namespace amrvis::qt {

// The physical box that a rectangle of raster pixels covers, given the box the
// whole raster covers. Only the two axes the view displays are narrowed;
// `rasterRegion`'s third stays as it is, which for a slice is the thickness the
// plane was requested with.
//
// Raster rows count down while physical coordinates count up, so the vertical
// edges swap: the rect's *bottom* gives the lower physical bound. Getting that
// backwards mirrors the region about the plane's middle, which looks entirely
// plausible on a symmetric domain -- which is why this is one definition used
// by both callers rather than the formula written out twice.
[[nodiscard]] inline RealBox physicalRegionForRasterRect(
    const RealBox& rasterRegion, double rasterWidth, double rasterHeight,
    const QRectF& rasterRect, const std::array<int, 2>& axes) noexcept
{
    auto region = rasterRegion;
    if (!(rasterWidth > 0.0) || !(rasterHeight > 0.0)) {
        return region;
    }
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto xExtent = rasterRegion.upper[xAxis] - rasterRegion.lower[xAxis];
    const auto yExtent = rasterRegion.upper[yAxis] - rasterRegion.lower[yAxis];
    region.lower[xAxis]
        = rasterRegion.lower[xAxis] + rasterRect.left() / rasterWidth * xExtent;
    region.upper[xAxis]
        = rasterRegion.lower[xAxis] + rasterRect.right() / rasterWidth * xExtent;
    region.lower[yAxis] = rasterRegion.lower[yAxis]
        + (rasterHeight - rasterRect.bottom()) / rasterHeight * yExtent;
    region.upper[yAxis] = rasterRegion.lower[yAxis]
        + (rasterHeight - rasterRect.top()) / rasterHeight * yExtent;
    return region;
}

// Converts between a view's logical in-plane physical coordinates (dataset
// axes 0 and 1 -- (x, y) for Cartesian, (r, theta) for 2-D spherical) and
// scene (pixmap-pixel) coordinates, absorbing the spherical layout: the R-Z
// warp, or the r-theta / theta-r logical (optionally axis-swapped) placements.
// Scene y is top-down because the displayed raster is flipped vertically, so
// plane row 0 maps to the bottom of the scene.
//
// For non-spherical data displayRegion == logicalRegion and every mapping is
// the plain linear one the overlay call sites used inline; the struct is only
// built and used on the spherical path, so those sites keep their exact prior
// behavior.
struct PlaneMapping {
    bool spherical = false;
    SphericalDisplay mode = SphericalDisplay::RZ;
    RealBox logicalRegion;  // plane.physicalRegion: (x, y) or (r, theta)
    RealBox displayRegion;  // pixmap display bounds (== logicalRegion if !spherical)
    double sceneWidth = 1.0;
    double sceneHeight = 1.0;
    double planeWidth = 1.0;
    double planeHeight = 1.0;

    // Logical (r, theta) -> display axes (u, v) matching displayRegion/pixmap.
    [[nodiscard]] std::array<double, 2> displayFromLogical(
        double r, double theta) const
    {
        if (!spherical) {
            return {r, theta};
        }
        switch (mode) {
        case SphericalDisplay::RZ:
            return sphericalToDisplay(r, theta);
        case SphericalDisplay::ThetaR:
            return {theta, r};
        case SphericalDisplay::RTheta:
        default:
            return {r, theta};
        }
    }

    // Inverse of displayFromLogical: display axes (u, v) -> logical (r, theta).
    [[nodiscard]] std::array<double, 2> logicalFromDisplay(
        double u, double v) const
    {
        if (!spherical) {
            return {u, v};
        }
        switch (mode) {
        case SphericalDisplay::RZ:
            return displayToSpherical(u, v);
        case SphericalDisplay::ThetaR:
            return {v, u};  // u = theta, v = r
        case SphericalDisplay::RTheta:
        default:
            return {u, v};
        }
    }

    // Logical (x, y)/(r, theta) -> scene point.
    [[nodiscard]] QPointF sceneFromLogical(double a, double b) const
    {
        const auto display = displayFromLogical(a, b);
        return sceneFromDisplay(display[0], display[1]);
    }

    // Display-space (u, v) -> scene point: the linear map over displayRegion
    // every layout shares. Used directly for overlays already expressed in
    // display coordinates (the R-Z vector glyphs).
    [[nodiscard]] QPointF sceneFromDisplay(double u, double v) const
    {
        const double spanX = displayRegion.upper[0] - displayRegion.lower[0];
        const double spanY = displayRegion.upper[1] - displayRegion.lower[1];
        const double x = spanX != 0.0
            ? (u - displayRegion.lower[0]) / spanX * sceneWidth : 0.0;
        const double y = spanY != 0.0
            ? sceneHeight - (v - displayRegion.lower[1]) / spanY * sceneHeight
            : 0.0;
        return {x, y};
    }

    // Scene point -> logical (x, y)/(r, theta). Inverse of sceneFromLogical.
    [[nodiscard]] std::array<double, 2> logicalFromScene(double px, double py) const
    {
        const double spanX = displayRegion.upper[0] - displayRegion.lower[0];
        const double spanY = displayRegion.upper[1] - displayRegion.lower[1];
        const double u = displayRegion.lower[0] + px / sceneWidth * spanX;
        const double v = displayRegion.lower[1]
            + (sceneHeight - py) / sceneHeight * spanY;
        return logicalFromDisplay(u, v);
    }

    // Plane-pixel (col, row; row 0 = bottom) -> scene point. Used to re-project
    // contour polylines, which are traced in the logical (r, theta) raster.
    [[nodiscard]] QPointF sceneFromPlanePixel(double col, double row) const
    {
        const double spanX = logicalRegion.upper[0] - logicalRegion.lower[0];
        const double spanY = logicalRegion.upper[1] - logicalRegion.lower[1];
        const double a = logicalRegion.lower[0] + col / planeWidth * spanX;
        const double b = logicalRegion.lower[1] + row / planeHeight * spanY;
        return sceneFromLogical(a, b);
    }
};

} // namespace amrvis::qt
