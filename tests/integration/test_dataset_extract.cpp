// Coverage for extractDatasetLevel (the Qt-free core behind the Dataset
// window). The dataset window's table model is a thin lazy view over this
// extract, so pinning the extract's index bounds, value/coverage layout,
// min/max, and truncation locks the data the window displays.

#include <amrexplorer/io/PlotfileDataset.hpp>
#include "../../src/qt/DatasetExtract.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view realDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create fixture text");
    output << text;
}

void writeFab(const std::filesystem::path& path, std::string_view box,
    std::span<const double> values)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create fixture FAB");
    output << "FAB " << realDescriptor << box << " 1\n";
    output.write(reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(values.size() * sizeof(double)));
}

// Single-level 2-D plotfile, 4x4, domain (0,0)-(3,3), unit cells, with two
// grids that together leave one cell of the domain uncovered so coverage is
// exercised: grid A covers (0,0)-(3,2) and grid B covers (0,3)-(2,3); cell
// (3,3) is covered by no grid. phi = 10*j + i over covered cells.
void writeSplitCoveragePlotfile(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nphi\n"
        "2\n0.0\n0\n"
        "0.0 0.0\n4.0 4.0\n\n"
        "((0,0) (3,3) (0,0))\n"
        "0\n1.0 1.0\n0\n0\n"
        "0 2 0.0\n0\n"
        "0.0 4.0\n0.0 3.0\n"
        "0.0 3.0\n3.0 4.0\n"
        "Level_0/Cell\n");
    // Two grids; no per-block statistics recorded (version-1 _H still fine).
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(2 0\n"
        "((0,0) (3,2) (0,0))\n"
        "((0,3) (2,3) (0,0))\n"
        ")\n"
        "2\n"
        "FabOnDisk: Cell_D_00000 0\n"
        "FabOnDisk: Cell_D_00001 0\n"
        "\n"
        // Per-block minima then maxima, one row per box (grid A: 0..23,
        // grid B: 30..32). Version-1 requires a boxCount x components matrix.
        "2,1\n0.0,\n30.0,\n\n2,1\n23.0,\n32.0,\n");
    std::vector<double> gridA;   // (0,0)-(3,2): j = 0..2, i = 0..3
    for (int j = 0; j <= 2; ++j) {
        for (int i = 0; i <= 3; ++i) {
            gridA.push_back(10.0 * j + i);
        }
    }
    std::vector<double> gridB;   // (0,3)-(2,3): j = 3, i = 0..2
    for (int i = 0; i <= 2; ++i) {
        gridB.push_back(10.0 * 3 + i);
    }
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0) (3,2) (0,0))", gridA);
    writeFab(root / "Level_0" / "Cell_D_00001", "((0,3) (2,3) (0,0))", gridB);
}

amrvis::RealBox box2d(double lo, double hi)
{
    amrvis::RealBox box;
    box.lower = {{lo, lo, 0.0}};
    box.upper = {{hi, hi, 0.0}};
    return box;
}

// Single-level 3-D plotfile, domain (0,0,0)-(3,3,3), unit cells, split into
// two grids along x: grid 0 = (0,0,0)-(1,3,3), grid 1 = (2,0,0)-(3,3,3).
// Grid 1 is OFF-ORIGIN on x (lower = {2,0,0}) — the shape a finer AMR level's
// patch has, and the case that exposed the wrong-axis offset bug in the
// Dataset window: a yz slice iterates y/z but the offset used them as x/y.
// q = 100*x + 10*y + z everywhere.
void writeSplitXPlotfile3d(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nq\n"
        "3\n0.0\n0\n"
        "0.0 0.0 0.0\n4.0 4.0 4.0\n\n"
        "((0,0,0) (3,3,3) (0,0,0))\n"
        "0\n1.0 1.0 1.0\n0\n0\n"
        "0 2 0.0\n0\n"
        "0.0 2.0\n0.0 4.0\n0.0 4.0\n"
        "2.0 4.0\n0.0 4.0\n0.0 4.0\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(2 0\n"
        "((0,0,0) (1,3,3) (0,0,0))\n"
        "((2,0,0) (3,3,3) (0,0,0))\n"
        ")\n"
        "2\n"
        "FabOnDisk: Cell_D_00000 0\n"
        "FabOnDisk: Cell_D_00001 0\n"
        "\n"
        "2,1\n0.0,\n200.0,\n\n2,1\n133.0,\n333.0,\n");
    const auto blockValues = [](int xLo, int xHi) {
        std::vector<double> values;  // axis-0 (x) fastest, then y, then z
        for (int z = 0; z <= 3; ++z) {
            for (int y = 0; y <= 3; ++y) {
                for (int x = xLo; x <= xHi; ++x) {
                    values.push_back(100.0 * x + 10.0 * y + z);
                }
            }
        }
        return values;
    };
    const auto gridA = blockValues(0, 1);
    const auto gridB = blockValues(2, 3);
    writeFab(root / "Level_0" / "Cell_D_00000",
        "((0,0,0) (1,3,3) (0,0,0))", gridA);
    writeFab(root / "Level_0" / "Cell_D_00001",
        "((2,0,0) (3,3,3) (0,0,0))", gridB);
}

