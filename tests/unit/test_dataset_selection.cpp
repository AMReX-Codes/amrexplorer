// Coverage for coveredSelectionBounds, which turns a block of selected
// Dataset-window cells into the sample indices the image highlights. The
// arithmetic it has to get right is the row/j flip (row 0 is the highest j)
// and the coverage skip, so the page below is deliberately offset from the
// origin and holes are punched in it.

#include "DatasetSelection.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// A 4x3 page (i in 10..13, j in 20..22) with every cell covered; values run
// j ascending with i fastest, matching a real DatasetPage.
amrvis::DatasetPage makePage()
{
    amrvis::DatasetPage page;
    page.lower = {10, 20};
    page.upper = {13, 22};
    page.nx = 4;
    page.ny = 3;
    page.values.assign(12, 0.0F);
    page.covered.assign(12, 1);
    return page;
}

// The cell at (i, j) in the page's own storage order.
std::size_t offsetOf(const amrvis::DatasetPage& page, int i, int j)
{
    return static_cast<std::size_t>(i - page.lower[0])
        + static_cast<std::size_t>(page.nx)
            * static_cast<std::size_t>(j - page.lower[1]);
}

std::string describe(const amrvis::qt::SelectedSampleBox& box)
{
    return "i " + std::to_string(box.iLow) + ".." + std::to_string(box.iHigh)
        + ", j " + std::to_string(box.jLow) + ".." + std::to_string(box.jHigh);
}

void requireBounds(const amrvis::DatasetPage& page,
    const std::vector<amrvis::qt::CellRange>& ranges,
    const amrvis::qt::SelectedSampleBox& expected, const std::string& what)
{
    const auto bounds = amrvis::qt::coveredSelectionBounds(page, ranges);
    require(bounds.has_value(), what + ": no bounds at all");
    require(*bounds == expected,
        what + ": got " + describe(*bounds) + ", wanted " + describe(expected));
}

} // namespace

int main()
{
    const auto page = makePage();

    // One cell. Row 0 is the highest j, so the top-left cell is (i=10, j=22)
    // -- the flip this whole function exists for.
    requireBounds(page, {{0, 0, 0, 0}}, {10, 10, 22, 22}, "top-left cell");
    requireBounds(page, {{2, 3, 2, 3}}, {13, 13, 20, 20}, "bottom-right cell");

    // A dragged block, and the whole table.
    requireBounds(page, {{0, 1, 1, 2}}, {11, 12, 21, 22}, "a 2x2 block");
    requireBounds(page, {{0, 0, 2, 3}}, {10, 13, 20, 22}, "the whole page");

    // Two disjoint ranges (ctrl-drag) give one bounding box over both.
    requireBounds(page, {{0, 0, 0, 0}, {2, 3, 2, 3}}, {10, 13, 20, 22},
        "two opposite corners");

    // Uncovered cells do not count towards the bounds: the selection below
    // sweeps the whole page but only the middle row is covered.
    auto holed = makePage();
    for (int i = 10; i <= 13; ++i) {
        holed.covered[offsetOf(holed, i, 20)] = 0;
        holed.covered[offsetOf(holed, i, 22)] = 0;
    }
    holed.covered[offsetOf(holed, 10, 21)] = 0;
    requireBounds(holed, {{0, 0, 2, 3}}, {11, 13, 21, 21},
        "a selection over mostly uncovered cells");

    // ... and a selection that catches none of them marks nothing, the same
    // as clicking a single uncovered cell.
    require(!amrvis::qt::coveredSelectionBounds(holed, std::vector<
                amrvis::qt::CellRange>{{0, 0, 0, 3}}).has_value(),
        "an all-uncovered selection produced bounds");
    require(!amrvis::qt::coveredSelectionBounds(
                page, std::vector<amrvis::qt::CellRange>{}).has_value(),
        "an empty selection produced bounds");

    // A range past the page's edge is clamped rather than read out of bounds
    // (a stale selection can outlive the page it was made on).
    requireBounds(page, {{-5, -5, 99, 99}}, {10, 13, 20, 22},
        "a range past the edges");

    // An empty page has nothing to select.
    require(!amrvis::qt::coveredSelectionBounds(amrvis::DatasetPage{},
                std::vector<amrvis::qt::CellRange>{{0, 0, 0, 0}}).has_value(),
        "an empty page produced bounds");

    std::cout << "dataset selection OK\n";
    return 0;
}
