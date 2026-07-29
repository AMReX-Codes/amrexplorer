#pragma once

#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/core/Geometry.hpp>

#include <QPointF>

#include <array>

namespace amrvis::qt {

// Converts between a view's logical in-plane physical coordinates (dataset
// axes 0 and 1 -- (x, y) for Cartesian, (r, theta) for 2-D spherical) and
// scene (pixmap-pixel) coordinates, absorbing the spherical (r, theta) ->
// (R, Z) warp. Scene y is top-down because the displayed raster is flipped
// vertically, so plane row 0 maps to the bottom of the scene.
//
// For non-spherical data displayRegion == logicalRegion and every mapping is
// the plain linear one the overlay call sites used inline; the struct is only
// built and used on the spherical path, so those sites keep their exact prior
// behavior.
struct PlaneMapping {
    bool spherical = false;
    RealBox logicalRegion;  // plane.physicalRegion: (x, y) or (r, theta)
    RealBox displayRegion;  // pixmap (R, Z) bounds (== logicalRegion if !spherical)
    double sceneWidth = 1.0;
    double sceneHeight = 1.0;
    double planeWidth = 1.0;
    double planeHeight = 1.0;

    // Logical (x, y)/(r, theta) -> scene point.
    [[nodiscard]] QPointF sceneFromLogical(double a, double b) const
    {
        const auto display = spherical
            ? sphericalToDisplay(a, b) : std::array<double, 2>{a, b};
        const double spanX = displayRegion.upper[0] - displayRegion.lower[0];
        const double spanY = displayRegion.upper[1] - displayRegion.lower[1];
        const double x = spanX != 0.0
            ? (display[0] - displayRegion.lower[0]) / spanX * sceneWidth : 0.0;
        const double y = spanY != 0.0
            ? sceneHeight - (display[1] - displayRegion.lower[1]) / spanY * sceneHeight
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
        if (spherical) {
            return displayToSpherical(u, v);
        }
        return {u, v};
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
