#include "MappedGeometry.hpp"

#include <amrexplorer/core/Geometry.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

int g_failures = 0;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

bool near(double a, double b, double tolerance = 1e-12)
{
    return std::abs(a - b) <= tolerance;
}

// A y-normal panel (axes x and z) over a 4 x 4 x 4 domain of 0.25 cells whose
// nodes were lifted so the canvas spans z in [0.05, 0.96].
amrvis::RealBox canvas()
{
    amrvis::RealBox box;
    box.lower = {{0.0, 0.0, 0.05}};
    box.upper = {{1.0, 1.0, 0.96}};
    return box;
}

void unitFactorsMakeTheTightestCellOneUnit()
{
    const amrvis::Real3 cells{{0.25, 0.25, 0.0625}};
    const amrvis::qt::MappedLayout layout(canvas(), {0, 2}, {1.0, 1.0, 1.0}, cells, 3);
    // The tightest cell (z, 0.0625) is one scene unit: 16 units per length
    // on every axis at unit factors.
    require(near(layout.unitsPerLength(0), 16.0)
            && near(layout.unitsPerLength(2), 16.0),
        "unit factors did not make the tightest finest cell one scene unit");
    const auto rect = layout.canvasRect();
    require(near(rect.x, 0.0) && near(rect.y, 0.0) && near(rect.width, 16.0)
            && near(rect.height, 0.91 * 16.0),
        "the canvas rect is not the canvas bounds in scene units");
}

void sceneCountsDownFromTheCanvasTop()
{
    const amrvis::Real3 cells{{0.25, 0.25, 0.25}};
    const amrvis::qt::MappedLayout layout(canvas(), {0, 2}, {1.0, 1.0, 1.0}, cells, 3);
    // One scene unit per 0.25 of length: x maps up from the left edge, z
    // down from the canvas top.
    require(near(layout.sceneFromPhysical(0, 0.0), 0.0)
            && near(layout.sceneFromPhysical(0, 0.5), 2.0),
        "the horizontal axis does not map up from the canvas left");
    require(near(layout.sceneFromPhysical(2, 0.96), 0.0)
            && near(layout.sceneFromPhysical(2, 0.46), 2.0),
        "the vertical axis does not count down from the canvas top");
    for (const double x : {0.0, 0.3, 1.0}) {
        require(near(layout.physicalFromScene(0, layout.sceneFromPhysical(0, x)), x),
            "x does not round-trip through the scene");
    }
    for (const double z : {0.05, 0.5, 0.96}) {
        require(near(layout.physicalFromScene(2, layout.sceneFromPhysical(2, z)), z),
            "z does not round-trip through the scene");
    }
}

void regionAndRectAreInverses()
{
    const amrvis::Real3 cells{{0.25, 0.25, 0.25}};
    const amrvis::qt::MappedLayout layout(canvas(), {0, 2}, {1.0, 1.0, 1.0}, cells, 3);
    amrvis::RealBox region = canvas();
    region.lower[0] = 0.25;
    region.upper[0] = 0.75;
    region.lower[2] = 0.3;
    region.upper[2] = 0.7;
    const auto rect = layout.sceneRectForRegion(region);
    require(near(rect.x, 1.0) && near(rect.width, 2.0), "the region's x did not land");
    // z in [0.3, 0.7]: the top (0.7) is 0.26 below the canvas top (0.96).
    require(near(rect.y, 0.26 * 4.0) && near(rect.height, 0.4 * 4.0),
        "the region's z did not land top-down");
    const auto back = layout.regionForSceneRect(rect);
    require(near(back.lower[0], 0.25) && near(back.upper[0], 0.75)
            && near(back.lower[2], 0.3) && near(back.upper[2], 0.7),
        "regionForSceneRect did not invert sceneRectForRegion");
    // The normal axis carries the canvas bounds.
    require(near(back.lower[1], 0.0) && near(back.upper[1], 1.0),
        "the normal axis did not carry the canvas bounds");
    // A rect past the canvas maps past it too: a view's window may reach a
    // pixel beyond the canvas edge.
    const auto beyond = layout.regionForSceneRect({-2.0, -1.0, 10.0, 10.0});
    require(near(beyond.lower[0], -0.5) && near(beyond.upper[0], 2.0)
            && near(beyond.upper[2], 1.21) && near(beyond.lower[2], -1.29),
        "a scene rect past the canvas was not mapped as given");
}

void axisFactorsStretchTheScene()
{
    const amrvis::Real3 cells{{0.25, 0.25, 0.25}};
    // z stretched four times: z gets four units per 0.25 where x keeps one.
    const amrvis::qt::MappedLayout layout(canvas(), {0, 2}, {1.0, 1.0, 4.0}, cells, 3);
    require(near(layout.unitsPerLength(0), 4.0) && near(layout.unitsPerLength(2), 16.0),
        "the z factor did not stretch z alone");
    const auto rect = layout.canvasRect();
    require(near(rect.width, 4.0) && near(rect.height, 0.91 * 16.0),
        "the canvas did not follow the z factor");
    // A non-finite or non-positive factor counts as one.
    const amrvis::qt::MappedLayout sane(canvas(), {0, 2}, {0.0, 1.0, -3.0}, cells, 3);
    require(near(sane.unitsPerLength(0), 4.0) && near(sane.unitsPerLength(2), 4.0),
        "bad factors were not sanitized to one");
}

void twoDimensionsNormalizeOverTwoAxes()
{
    // A 2-D dataset: the third cell size (a default of one) must not decide
    // the unit.
    amrvis::RealBox box;
    box.lower = {{0.0, 0.0, 0.0}};
    box.upper = {{2.0, 1.0, 0.0}};
    const amrvis::Real3 cells{{0.5, 0.125, 1.0}};
    const amrvis::qt::MappedLayout layout(box, {0, 1}, {1.0, 1.0, 1.0}, cells, 2);
    require(near(layout.unitsPerLength(0), 8.0) && near(layout.unitsPerLength(1), 8.0),
        "a 2-D layout did not take its unit from the two shown axes");
}

} // namespace

int main()
{
    unitFactorsMakeTheTightestCellOneUnit();
    sceneCountsDownFromTheCanvasTop();
    regionAndRectAreInverses();
    axisFactorsStretchTheScene();
    twoDimensionsNormalizeOverTwoAxes();
    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
