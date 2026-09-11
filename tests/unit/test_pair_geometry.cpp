#include "PairGeometry.hpp"

#include <amrexplorer/core/Metadata.hpp>

#include <array>
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

bool nearly(double actual, double expected, double tolerance = 1e-9)
{
    return std::abs(actual - expected) <= tolerance * std::max(1.0, std::abs(expected));
}

bool nearly(const amrvis::qt::SceneRect& actual, const amrvis::qt::SceneRect& expected)
{
    return nearly(actual.x, expected.x) && nearly(actual.y, expected.y)
        && nearly(actual.width, expected.width) && nearly(actual.height, expected.height);
}

// A single-level 3-D plotfile over [lower, upper] with the given cell counts.
amrvis::DatasetMetadata plotfile(const amrvis::Real3& lower, const amrvis::Real3& upper,
    const amrvis::Int3& cells)
{
    amrvis::DatasetMetadata metadata;
    metadata.dimension = 3;
    metadata.finestLevel = 0;
    metadata.physicalDomain.lower = lower;
    metadata.physicalDomain.upper = upper;
    amrvis::LevelMetadata level;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        level.domain.upper[axis] = cells[axis] - 1;
        level.cellSize[axis] = (upper[axis] - lower[axis]) / cells[axis];
        level.indexOrigin[axis] = lower[axis];
    }
    metadata.levels.push_back(level);
    return metadata;
}

// The coupled ERF + REMORA example: atmosphere over ocean, touching at z = 0,
// the atmosphere reaching further west.
amrvis::DatasetMetadata erf()
{
    return plotfile({{-20000.0, 0.0, 0.0}}, {{50000.0, 20000.0, 9000.0}}, {{70, 20, 48}});
}

amrvis::DatasetMetadata remora()
{
    return plotfile({{0.0, 0.0, -300.0}}, {{50000.0, 20000.0, 0.0}}, {{50, 20, 40}});
}

void detectsTheSharedPlane()
{
    const auto result = amrvis::qt::pairGeometry(erf(), remora());
    require(result.geometry.has_value(), "the ERF + REMORA pair was refused");
    const auto& geometry = *result.geometry;
    require(geometry.perpendicularAxis == 2, "the shared plane is not z = 0");
    require(geometry.sharedAxes == std::array<int, 2>{0, 1}, "the shared axes are not x, y");
    require(geometry.upperLayer == 0 && geometry.lowerLayer() == 1,
        "the atmosphere is not the upper layer");
    require(nearly(geometry.unionBounds.lower[0], -20000.0)
            && nearly(geometry.unionBounds.upper[0], 50000.0)
            && nearly(geometry.unionBounds.lower[2], -300.0)
            && nearly(geometry.unionBounds.upper[2], 9000.0),
        "the union bounds are wrong");
    require(nearly(geometry.referenceCellSize[0], 1000.0)
            && nearly(geometry.referenceCellSize[2], 7.5),
        "the reference cell size is not the smaller of the two");
    // The interface belongs to the upper layer; either side to its own.
    require(geometry.layerAt(0.0) == 0 && geometry.layerAt(-0.001) == 1
            && geometry.layerAt(5000.0) == 0 && geometry.layerAt(-1000.0) == 1,
        "layerAt does not split at the interface");

    // The companion given first swaps the layers but not the geometry.
    const auto swapped = amrvis::qt::pairGeometry(remora(), erf());
    require(swapped.geometry && swapped.geometry->upperLayer == 1
            && swapped.geometry->perpendicularAxis == 2,
        "swapping the datasets changed the geometry");
}

