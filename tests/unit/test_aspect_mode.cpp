#include "AspectMode.hpp"

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

bool nearly(double actual, double expected)
{
    return std::abs(actual - expected) <= 1e-12 * std::max(1.0, std::abs(expected));
}

bool nearly(const std::array<double, 3>& actual, const std::array<double, 3>& expected)
{
    return nearly(actual[0], expected[0]) && nearly(actual[1], expected[1])
        && nearly(actual[2], expected[2]);
}

// plotfile_2d_tall: 64x1024 cells over a 1x1024 domain, so dy = 64 dx.
amrvis::DatasetMetadata tallMetadata()
{
    amrvis::DatasetMetadata metadata;
    metadata.dimension = 2;
    metadata.finestLevel = 0;
    metadata.physicalDomain.lower = {{0.0, 0.0, 0.0}};
    metadata.physicalDomain.upper = {{1.0, 1024.0, 1.0}};
    amrvis::LevelMetadata level;
    level.domain.upper = {{63, 1023, 0}};
    level.cellSize = {{1.0 / 64.0, 1.0, 1.0}};
    metadata.levels.push_back(level);
    return metadata;
}

constexpr std::array<double, 3> unit{1.0, 1.0, 1.0};

void physicalModeStretchesByTheFinestCellSize()
{
    const auto metadata = tallMetadata();
    require(nearly(amrvis::qt::displayStretchPerAxis(metadata,
                amrvis::qt::AspectMode::CellCounts, unit, false),
                {1.0, 1.0, 1.0}),
        "Cell Counts is not the identity stretch");
    require(nearly(amrvis::qt::displayStretchPerAxis(metadata,
                amrvis::qt::AspectMode::PhysicalSize, unit, false),
                {1.0, 64.0, 1.0}),
        "Physical Size did not stretch the tall axis by the cell aspect");

    // The finest level is the raster's unit: a coarser level's cell sizes
    // must not enter.
    auto refined = metadata;
    refined.finestLevel = 1;
    auto fine = refined.levels.front();
    fine.level = 1;
    fine.cellSize = {{1.0 / 128.0, 1.0 / 4.0, 1.0}};
    refined.levels.push_back(fine);
    require(nearly(amrvis::qt::displayStretchPerAxis(refined,
                amrvis::qt::AspectMode::PhysicalSize, unit, false),
                {1.0, 32.0, 1.0}),
        "Physical Size did not use the finest level's cell sizes");
}

void axisFactorsMultiplyAndNormalize()
{
    const auto metadata = tallMetadata();
    require(nearly(amrvis::qt::displayStretchPerAxis(metadata,
                amrvis::qt::AspectMode::CellCounts, {2.0, 1.0, 1.0}, false),
                {2.0, 1.0, 1.0}),
        "an X factor in Cell Counts did not stretch X");
    // Doubling X against a 64x taller Y halves the ratio: (2/64, 1) -> (1, 32).
    require(nearly(amrvis::qt::displayStretchPerAxis(metadata,
                amrvis::qt::AspectMode::PhysicalSize, {2.0, 1.0, 1.0}, false),
                {1.0, 32.0, 1.0}),
        "an X factor in Physical Size did not combine with the cell aspect");
    // Equal factors change nothing after normalization.
    require(nearly(amrvis::qt::displayStretchPerAxis(metadata,
                amrvis::qt::AspectMode::CellCounts, {3.0, 3.0, 3.0}, false),
                {1.0, 1.0, 1.0}),
        "equal factors were not normalized away");
    // Bad factors count as one, and a third axis is ignored in 2-D.
    require(nearly(amrvis::qt::displayStretchPerAxis(metadata,
                amrvis::qt::AspectMode::CellCounts, {0.0, -1.0, 5.0}, false),
                {1.0, 1.0, 1.0}),
        "non-positive factors were not treated as one");
}

void physicalModeNeedsPhysicalGeometryAndNoSphere()
{
    auto metadata = tallMetadata();
    require(nearly(amrvis::qt::displayStretchPerAxis(metadata,
                amrvis::qt::AspectMode::PhysicalSize, unit, true),
                {1.0, 1.0, 1.0}),
        "a spherical dataset took the physical stretch");
    metadata.hasPhysicalGeometry = false;
    require(nearly(amrvis::qt::displayStretchPerAxis(metadata,
                amrvis::qt::AspectMode::PhysicalSize, unit, false),
                {1.0, 1.0, 1.0}),
        "a dataset without physical geometry took the physical stretch");
    // User factors still apply to such data.
    require(nearly(amrvis::qt::displayStretchPerAxis(metadata,
                amrvis::qt::AspectMode::PhysicalSize, {1.0, 4.0, 1.0}, false),
                {1.0, 4.0, 1.0}),
        "an axis factor was lost without physical geometry");
}

