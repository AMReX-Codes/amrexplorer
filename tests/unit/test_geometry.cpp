#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/Metadata.hpp>

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

bool nearlyEqual(double a, double b)
{
    return std::abs(a - b) <= 1e-12 * std::max(1.0, std::abs(b));
}

} // namespace

int main()
{
    // 4x4 cells of size 0.25: exact binary arithmetic, no rounding noise.
    const amrvis::RealBox domain{{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 0.0}}};
    const amrvis::Real3 cellSize{{0.25, 0.25, 1.0}};
    const std::array<int, 2> axes{0, 1};

    // Mid-cell edges expand outward to the surrounding cell boundaries.
    {
        const amrvis::RealBox region{{{0.3, 0.1, 0.0}}, {{0.7, 0.6, 0.0}}};
        const auto snapped = amrvis::snapToCellBoundaries(
            region, domain, cellSize, axes);
        require(snapped.lower[0] == 0.25, "lower x did not floor to cell edge");
        require(snapped.upper[0] == 0.75, "upper x did not ceil to cell edge");
        require(snapped.lower[1] == 0.0, "lower y did not floor to cell edge");
        require(snapped.upper[1] == 0.75, "upper y did not ceil to cell edge");
        require(snapped.lower[2] == region.lower[2]
            && snapped.upper[2] == region.upper[2],
            "axis outside the list was modified");
    }

    // An already-aligned region is unchanged.
    {
        const amrvis::RealBox region{{{0.25, 0.5, 0.0}}, {{0.75, 1.0, 0.0}}};
        const auto snapped = amrvis::snapToCellBoundaries(
            region, domain, cellSize, axes);
        require(snapped == region, "aligned region was modified");
    }

    // A selection inside a single cell snaps out to that whole cell.
    {
        const amrvis::RealBox region{{{0.3, 0.3, 0.0}}, {{0.4, 0.4, 0.0}}};
        const auto snapped = amrvis::snapToCellBoundaries(
            region, domain, cellSize, axes);
        require(snapped.lower[0] == 0.25 && snapped.upper[0] == 0.5,
            "single-cell selection did not snap to its cell in x");
        require(snapped.lower[1] == 0.25 && snapped.upper[1] == 0.5,
            "single-cell selection did not snap to its cell in y");
    }

    // A non-zero origin anchors the index space.
    {
        const amrvis::RealBox shiftedDomain{{{2.0, -1.0, 0.0}},
            {{3.0, 0.0, 0.0}}};
        const amrvis::RealBox region{{{2.3, -0.9, 0.0}}, {{2.7, -0.4, 0.0}}};
        const auto snapped = amrvis::snapToCellBoundaries(
            region, shiftedDomain, cellSize, axes);
        require(snapped.lower[0] == 2.25 && snapped.upper[0] == 2.75,
            "non-zero origin: wrong x snap");
        require(snapped.lower[1] == -1.0 && snapped.upper[1] == -0.25,
            "non-zero origin: wrong y snap");
    }

    // With a non-binary cell size the snapped edges still land on cell
    // boundaries and the extent stays an exact multiple of the cell size —
    // the property the slice sampler relies on.
    {
        const amrvis::Real3 tenth{{0.1, 0.1, 1.0}};
        const amrvis::RealBox region{{{0.17, 0.23, 0.0}}, {{0.43, 0.61, 0.0}}};
        const auto snapped = amrvis::snapToCellBoundaries(
            region, domain, tenth, axes);
        for (const auto axis : axes) {
            const auto i = static_cast<std::size_t>(axis);
            const auto lowerCells = (snapped.lower[i] - domain.lower[i])
                / tenth[i];
            const auto upperCells = (snapped.upper[i] - domain.lower[i])
                / tenth[i];
            require(nearlyEqual(lowerCells, std::round(lowerCells)),
                "snapped lower edge is not on a cell boundary");
            require(nearlyEqual(upperCells, std::round(upperCells)),
                "snapped upper edge is not on a cell boundary");
            require(nearlyEqual((snapped.upper[i] - snapped.lower[i]) / tenth[i],
                std::round(upperCells) - std::round(lowerCells)),
                "snapped extent is not an integer cell count");
        }
        require(snapped.lower[0] <= region.lower[0]
            && snapped.upper[0] >= region.upper[0]
            && snapped.lower[1] <= region.lower[1]
            && snapped.upper[1] >= region.upper[1],
            "snapped region does not contain the selection");
    }

    // snapToNearestCellGrid: a fractionally translated region rounds to the
    // nearest cell boundary and keeps its integer cell count.
    {
        const amrvis::RealBox region{{{0.35, 0.1, 0.0}}, {{0.85, 0.6, 0.0}}};
        const auto snapped = amrvis::snapToNearestCellGrid(
            region, domain, cellSize, axes);
        require(snapped.lower[0] == 0.25 && snapped.upper[0] == 0.75,
            "nearest-cell snap moved x edges off the grid");
        require(snapped.lower[1] == 0.0 && snapped.upper[1] == 0.5,
            "nearest-cell snap moved y edges off the grid");
        require(snapped.lower[2] == region.lower[2]
            && snapped.upper[2] == region.upper[2],
            "axis outside the list was modified");
    }

    // The pan failure mode: a region whose lower edge sits at a half-cell
    // offset puts every pixel center on a cell boundary. Snapping moves the
    // edge to a boundary (std::round breaks the x.5 tie away from zero) and
    // restores the span (2 cells).
    {
        const amrvis::RealBox region{{{0.125, 0.375, 0.0}}, {{0.625, 0.875, 0.0}}};
        const auto snapped = amrvis::snapToNearestCellGrid(
            region, domain, cellSize, axes);
        require(snapped.lower[0] == 0.25 && snapped.upper[0] == 0.75,
            "half-cell x offset did not snap to a boundary");
        require(snapped.lower[1] == 0.5 && snapped.upper[1] == 1.0,
            "half-cell y offset did not snap to a boundary");
    }

    // The snapped window stays inside the domain when the translation pushes
    // it against an edge, preserving the cell count.
    {
        const amrvis::RealBox region{{{0.8, -0.1, 0.0}}, {{1.3, 0.4, 0.0}}};
        const auto snapped = amrvis::snapToNearestCellGrid(
            region, domain, cellSize, axes);
        require(snapped.lower[0] == 0.5 && snapped.upper[0] == 1.0,
            "x window not clamped inside the domain");
        require(snapped.lower[1] == 0.0 && snapped.upper[1] == 0.5,
            "y window not clamped inside the domain");
    }

    // Snapping is idempotent.
    {
        const amrvis::RealBox region{{{0.37, 0.11, 0.0}}, {{0.87, 0.61, 0.0}}};
        const auto once = amrvis::snapToNearestCellGrid(
            region, domain, cellSize, axes);
        const auto twice = amrvis::snapToNearestCellGrid(
            once, domain, cellSize, axes);
        require(once == twice, "nearest-cell snap is not idempotent");
    }

    // Mixed nodal/cell axes use sample-bin bounds as the grid anchor.  This
    // is the panning/expand-to-snap case for an index type of (0,1,1).
    {
        amrvis::LevelMetadata level;
        level.domain = {{{0, 0, 0}}, {{3, 3, 1}}, {{0, 1, 1}}};
        level.cellSize = {{1.0, 1.0, 1.0}};
        const auto sampleDomain = amrvis::sampleBounds(level, level.domain, 3);
        const amrvis::RealBox selection{
            {{0.6, -0.2, 0.0}}, {{2.2, 1.2, 0.0}}};
        const auto expanded = amrvis::snapToCellBoundaries(
            selection, sampleDomain, level.cellSize, axes);
        require(expanded.lower[0] == 0.0 && expanded.upper[0] == 3.0,
            "mixed snap did not use cell-centered x sample bins");
        require(expanded.lower[1] == -0.5 && expanded.upper[1] == 1.5,
            "mixed snap did not use nodal y sample bins");

        const amrvis::RealBox shifted{
            {{0.6, 0.0, 0.0}}, {{2.6, 2.0, 0.0}}};
        const auto panned = amrvis::snapToNearestCellGrid(
            shifted, sampleDomain, level.cellSize, axes);
        require(panned.lower[0] == 1.0 && panned.upper[0] == 3.0,
            "mixed pan did not snap the cell-centered x axis");
        require(panned.lower[1] == 0.5 && panned.upper[1] == 2.5,
            "mixed pan did not snap the nodal y axis");
    }

    return 0;
}