void refusesWhatCannotShareAPlane()
{
    // Overlapping along every axis: the same domain twice.
    require(!amrvis::qt::pairGeometry(erf(), erf()).geometry
            && !amrvis::qt::pairGeometry(erf(), erf()).error.empty(),
        "two overlapping domains were paired");
    // Apart along two axes: a corner neighbour, not a face neighbour.
    const auto corner = plotfile({{50000.0, 0.0, -300.0}}, {{60000.0, 20000.0, 0.0}}, {{10, 20, 40}});
    require(!amrvis::qt::pairGeometry(erf(), corner).geometry,
        "domains apart along two axes were paired");
    // Two-dimensional data.
    auto flat = remora();
    flat.dimension = 2;
    require(!amrvis::qt::pairGeometry(erf(), flat).geometry, "a 2-D companion was paired");
    // No physical geometry.
    auto fab = remora();
    fab.hasPhysicalGeometry = false;
    require(!amrvis::qt::pairGeometry(erf(), fab).geometry, "a FAB companion was paired");
    // A gap is refused: drawn shut it would hide the missing 100 m.
    const auto gapped = plotfile({{0.0, 0.0, -400.0}}, {{50000.0, 20000.0, -100.0}}, {{50, 20, 40}});
    require(!amrvis::qt::pairGeometry(erf(), gapped).geometry.has_value(),
        "a gap along the perpendicular axis was accepted");
    // Side by side along x is a shared plane too.
    const auto east = plotfile({{50000.0, 0.0, 0.0}}, {{60000.0, 20000.0, 9000.0}}, {{10, 20, 48}});
    const auto beside = amrvis::qt::pairGeometry(erf(), east);
    require(beside.geometry && beside.geometry->perpendicularAxis == 0
            && beside.geometry->upperLayer == 1,
        "a pair touching along x was not detected");
}

void indicesSpanBothLayers()
{
    const auto geometry = *amrvis::qt::pairGeometry(erf(), remora()).geometry;
    require(geometry.perpendicularCells(0) == 48 && geometry.perpendicularCells(1) == 40
            && geometry.stackedCellCount() == 88,
        "the stacked row count is wrong");
    // Lower layer first: row 0 is the ocean floor, row 40 the first air cell.
    require(nearly(geometry.positionForStackedIndex(0), -300.0 + 3.75)
            && nearly(geometry.positionForStackedIndex(39), -3.75)
            && nearly(geometry.positionForStackedIndex(40), 93.75)
            && nearly(geometry.positionForStackedIndex(87), 9000.0 - 93.75),
        "stacked indices do not map to cell centres");
    require(geometry.stackedIndexForPosition(0.0) == 40
            && geometry.stackedIndexForPosition(-0.001) == 39
            && geometry.stackedIndexForPosition(-300.0) == 0
            && geometry.stackedIndexForPosition(20000.0) == 87,
        "positions do not map back to stacked indices");
    // Shared axes over the union at the reference cell.
    require(geometry.unionCellCount(0) == 70 && geometry.unionCellCount(1) == 20,
        "the union cell counts are wrong");
    require(nearly(geometry.positionForUnionIndex(0, 0), -19500.0)
            && geometry.unionIndexForPosition(0, 10000.0) == 30,
        "union indices do not map along x");
}

void cellCountsLayoutStacksRasters()
{
    const auto geometry = *amrvis::qt::pairGeometry(erf(), remora()).geometry;
    const std::array<double, 3> unit{1.0, 1.0, 1.0};
    // The XZ panel (normal y): x shared horizontally, z stacked vertically.
    const amrvis::qt::PairLayout xz(geometry, 1, amrvis::qt::AspectMode::CellCounts,
        unit, {1.0, 1.0});
    require(xz.showsPerpendicular() && xz.axes() == std::array<int, 2>{0, 2},
        "the XZ panel does not show x and z");
    // One scene unit per cell of either raster: ERF 70x48 on top, REMORA
    // 50x40 below it, 20 cells in from the left.
    require(nearly(xz.tileRect(0), {0.0, 0.0, 70.0, 48.0}),
        "the ERF tile is not one unit per cell at the top");
    require(nearly(xz.tileRect(1), {20.0, 48.0, 50.0, 40.0}),
        "the REMORA tile is not stacked under the ERF tile");
    require(nearly(xz.canvasRect(), {0.0, 0.0, 70.0, 88.0}), "the canvas is not the union");
    // Points across the interface: 12 ocean rows down from the surface, and
    // 30 atmosphere columns in from the union's west edge.
    require(nearly(xz.sceneFromPhysical(1, 2, -90.0), 60.0)
            && nearly(xz.sceneFromPhysical(0, 2, 9000.0 - 10.0 * 187.5), 10.0)
            && nearly(xz.sceneFromPhysical(0, 0, 10000.0), 30.0),
        "sceneFromPhysical does not place points in their bands");

    // The XY panel (normal z) shows one layer at a time over the same x, y map.
    const amrvis::qt::PairLayout xy(geometry, 2, amrvis::qt::AspectMode::CellCounts,
        unit, {1.0, 1.0});
    require(!xy.showsPerpendicular(), "the XY panel claims to show z");
    require(nearly(xy.tileRect(0), {0.0, 0.0, 70.0, 20.0})
            && nearly(xy.tileRect(1), {20.0, 0.0, 50.0, 20.0}),
        "the XY tiles are not aligned over the union");
    require(nearly(xy.sceneFromPhysical(1, 1, 15000.0), 5.0),
        "the XY panel's vertical axis does not count down from the union's top");
}