void isotropyFollowsTheScreenNotTheCells()
{
    const auto metadata = tallMetadata();
    const auto cells = amrvis::qt::displayStretchPerAxis(metadata,
        amrvis::qt::AspectMode::CellCounts, unit, false);
    require(!amrvis::qt::displayIsPhysicallyIsotropic(metadata, cells),
        "square pixels over 64:1 cells passed as physically isotropic");
    const auto physical = amrvis::qt::displayStretchPerAxis(metadata,
        amrvis::qt::AspectMode::PhysicalSize, unit, false);
    require(amrvis::qt::displayIsPhysicallyIsotropic(metadata, physical),
        "Physical Size over 64:1 cells did not pass as isotropic");
    const auto skewed = amrvis::qt::displayStretchPerAxis(metadata,
        amrvis::qt::AspectMode::PhysicalSize, {2.0, 1.0, 1.0}, false);
    require(!amrvis::qt::displayIsPhysicallyIsotropic(metadata, skewed),
        "an unequal axis factor passed as isotropic");

    auto square = metadata;
    square.levels.front().cellSize = {{0.25, 0.25, 1.0}};
    require(amrvis::qt::displayIsPhysicallyIsotropic(square, unit),
        "square cells at unit stretch did not pass as isotropic");
    square.hasPhysicalGeometry = false;
    require(!amrvis::qt::displayIsPhysicallyIsotropic(square, unit),
        "a dataset without physical geometry offered a physical scale bar");
}

// The iso wireframe follows the axis factors alone: physical proportions
// stretched about the domain's lower corner, factors normalized to a smallest
// of one, junk factors read as one.
void isoWireframeFollowsTheAxisFactors()
{
    const auto factors = amrvis::qt::normalizedAxisScale({4.0, 0.0, 16.0});
    require(factors[0] == 4.0 && factors[1] == 1.0 && factors[2] == 16.0,
        "a junk axis factor did not read as one");
    const auto scaled = amrvis::qt::normalizedAxisScale({4.0, 2.0, 16.0});
    require(scaled[0] == 2.0 && scaled[1] == 1.0 && scaled[2] == 8.0,
        "axis factors were not normalized to a smallest of one");
    const amrvis::RealBox domain{amrvis::Real3{{-1.0, 0.0, 10.0}},
        amrvis::Real3{{1.0, 2.0, 12.0}}};
    const auto lower = amrvis::qt::axisScaledDisplayPoint(domain, factors, domain.lower);
    require(lower[0] == -1.0 && lower[1] == 0.0 && lower[2] == 10.0,
        "the domain's lower corner moved under a stretch");
    const auto upper = amrvis::qt::axisScaledDisplayPoint(domain, factors, domain.upper);
    require(upper[0] == 7.0 && upper[1] == 2.0 && upper[2] == 42.0,
        "the domain's upper corner was not stretched per axis about the lower one");
    const auto ones = amrvis::qt::normalizedAxisScale({1.0, 1.0, 1.0});
    const auto same = amrvis::qt::axisScaledDisplayPoint(domain, ones, {{0.5, 1.5, 11.0}});
    require(same[0] == 0.5 && same[1] == 1.5 && same[2] == 11.0,
        "unit factors changed a point");
}

// One finest cell per sample unless the per-axis output cap coarsened the
// raster: what tells a mapped view to re-slice a narrower region.
void rasterPitchOverCellReportsTheCap()
{
    const auto metadata = tallMetadata();
    amrvis::RealBox region;
    region.lower = {{0.0, 0.0, 0.0}};
    region.upper = {{1.0, 1024.0, 0.0}};
    const std::array<int, 2> axes{0, 1};
    // Native: 64 x 1024 samples over the domain, one per cell.
    const auto native = amrvis::qt::rasterPitchOverCell(
        metadata, region, 64, 1024, axes);
    require(nearly(native[0], 1.0) && nearly(native[1], 1.0),
        "a native raster has the cell's pitch on both axes");
    // Capped along y to 512 samples: each sample spans two cells there.
    const auto capped = amrvis::qt::rasterPitchOverCell(
        metadata, region, 64, 512, axes);
    require(nearly(capped[0], 1.0) && nearly(capped[1], 2.0),
        "a capped axis reports the raster pitch over the cell");
    // Half the domain at native pitch is still one.
    amrvis::RealBox half = region;
    half.upper[1] = 512.0;
    const auto zoomed = amrvis::qt::rasterPitchOverCell(
        metadata, half, 64, 512, axes);
    require(nearly(zoomed[0], 1.0) && nearly(zoomed[1], 1.0),
        "a zoomed native raster is not mistaken for a capped one");
    // No physical geometry, an empty plane: one, never a division by zero.
    auto fab = metadata;
    fab.hasPhysicalGeometry = false;
    const auto noGeometry = amrvis::qt::rasterPitchOverCell(
        fab, region, 64, 512, axes);
    require(nearly(noGeometry[0], 1.0) && nearly(noGeometry[1], 1.0),
        "no physical geometry means no correction");
    const auto empty = amrvis::qt::rasterPitchOverCell(
        metadata, region, 0, 512, axes);
    require(nearly(empty[0], 1.0) && nearly(empty[1], 1.0),
        "an empty plane means no correction");
}

} // namespace

int main()
{
    rasterPitchOverCellReportsTheCap();
    isoWireframeFollowsTheAxisFactors();
    physicalModeStretchesByTheFinestCellSize();
    axisFactorsMultiplyAndNormalize();
    physicalModeNeedsPhysicalGeometryAndNoSphere();
    isotropyFollowsTheScreenNotTheCells();
    return 0;
}
