#pragma once

// Turning a block of selected Dataset-window table cells into the sample
// indices it stands for. Kept out of the window (and out of Qt's selection
// types) so the index arithmetic -- which flips rows against j and has to skip
// the samples no grid covers -- can be tested on a synthetic page.

#include <amrexplorer/data/DatasetPage.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>

namespace amrvis::qt {

// A contiguous block of table cells in the window's own row/column terms:
// column 0 is the page's lowest i, and row 0 is its *highest* j (see
// LevelTableModel::headerData).
struct CellRange {
    int topRow = 0;
    int leftColumn = 0;
    int bottomRow = 0;
    int rightColumn = 0;
};

// Sample indices along the two in-plane axes, inclusive.
struct SelectedSampleBox {
    int iLow = 0;
    int iHigh = 0;
    int jLow = 0;
    int jHigh = 0;
    friend bool operator==(const SelectedSampleBox&, const SelectedSampleBox&)
        = default;
};

// The sample bounds of the covered cells the ranges cover, or nullopt when
// they cover none. Only covered cells count: a selection dragged out past the
// grids at this level marks the data the user actually caught, not the empty
// cells they swept over -- the same rule as clicking one, which does nothing
// on an uncovered cell. The result is a bounding box, so uncovered samples
// inside it are included; a rectangle is what the image can highlight.
[[nodiscard]] inline std::optional<SelectedSampleBox> coveredSelectionBounds(
    const DatasetPage& page, std::span<const CellRange> ranges)
{
    if (page.nx <= 0 || page.ny <= 0) {
        return std::nullopt;
    }
    int minRow = page.ny;
    int maxRow = -1;
    int minColumn = page.nx;
    int maxColumn = -1;
    for (const auto& range : ranges) {
        const auto top = std::max(range.topRow, 0);
        const auto bottom = std::min(range.bottomRow, page.ny - 1);
        const auto left = std::max(range.leftColumn, 0);
        const auto right = std::min(range.rightColumn, page.nx - 1);
        for (int row = top; row <= bottom; ++row) {
            // Row 0 is the highest j; the values run j ascending with the
            // first in-plane axis fastest (LevelTableModel::cellOffset).
            const auto valueRow = static_cast<std::size_t>(page.ny - 1 - row);
            for (int column = left; column <= right; ++column) {
                const auto offset = static_cast<std::size_t>(column)
                    + static_cast<std::size_t>(page.nx) * valueRow;
                if (page.covered[offset] == 0) {
                    continue;
                }
                minRow = std::min(minRow, row);
                maxRow = std::max(maxRow, row);
                minColumn = std::min(minColumn, column);
                maxColumn = std::max(maxColumn, column);
            }
        }
    }
    if (maxRow < 0) {
        return std::nullopt;
    }
    // Rows run opposite to j, so the bottom row carries the lowest j.
    return SelectedSampleBox{page.lower[0] + minColumn,
        page.lower[0] + maxColumn, page.upper[1] - maxRow,
        page.upper[1] - minRow};
}

} // namespace amrvis::qt