void physicalLayoutStretchesEachLayerOnItsOwn()
{
    const auto geometry = *amrvis::qt::pairGeometry(erf(), remora()).geometry;
    const std::array<double, 3> unit{1.0, 1.0, 1.0};
    // Physical size: the primary sets the unit, as it does alone -- its
    // tightest pixel is a 187.5 m ERF row -- so opening the ocean beside it
    // changes nothing about 1x. ERF is 48 rows tall and 70000 / 187.5 wide;
    // the ocean's 7.5 m rows fall to 0.04 units each, 1.6 for all forty.
    const amrvis::qt::PairLayout physical(geometry, 1,
        amrvis::qt::AspectMode::PhysicalSize, unit, {1.0, 1.0});
    require(nearly(physical.tileRect(0), {0.0, 0.0, 70000.0 / 187.5, 48.0})
            && nearly(physical.tileRect(1),
                {20000.0 / 187.5, 48.0, 50000.0 / 187.5, 300.0 / 187.5}),
        "the physical tiles are not in the primary's units");

    // The ocean's own factor is what brings it out: thirty times taller it
    // is 48 units, as tall as the air, whose tile does not move.
    const amrvis::qt::PairLayout deepOcean(geometry, 1,
        amrvis::qt::AspectMode::PhysicalSize, unit, {1.0, 30.0});
    require(nearly(deepOcean.tileRect(1).height, 48.0)
            && nearly(deepOcean.tileRect(0), {0.0, 0.0, 70000.0 / 187.5, 48.0}),
        "a companion perpendicular factor did not rescale only the ocean band");
    // A shared-axis factor widens both tiles alike.
    const amrvis::qt::PairLayout wide(geometry, 1, amrvis::qt::AspectMode::CellCounts,
        {2.0, 1.0, 1.0}, {1.0, 1.0});
    require(nearly(wide.tileRect(0), {0.0, 0.0, 140.0, 48.0})
            && nearly(wide.tileRect(1), {40.0, 48.0, 100.0, 40.0}),
        "an x factor did not widen both tiles");
}

bool nearly(const amrvis::RealBox& actual, const amrvis::RealBox& expected)
{
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!nearly(actual.lower[axis], expected.lower[axis])
            || !nearly(actual.upper[axis], expected.upper[axis])) {
            return false;
        }
    }
    return true;
}

