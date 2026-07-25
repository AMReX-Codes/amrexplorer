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

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return 0;
}
