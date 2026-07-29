// Pins PlaneMapping's per-mode invariants: logical <-> scene round trips, and
// which logical axis each screen direction follows in the three spherical
// layouts. The theta-r horizontal-axis check is the invariant the line-plot
// tool's drag-direction-to-axis mapping relies on.

#include "PlaneMapping.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool approx(double a, double b, double tol = 1e-9)
{
    return std::abs(a - b) <= tol;
}

amrvis::qt::PlaneMapping baseMapping()
{
    amrvis::qt::PlaneMapping mapping;
    mapping.spherical = true;
    // r in [1, 2] (axis 0), theta in [0, 0.5] (axis 1); 16 x 8 plane.
    mapping.logicalRegion.lower[0] = 1.0;
    mapping.logicalRegion.upper[0] = 2.0;
    mapping.logicalRegion.lower[1] = 0.0;
    mapping.logicalRegion.upper[1] = 0.5;
    mapping.planeWidth = 16.0;
    mapping.planeHeight = 8.0;
    return mapping;
}

void requireRoundTrip(const amrvis::qt::PlaneMapping& mapping, const char* what)
{
    for (double r : {1.05, 1.5, 1.95}) {
        for (double theta : {0.05, 0.25, 0.45}) {
            const auto scene = mapping.sceneFromLogical(r, theta);
            const auto back = mapping.logicalFromScene(scene.x(), scene.y());
            require(approx(back[0], r) && approx(back[1], theta), what);
        }
    }
}

} // namespace

int main()
{
    using amrvis::SphericalDisplay;

    // r-theta: identity layout. Scene x follows r, scene y follows theta
    // (top-down, so larger theta is a smaller scene y).
    {
        auto mapping = baseMapping();
        mapping.mode = SphericalDisplay::RTheta;
        mapping.displayRegion = mapping.logicalRegion;
        mapping.sceneWidth = 16.0;
        mapping.sceneHeight = 8.0;
        requireRoundTrip(mapping, "r-theta round trip");

        const auto left = mapping.logicalFromScene(2.0, 4.0);
        const auto right = mapping.logicalFromScene(10.0, 4.0);
        require(right[0] > left[0] && approx(right[1], left[1]),
            "r-theta: scene x varies r only");
        const auto low = mapping.logicalFromScene(2.0, 6.0);
        const auto high = mapping.logicalFromScene(2.0, 1.0);
        require(high[1] > low[1] && approx(high[0], low[0]),
            "r-theta: scene y varies theta only");

        // Plane pixel (col, row) with row 0 at the bottom: the bottom-left
        // cell corner lands at the scene's bottom-left.
        const auto corner = mapping.sceneFromPlanePixel(0.0, 0.0);
        require(approx(corner.x(), 0.0) && approx(corner.y(), 8.0),
            "r-theta: plane row 0 maps to the scene bottom");
    }

    // theta-r: transposed layout. Scene x follows theta -- the invariant the
    // line tool's horizontal-drag-varies-theta rule depends on -- and scene y
    // follows r.
    {
        auto mapping = baseMapping();
        mapping.mode = SphericalDisplay::ThetaR;
        mapping.displayRegion.lower[0] = 0.0;
        mapping.displayRegion.upper[0] = 0.5;
        mapping.displayRegion.lower[1] = 1.0;
        mapping.displayRegion.upper[1] = 2.0;
        mapping.sceneWidth = 8.0;
        mapping.sceneHeight = 16.0;
        requireRoundTrip(mapping, "theta-r round trip");

        const auto left = mapping.logicalFromScene(1.0, 8.0);
        const auto right = mapping.logicalFromScene(7.0, 8.0);
        require(right[1] > left[1] && approx(right[0], left[0]),
            "theta-r: scene x varies theta only");
        const auto low = mapping.logicalFromScene(4.0, 14.0);
        const auto high = mapping.logicalFromScene(4.0, 2.0);
        require(high[0] > low[0] && approx(high[1], low[1]),
            "theta-r: scene y varies r only");
    }

    // R-Z: the warp. Round trips through the nonlinear mapping, and the
    // logical origin edge (theta = 0) lands on the scene's left edge (R = 0).
    {
        auto mapping = baseMapping();
        mapping.mode = SphericalDisplay::RZ;
        mapping.displayRegion = amrvis::sphericalDisplayBounds(mapping.logicalRegion);
        mapping.sceneWidth = 61.0;
        mapping.sceneHeight = 72.0;
        requireRoundTrip(mapping, "R-Z round trip");

        const auto axisPoint = mapping.sceneFromLogical(1.5, 0.0);
        require(approx(axisPoint.x(), 0.0), "R-Z: theta = 0 lies on R = 0");
    }

    std::cout << "plane_mapping OK\n";
    return 0;
}
