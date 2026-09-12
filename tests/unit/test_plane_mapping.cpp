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

    // Mapped grid: a 2 x 2 plane over [0, 2] x [0, 2] whose top nodes are
    // lifted by 1 (displayRegion y in [0, 3]), drawn into a 4 x 6 pixmap.
    // Plane pixels go through the node positions (bilinear), scene pixels
    // come back through the warp's source index, and display <-> scene is
    // linear over displayRegion.
    {
        amrvis::qt::PlaneMapping mapping;
        mapping.mapped = true;
        mapping.axes = {0, 1};
        mapping.logicalRegion.lower[0] = 0.0;
        mapping.logicalRegion.upper[0] = 2.0;
        mapping.logicalRegion.lower[1] = 0.0;
        mapping.logicalRegion.upper[1] = 2.0;
        mapping.displayRegion = mapping.logicalRegion;
        mapping.displayRegion.upper[1] = 3.0;
        mapping.planeWidth = 2.0;
        mapping.planeHeight = 2.0;
        mapping.sceneWidth = 4.0;
        mapping.sceneHeight = 6.0;
        auto nodes = std::make_shared<amrvis::MappedGridPlane>();
        nodes->width = 3;
        nodes->height = 3;
        nodes->physicalRegion = mapping.logicalRegion;
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 3; ++i) {
                nodes->a.push_back(static_cast<double>(i));
                // Rows 0 and 1 sit at their uniform height; row 2 is lifted.
                nodes->b.push_back(j == 2 ? 3.0 : static_cast<double>(j));
            }
        }
        mapping.nodes = nodes;
        // Source index, row 0 = bottom: display rows 0-1 are plane row 0,
        // rows 2-5 plane row 1 (the stretched cells); columns 0-1 plane
        // column 0, columns 2-3 plane column 1.
        auto index = std::make_shared<std::vector<std::int32_t>>();
        for (int row = 0; row < 6; ++row) {
            for (int col = 0; col < 4; ++col) {
                const int planeRow = row < 2 ? 0 : 1;
                const int planeCol = col < 2 ? 0 : 1;
                index->push_back(planeRow * 2 + planeCol);
            }
        }
        (*index)[static_cast<std::size_t>(5 * 4 + 3)] = -1;  // a lifted corner
        mapping.sourceIndex = index;

        const auto origin = mapping.sceneFromPlanePixel(0.0, 0.0);
        require(approx(origin.x(), 0.0) && approx(origin.y(), 6.0),
            "mapped: plane origin is the scene's bottom-left");
        const auto top = mapping.sceneFromPlanePixel(2.0, 2.0);
        require(approx(top.x(), 4.0) && approx(top.y(), 0.0),
            "mapped: the lifted top-right node is the scene's top-right");
        const auto mid = mapping.sceneFromPlanePixel(1.0, 1.0);
        // Node (1, 1) is at physical (1, 1): scene x 2, scene y 6 - 1/3*6 = 4.
        require(approx(mid.x(), 2.0) && approx(mid.y(), 4.0),
            "mapped: an unlifted interior node keeps its uniform position");
        const auto upper = mapping.sceneFromPlanePixel(1.0, 1.5);
        // Halfway up the stretched cell: y = 1 + 0.5 * (3 - 1) = 2 -> scene 2.
        require(approx(upper.x(), 2.0) && approx(upper.y(), 2.0),
            "mapped: a fractional plane row interpolates the lifted edge");

        const auto bottomLeft = mapping.planePixelFromScene(0.5, 5.5);
        require(bottomLeft && (*bottomLeft)[0] == 0 && (*bottomLeft)[1] == 0,
            "mapped: the bottom-left scene pixel is plane cell (0, 0)");
        const auto topRight = mapping.planePixelFromScene(3.5, 0.5);
        require(!topRight, "mapped: a pixel no cell drew has no plane pixel");
        const auto stretched = mapping.planePixelFromScene(2.5, 1.5);
        require(stretched && (*stretched)[0] == 1 && (*stretched)[1] == 1,
            "mapped: a stretched cell's pixels map to its plane row");
        require(!mapping.planePixelFromScene(-1.0, 1.0)
                && !mapping.planePixelFromScene(1.0, 6.5),
            "mapped: scene pixels outside the pixmap have no plane pixel");

        const auto display = mapping.displayFromScene(2.0, 0.0);
        require(approx(display[0], 1.0) && approx(display[1], 3.0),
            "mapped: display <-> scene is linear over displayRegion");
        const auto back = mapping.sceneFromDisplay(display[0], display[1]);
        require(approx(back.x(), 2.0) && approx(back.y(), 0.0),
            "mapped: sceneFromDisplay inverts displayFromScene");
    }

    // Mapped particles: a 4 x 4 plane over [0, 4]^2 sheared by y' = y + 0.5 x,
    // warped whole at one pixel per unit. A particle at (3, 4.5) lies over the
    // grid though past its logical bounds, and is kept; one at (3, 1) lies
    // below the sheared bottom and is not. With a slab, the normal decides.
    {
        auto nodes = std::make_shared<amrvis::MappedGridPlane>();
        nodes->width = 5;
        nodes->height = 5;
        nodes->physicalRegion.upper = {{4.0, 4.0, 1.0}};
        for (int j = 0; j <= 4; ++j) {
            for (int i = 0; i <= 4; ++i) {
                nodes->a.push_back(static_cast<double>(i));
                nodes->b.push_back(static_cast<double>(j) + 0.5 * i);
            }
        }
        amrvis::ImageBuffer raster;
        raster.width = 4;
        raster.height = 4;
        raster.strideBytes = 16;
        raster.rgba.assign(16, 0xFF808080U);
        const auto warped = amrvis::warpMappedGrid(
            raster, *nodes, {0, 1}, amrvis::RealBox{}, {4, 6});
        require(warped.sourceIndex != nullptr, "mapped particles: the grid did not warp");

        amrvis::ScalarPlane plane;
        plane.width = 4;
        plane.height = 4;
        plane.physicalRegion = nodes->physicalRegion;
        plane.sourceLevel.assign(16, 0);

        amrvis::qt::PlaneMapping mapping;
        mapping.mapped = true;
        mapping.axes = {0, 1};
        mapping.nodes = nodes;
        mapping.sourceIndex = warped.sourceIndex;
        mapping.logicalRegion = plane.physicalRegion;
        mapping.displayRegion = warped.displayRegion;
        mapping.planeWidth = 4.0;
        mapping.planeHeight = 4.0;
        mapping.sceneWidth = 4.0;
        mapping.sceneHeight = 6.0;

        const std::vector<amrvis::SliceCellSlab> noSlabs;
        require(amrvis::qt::mappedParticlePoint(mapping, plane, 3.0, 4.5, 0.5, noSlabs)
                .has_value(),
            "mapped particles: one over the grid past the logical bounds was dropped");
        require(!amrvis::qt::mappedParticlePoint(mapping, plane, 3.0, 1.0, 0.5, noSlabs),
            "mapped particles: one below the sheared grid was kept");
        const std::vector<amrvis::SliceCellSlab> slab{{0.0, 1.0}};
        require(amrvis::qt::mappedParticlePoint(mapping, plane, 3.0, 4.5, 0.5, slab)
                && !amrvis::qt::mappedParticlePoint(mapping, plane, 3.0, 4.5, 1.5, slab),
            "mapped particles: the slab of the cell drawn there was not applied");
        // Displaced faces outrank that logical slab: terrain lifts the cell to
        // [1, 2), so the normal that was inside the slab is out of the cell and
        // the one that was outside is in it.
        nodes->faceLevels = {0};
        nodes->normalLower.assign(25, 1.0);
        nodes->normalUpper.assign(25, 2.0);
        require(!amrvis::qt::mappedParticlePoint(mapping, plane, 3.0, 4.5, 0.5, slab)
                && amrvis::qt::mappedParticlePoint(mapping, plane, 3.0, 4.5, 1.5, slab)
                       .has_value(),
            "mapped particles: the cell's displaced faces were not used");
        // A face sloping across the cell is read where the particle is, not at
        // the cell's centre: with the faces set to a and a + 1 the particle at
        // a = 3 sits in [3, 4), which no corner average of the cell gives.
        for (std::size_t n = 0; n < nodes->a.size(); ++n) {
            nodes->normalLower[n] = nodes->a[n];
            nodes->normalUpper[n] = nodes->a[n] + 1.0;
        }
        require(amrvis::qt::mappedParticlePoint(mapping, plane, 3.0, 4.5, 3.5, slab)
                && !amrvis::qt::mappedParticlePoint(mapping, plane, 3.0, 4.5, 2.9, slab)
                && !amrvis::qt::mappedParticlePoint(mapping, plane, 3.0, 4.5, 4.1, slab),
            "mapped particles: a sloped face was read at the cell's centre");
        require(!mapping.planePixelFromScene(1.0e30, 2.0)
                && !mapping.planePixelFromScene(2.0, -1.0e30),
            "mapped: scene points far off the pixmap have no plane pixel");
    }

    std::cout << "plane_mapping OK\n";
    return 0;
}