// The 2-D split-coverage plotfile with grid B's FAB header box disagreeing
// with the catalog: the Header/Cell_H say (0,3)-(2,3), the FAB on disk says
// (0,3)-(1,3) and holds two values. A v1 VisMF layout permits this and nothing
// upstream cross-checks the two boxes; reading it must fail, not alias.
void writeMismatchedFabPlotfile(const std::filesystem::path& root)
{
    writeSplitCoveragePlotfile(root);
    const std::vector<double> gridB{30.0, 31.0};
    writeFab(root / "Level_0" / "Cell_D_00001", "((0,3) (1,3) (0,0))", gridB);
}

// The 3-D split-x plotfile with grid 1's FAB header box one z-plane short of
// its catalog box: catalog (2,0,0)-(3,3,3), FAB (2,0,0)-(3,3,2), 48 values.
void writeMismatchedFabPlotfile3d(const std::filesystem::path& root)
{
    writeSplitXPlotfile3d(root);
    std::vector<double> gridB;
    for (int z = 0; z <= 2; ++z) {
        for (int y = 0; y <= 3; ++y) {
            for (int x = 2; x <= 3; ++x) {
                gridB.push_back(100.0 * x + 10.0 * y + z);
            }
        }
    }
    writeFab(root / "Level_0" / "Cell_D_00001",
        "((2,0,0) (3,3,2) (0,0,0))", gridB);
}