void inverseMapsRecoverPhysicalRegions()
{
    const auto geometry = *amrvis::qt::pairGeometry(erf(), remora()).geometry;
    const std::array<double, 3> unit{1.0, 1.0, 1.0};
    const amrvis::qt::PairLayout xz(geometry, 1, amrvis::qt::AspectMode::CellCounts,
        unit, {1.0, 1.0});
    const amrvis::qt::PairLayout xy(geometry, 2, amrvis::qt::AspectMode::CellCounts,
        unit, {1.0, 1.0});
    // Round trips through both bands and both kinds of axis.
    require(nearly(xz.physicalFromScene(0, 0, xz.sceneFromPhysical(0, 0, 10000.0)), 10000.0)
            && nearly(xz.physicalFromScene(1, 2, xz.sceneFromPhysical(1, 2, -90.0)), -90.0)
            && nearly(xz.physicalFromScene(0, 2, xz.sceneFromPhysical(0, 2, 1500.0)), 1500.0)
            && nearly(xy.physicalFromScene(1, 1, xy.sceneFromPhysical(1, 1, 15000.0)), 15000.0),
        "physicalFromScene does not invert sceneFromPhysical");
    // A whole tile is the layer's domain.
    require(nearly(*xz.regionForSceneRect(0, xz.tileRect(0)), geometry.bounds[0])
            && nearly(*xz.regionForSceneRect(1, xz.tileRect(1)), geometry.bounds[1])
            && nearly(*xy.regionForSceneRect(1, xy.tileRect(1)), geometry.bounds[1]),
        "a tile's rect does not map back to its domain");
    // A rect straddling the interface: 8 atmosphere rows above it and 12
    // ocean rows below, 20 columns wide, each layer getting its own part.
    const auto straddle = xz.regionForSceneRect(0, {30.0, 40.0, 20.0, 20.0});
    const auto straddleOcean = xz.regionForSceneRect(1, {30.0, 40.0, 20.0, 20.0});
    require(straddle && straddleOcean
            && nearly(*straddle,
                {{{10000.0, 0.0, 0.0}}, {{30000.0, 20000.0, 8.0 * 187.5}}})
            && nearly(*straddleOcean, {{{10000.0, 0.0, -90.0}}, {{30000.0, 20000.0, 0.0}}}),
        "a straddling rect is not split at the interface");
    // Entirely in the atmosphere's band: the ocean is missed; ending at the
    // interface: the ocean is met within tolerance only, so missed too.
    require(!xz.regionForSceneRect(1, {0.0, 0.0, 70.0, 10.0})
            && !xz.regionForSceneRect(1, {0.0, 40.0, 70.0, 8.0})
            && xz.regionForSceneRect(0, {0.0, 40.0, 70.0, 8.0}),
        "a rect outside a band was not reported as missing it");
    // West of the ocean's domain the ocean's part is cut to its own x range.
    const auto west = xz.regionForSceneRect(1, {0.0, 48.0, 30.0, 40.0});
    require(west && nearly(west->lower[0], 0.0) && nearly(west->upper[0], 10000.0)
            && nearly(west->lower[2], -300.0) && nearly(west->upper[2], 0.0),
        "a rect past the ocean's edge was not cut to its domain");
    // Back through the forward map, the ocean's part lands where it was cut.
    require(nearly(xz.sceneRectForRegion(1, *west), {20.0, 48.0, 10.0, 40.0}),
        "the cut region does not map back onto the tile");
}

void displayMapStacksTheWholeDomains()
{
    const auto geometry = *amrvis::qt::pairGeometry(erf(), remora()).geometry;
    const std::array<double, 3> unit{1.0, 1.0, 1.0};
    const amrvis::qt::PairDisplayMap map(geometry, amrvis::qt::AspectMode::CellCounts,
        unit, {1.0, 1.0});
    // The ocean floor is the origin along z; the surface is 40 units up, and
    // the top of the atmosphere 48 more. x counts cells from the union's west.
    const auto floor = map.displayFromPhysical(1, {{0.0, 0.0, -300.0}});
    const auto surface = map.displayFromPhysical(0, {{0.0, 0.0, 0.0}});
    const auto top = map.displayFromPhysical(0, {{50000.0, 20000.0, 9000.0}});
    require(nearly(floor[0], 20.0) && nearly(floor[2], 0.0),
        "the ocean floor is not the display origin along z");
    require(nearly(surface[2], 40.0) && nearly(top[2], 88.0) && nearly(top[0], 70.0),
        "the display map does not stack the two domains");
}

} // namespace

int main()
{
    displayMapStacksTheWholeDomains();
    detectsTheSharedPlane();
    refusesWhatCannotShareAPlane();
    indicesSpanBothLayers();
    cellCountsLayoutStacksRasters();
    physicalLayoutStretchesEachLayerOnItsOwn();
    inverseMapsRecoverPhysicalRegions();
    return 0;
}