template <typename Function>
bool throwsRuntimeError(Function&& function)
{
    try {
        function();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

int main()
{
    using namespace amrvis;
    using namespace amrvis::qt;

    const auto unique
        = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("amrexplorer-dataset-extract-" + std::to_string(unique));
    writeSplitCoveragePlotfile(root);

    PlotfileDataset dataset(root, DatasetId{1}, 1ULL << 20);

    // --- full domain: extent, per-cell values, coverage, min/max -----------
    {
        const auto extract = extractDatasetLevel(dataset, FieldId{0}, 0,
            box2d(0.0, 4.0), 1, 0.0, datasetExtractMaxExtent);
        require(extract.nx == 4 && extract.ny == 4,
            "full-domain extent is wrong");
        require(extract.lower[0] == 0 && extract.lower[1] == 0
                && extract.upper[0] == 3 && extract.upper[1] == 3,
            "full-domain index box is wrong");
        require(!extract.truncatedX && !extract.truncatedY,
            "full domain should not be truncated");

        // Value layout: first in-plane axis fastest, j ascending.
        const auto at = [&](int i, int j) {
            return extract.values[static_cast<std::size_t>(i)
                + static_cast<std::size_t>(extract.nx)
                    * static_cast<std::size_t>(j)];
        };
        const auto covered = [&](int i, int j) {
            return extract.covered[static_cast<std::size_t>(i)
                + static_cast<std::size_t>(extract.nx)
                    * static_cast<std::size_t>(j)] != 0;
        };
        require(at(0, 0) == 0.0 && at(3, 0) == 3.0 && at(0, 2) == 20.0
                && at(2, 3) == 32.0,
            "extracted values do not match phi = 10*j + i");
        require(covered(0, 0) && covered(2, 3),
            "cells inside a grid are not covered");
        require(!covered(3, 3),
            "the domain corner touched by no grid must be uncovered");
        require(extract.hasFiniteValues && extract.minimum == 0.0
                && extract.maximum == 32.0,
            "min/max over covered finite cells is wrong");
    }

    // --- the model's (row,column) mapping: row 0 is the highest j ----------
    {
        const auto extract = extractDatasetLevel(dataset, FieldId{0}, 0,
            box2d(0.0, 4.0), 1, 0.0, datasetExtractMaxExtent);
        // Reproduce LevelTableModel::cellOffset: row 0 -> j = upper[1].
        const auto cellValue = [&](int row, int column) {
            const auto valueRow
                = static_cast<std::size_t>(extract.ny - 1 - row);
            return extract.values[static_cast<std::size_t>(column)
                + static_cast<std::size_t>(extract.nx) * valueRow];
        };
        // Header labels the model would show: column c -> lower[0]+c,
        // row r -> upper[1]-r. Row 0 (j=3) column 2 is phi(2,3)=32.
        require(cellValue(0, 2) == 32.0,
            "row-0/highest-j model mapping is wrong");
        require(cellValue(3, 0) == 0.0,
            "bottom-row model mapping is wrong");
    }

    // --- a sub-region clips to the covered index box -----------------------
    {
        const auto extract = extractDatasetLevel(dataset, FieldId{0}, 0,
            box2d(1.0, 3.0), 1, 0.0, datasetExtractMaxExtent);
        require(extract.lower[0] == 1 && extract.lower[1] == 1
                && extract.upper[0] == 2 && extract.upper[1] == 2,
            "sub-region index box is wrong");
        require(extract.nx == 2 && extract.ny == 2,
            "sub-region extent is wrong");
    }

    // --- truncation caps the extent and flags the axis ---------------------
    {
        const auto extract = extractDatasetLevel(dataset, FieldId{0}, 0,
            box2d(0.0, 4.0), 1, 0.0, /*maxExtent=*/2);
        require(extract.nx == 2 && extract.ny == 2,
            "extent was not capped at maxExtent");
        require(extract.truncatedX && extract.truncatedY,
            "truncation flags were not set");
        require(extract.lower[0] == 0 && extract.upper[0] == 1,
            "truncation kept the wrong cells");
    }

    // --- a region missing the domain yields an empty extract ---------------
    {
        const auto extract = extractDatasetLevel(dataset, FieldId{0}, 0,
            box2d(10.0, 12.0), 1, 0.0, datasetExtractMaxExtent);
        require(extract.nx == 0 && extract.ny == 0,
            "a region outside the domain should extract nothing");
    }

    // --- 3-D yz slice through an off-origin grid ---------------------------
    // Regression: extraction offsets must map the in-plane and slice indices
    // to their real axes using the FAB box. When a grid is off-origin (a
    // finer level's patch; here grid 1 at x in [2,3]), a yz slice previously
    // used the y coordinate as x and read the wrong cell — with the checked
    // offset that surfaced as "FAB point precedes its indexed box".
    {
        const auto root3d = std::filesystem::temp_directory_path()
            / ("amrexplorer-dataset-extract-3d-" + std::to_string(unique));
        writeSplitXPlotfile3d(root3d);
        PlotfileDataset dataset3d(root3d, DatasetId{2}, 1ULL << 20);

        RealBox full;
        full.lower = {{0.0, 0.0, 0.0}};
        full.upper = {{4.0, 4.0, 4.0}};
        // normal = 0 (yz plane), slice at the x=3 cell center (3.5).
        const auto yz = extractDatasetLevel(dataset3d, FieldId{0}, 0, full,
            /*normalAxis=*/0, /*slicePosition=*/3.5, datasetExtractMaxExtent);
        require(yz.nx == 4 && yz.ny == 4, "yz-slice extent is wrong");
        require(yz.sliceIndex == 3, "yz slice did not land on the x=3 cell");
        // In-plane axis 0 is y, axis 1 is z; value at (y, z) for x=3 is
        // 300 + 10*y + z. Grid 1 (x in [2,3]) covers the whole slice.
        const auto at = [&](int y, int z) {
            return yz.values[static_cast<std::size_t>(y)
                + static_cast<std::size_t>(yz.nx)
                    * static_cast<std::size_t>(z)];
        };
        require(yz.covered[0] != 0, "yz slice cell (0,0) is uncovered");
        require(at(0, 0) == 300.0, "yz slice corner value is wrong");
        require(at(1, 2) == 312.0, "yz slice value maps the wrong axes");
        require(at(3, 3) == 333.0, "yz slice far value is wrong");

        std::error_code cleanup3d;
        std::filesystem::remove_all(root3d, cleanup3d);
    }

    // --- a FAB that does not cover its catalog box is refused, not aliased ---
    // 2-D: a whole-domain page touches grid B; 3-D: any z-slice reads grid 1,
    // and its FAB is short on z. Both go through the page path's per-block
    // requireBlockPayload, including the 3-D normal-axis handling.
    {
        const auto badRoot = std::filesystem::temp_directory_path()
            / ("amrexplorer-dataset-extract-bad2d-" + std::to_string(unique));
        writeMismatchedFabPlotfile(badRoot);
        PlotfileDataset badDataset(badRoot, DatasetId{3}, 1ULL << 20);
        require(throwsRuntimeError([&] {
            static_cast<void>(extractDatasetLevel(badDataset, FieldId{0}, 0,
                box2d(0.0, 4.0), 1, 0.0, datasetExtractMaxExtent));
        }), "a 2-D FAB smaller than its catalog box was read");
        std::error_code cleanupBad;
        std::filesystem::remove_all(badRoot, cleanupBad);
    }
    {
        const auto badRoot = std::filesystem::temp_directory_path()
            / ("amrexplorer-dataset-extract-bad3d-" + std::to_string(unique));
        writeMismatchedFabPlotfile3d(badRoot);
        PlotfileDataset badDataset(badRoot, DatasetId{4}, 1ULL << 20);
        RealBox full;
        full.lower = {{0.0, 0.0, 0.0}};
        full.upper = {{4.0, 4.0, 4.0}};
        require(throwsRuntimeError([&] {
            static_cast<void>(extractDatasetLevel(badDataset, FieldId{0}, 0,
                full, /*normalAxis=*/2, /*slicePosition=*/0.5,
                datasetExtractMaxExtent));
        }), "a 3-D FAB short on the normal axis was read");
        // Grid 0's FAB is intact, but a page over the whole plane reads
        // grid 1 too, so it must fail even where grid 0 alone would do.
        require(throwsRuntimeError([&] {
            static_cast<void>(extractDatasetLevel(badDataset, FieldId{0}, 0,
                full, /*normalAxis=*/0, /*slicePosition=*/3.5,
                datasetExtractMaxExtent));
        }), "a yz page over the short FAB was read");
        std::error_code cleanupBad;
        std::filesystem::remove_all(badRoot, cleanupBad);
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return 0;
}
