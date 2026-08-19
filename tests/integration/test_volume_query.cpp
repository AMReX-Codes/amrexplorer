// VolumeQuery: the AMR hierarchy sampled onto a bounded uniform grid.
// Synthesised plotfiles (as test_slice_query does): a single-level 4x4x4
// analytic field q(i, j, k) = (i + j + k) / 9 pins exact values, the voxel
// budget, sub-regions and the grid geometry; a two-level fixture pins the
// composition (fine over coarse, ExactLevel, a coarser maximum level).

#include <amrexplorer/query/VolumeQuery.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
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

void require(bool condition, const char* message)
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

// The single-level analytic fixture: 4^3 cells over [0,1]^3, cell size 0.25.
std::filesystem::path writeAnalyticFixture(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nq\n"
        "3\n0.0\n0\n"
        "0.0 0.0 0.0\n1.0 1.0 1.0\n\n"
        "((0,0,0) (3,3,3) (0,0,0))\n"
        "0\n"
        "0.25 0.25 0.25\n"
        "0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0,0) (3,3,3) (0,0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n0.0,\n\n1,1\n1.0,\n\n");
    std::vector<double> values;
    for (int k = 0; k <= 3; ++k) {
        for (int j = 0; j <= 3; ++j) {
            for (int i = 0; i <= 3; ++i) {
                values.push_back(static_cast<double>(i + j + k) / 9.0);
            }
        }
    }
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0,0) (3,3,3) (0,0,0))",
        values);
    return root;
}

// Two levels over [0,1]^3: coarse 4^3 = 1.0 everywhere, a fine 4^3 block
// (level-1 indices 2..5, i.e. the central [0.25,0.75]^3) = 2.0.
std::filesystem::path writeTwoLevelFixture(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    std::filesystem::create_directories(root / "Level_1");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nphi\n"
        "3\n0.0\n1\n"
        "0.0 0.0 0.0\n1.0 1.0 1.0\n2\n"
        "((0,0,0) (3,3,3) (0,0,0))\n"
        "((0,0,0) (7,7,7) (0,0,0))\n"
        "0 0\n"
        "0.25 0.25 0.25\n0.125 0.125 0.125\n"
        "0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n"
        "1 1 0.0\n0\n"
        "0.25 0.75\n0.25 0.75\n0.25 0.75\n"
        "Level_1/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0,0) (3,3,3) (0,0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n1.0,\n\n1,1\n1.0,\n\n");
    writeText(root / "Level_1" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((2,2,2) (5,5,5) (0,0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n2.0,\n\n1,1\n2.0,\n\n");
    std::array<double, 64> coarse{};
    std::array<double, 64> fine{};
    coarse.fill(1.0);
    fine.fill(2.0);
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0,0) (3,3,3) (0,0,0))",
        coarse);
    writeFab(root / "Level_1" / "Cell_D_00000", "((2,2,2) (5,5,5) (0,0,0))",
        fine);
    return root;
}

// Two blocks over 16 x 1 x 1 cells of dx = 0.1, split at i = 12: block 0
// (x < 1.2) is 1.0, block 1 is 2.0. The seam is the point of the fixture --
// 0.1 and 1.2 are not exact in binary, so a voxel centre that lands on the
// boundary exposes any rounding the block window does.
std::filesystem::path writeSeamFixture(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nq\n"
        "3\n0.0\n0\n"
        "0.0 0.0 0.0\n1.6 0.1 0.1\n\n"
        "((0,0,0) (15,0,0) (0,0,0))\n"
        "0\n"
        "0.1 0.1 0.1\n"
        "0\n0\n"
        "0 2 0.0\n0\n"
        "0.0 1.2\n0.0 0.1\n0.0 0.1\n"
        "1.2 1.6\n0.0 0.1\n0.0 0.1\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(2 0\n((0,0,0) (11,0,0) (0,0,0))\n((12,0,0) (15,0,0) (0,0,0))\n)\n"
        "2\nFabOnDisk: Cell_D_00000 0\nFabOnDisk: Cell_D_00001 0\n\n"
        "2,1\n1.0,\n2.0,\n\n2,1\n1.0,\n2.0,\n\n");
    const std::vector<double> lower(12, 1.0);
    const std::vector<double> upper(4, 2.0);
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0,0) (11,0,0) (0,0,0))",
        lower);
    writeFab(root / "Level_0" / "Cell_D_00001", "((12,0,0) (15,0,0) (0,0,0))",
        upper);
    return root;
}

// 4^3 cells of dx = 0.25 whose index domain starts at -2, over [-0.5,0.5]^3.
// Cell (i,j,k) holds i + 10*j + 100*k, so a voxel that silently fell back to
// another cell is visible in the value. One cell carries 1e300, which no
// float can hold.
std::filesystem::path writeNegativeIndexFixture(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nq\n"
        "3\n0.0\n0\n"
        "-0.5 -0.5 -0.5\n0.5 0.5 0.5\n\n"
        "((-2,-2,-2) (1,1,1) (0,0,0))\n"
        "0\n"
        "0.25 0.25 0.25\n"
        "0\n0\n"
        "0 1 0.0\n0\n"
        "-0.5 0.5\n-0.5 0.5\n-0.5 0.5\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((-2,-2,-2) (1,1,1) (0,0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n-222.0,\n\n1,1\n1e300,\n\n");
    std::vector<double> values;
    for (int k = -2; k <= 1; ++k) {
        for (int j = -2; j <= 1; ++j) {
            for (int i = -2; i <= 1; ++i) {
                values.push_back(i == 1 && j == 1 && k == 1
                    ? 1.0e300
                    : static_cast<double>(i + 10 * j + 100 * k));
            }
        }
    }
    writeFab(root / "Level_0" / "Cell_D_00000", "((-2,-2,-2) (1,1,1) (0,0,0))",
        values);
    return root;
}

std::size_t voxel(const amrvis::VolumeGrid& grid, int i, int j, int k)
{
    return static_cast<std::size_t>(i)
        + static_cast<std::size_t>(grid.dims[0])
            * (static_cast<std::size_t>(j)
                + static_cast<std::size_t>(grid.dims[1]) * static_cast<std::size_t>(k));
}

amrvis::RealBox box(double x0, double y0, double z0, double x1, double y1,
    double z1)
{
    amrvis::RealBox result;
    result.lower = {{x0, y0, z0}};
    result.upper = {{x1, y1, z1}};
    return result;
}

} // namespace

int main()
{
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto scratch = std::filesystem::temp_directory_path()
        / ("amrexplorer-volume-query-" + std::to_string(unique));
    const auto analyticRoot = writeAnalyticFixture(scratch / "analytic");
    const auto twoLevelRoot = writeTwoLevelFixture(scratch / "two-level");

    // --- the analytic single-level field ---------------------------------
    {
        amrvis::PlotfileDataset dataset(
            analyticRoot, amrvis::DatasetId{3}, 1024 * 1024);
        amrvis::VolumeQuery query(dataset);
        amrvis::VolumeSampleRequest request;
        request.dataset.value = 3;
        request.field.value = 0;
        request.maximumLevel = 0;
        request.region = box(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);

        // Whole domain within budget: native 4x4x4, every voxel exact.
        request.maximumVoxels = 64;
        const auto native = query.execute(request);
        require(native.grid.dims == (std::array<int, 3>{4, 4, 4}),
            "the native grid is not one voxel per cell");
        require(native.grid.values.size() == 64
                && native.grid.coveredVoxels == 64
                && native.grid.maximumLevel == 0
                && native.grid.region == request.region,
            "the native grid's shape or coverage is wrong");
        require(native.metrics.candidateBlocks == 1
                && native.metrics.blocksRead == 1
                && native.metrics.cacheHits == 0,
            "the single block was not read exactly once");
        for (int k = 0; k < 4; ++k) {
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 4; ++i) {
                    const auto expected
                        = static_cast<float>(static_cast<double>(i + j + k) / 9.0);
                    require(native.grid.values[voxel(native.grid, i, j, k)]
                            == expected,
                        "a native voxel does not carry its cell's value");
                }
            }
        }
        // A second execute hits the cache.
        const auto again = query.execute(request);
        require(again.metrics.cacheHits == 1 && again.metrics.blocksRead == 0
                && again.grid.values == native.grid.values,
            "the second sample did not come from the block cache");

        // Over budget: 4^3 = 64 into a budget of 8 -> 2x2x2, each voxel the
        // cell that contains its centre (0.25 -> cell 1, 0.75 -> cell 3).
        request.maximumVoxels = 8;
        const auto coarse = query.execute(request);
        require(coarse.grid.dims == (std::array<int, 3>{2, 2, 2})
                && coarse.grid.coveredVoxels == 8,
            "the budget did not scale the grid down uniformly");
        for (int k = 0; k < 2; ++k) {
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const auto cell = [](int index) { return index == 0 ? 1 : 3; };
                    const auto expected = static_cast<float>(
                        static_cast<double>(cell(i) + cell(j) + cell(k)) / 9.0);
                    require(coarse.grid.values[voxel(coarse.grid, i, j, k)]
                            == expected,
                        "a downsampled voxel is not the cell under its centre");
                }
            }
        }
        // A budget of 1 collapses to a single voxel: the centre cell (2,2,2).
        request.maximumVoxels = 1;
        const auto single = query.execute(request);
        require(single.grid.dims == (std::array<int, 3>{1, 1, 1})
                && single.grid.values[0] == static_cast<float>(6.0 / 9.0),
            "a one-voxel grid is not the centre cell");

        // A sub-region: [0.5,1] x [0,1] x [0.25,0.75] at native pitch is
        // 2 x 4 x 2 voxels over cells i=2..3, j=0..3, k=1..2.
        request.maximumVoxels = 4096;
        request.region = box(0.5, 0.0, 0.25, 1.0, 1.0, 0.75);
        const auto sub = query.execute(request);
        require(sub.grid.dims == (std::array<int, 3>{2, 4, 2})
                && sub.grid.coveredVoxels == 16,
            "the sub-region grid has the wrong shape");
        for (int k = 0; k < 2; ++k) {
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const auto expected = static_cast<float>(
                        static_cast<double>((i + 2) + j + (k + 1)) / 9.0);
                    require(sub.grid.values[voxel(sub.grid, i, j, k)] == expected,
                        "a sub-region voxel does not carry its cell's value");
                }
            }
        }
        // volumeGridDims agrees with what execute produced, and is pure.
        require(amrvis::volumeGridDims(dataset.metadata(), request.region, 0, 4096)
                    == sub.grid.dims
                && amrvis::volumeGridDims(dataset.metadata(),
                       box(0.0, 0.0, 0.0, 1.0, 1.0, 1.0), 0, 8)
                    == (std::array<int, 3>{2, 2, 2}),
            "volumeGridDims disagrees with the sampled grid");

        // Refusals: a region outside the domain, a bad field, a foreign
        // dataset id, and a cancelled token.
        bool threw = false;
        try {
            request.region = box(0.5, 0.0, 0.0, 1.5, 1.0, 1.0);
            (void)query.execute(request);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "a region outside the domain was accepted");
        threw = false;
        try {
            request.region = box(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
            request.field.value = 5;
            (void)query.execute(request);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "an unknown field was accepted");
        request.field.value = 0;
        threw = false;
        try {
            request.dataset.value = 4;
            (void)query.execute(request);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "a foreign dataset id was accepted");
        request.dataset.value = 3;
        amrvis::StopSource stop;
        stop.request_stop();
        threw = false;
        try {
            (void)query.execute(request, stop.get_token());
        } catch (const amrvis::ReadCancelled&) {
            threw = true;
        }
        require(threw, "a cancelled sample did not throw ReadCancelled");
    }

    // --- two levels: composition ------------------------------------------
    {
        amrvis::PlotfileDataset dataset(
            twoLevelRoot, amrvis::DatasetId{5}, 1024 * 1024);
        amrvis::VolumeQuery query(dataset);
        amrvis::VolumeSampleRequest request;
        request.dataset.value = 5;
        request.field.value = 0;
        request.region = box(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
        request.maximumVoxels = 4096;

        // Finest available up to level 1: an 8^3 grid, 2.0 under the fine
        // block (level-1 cells 2..5 on every axis), 1.0 elsewhere.
        request.maximumLevel = 1;
        const auto composite = query.execute(request);
        require(composite.grid.dims == (std::array<int, 3>{8, 8, 8})
                && composite.grid.coveredVoxels == 512
                && composite.grid.maximumLevel == 1
                && composite.metrics.candidateBlocks == 2
                && composite.metrics.blocksRead == 2,
            "the two-level composite has the wrong shape or reads");
        for (int k = 0; k < 8; ++k) {
            for (int j = 0; j < 8; ++j) {
                for (int i = 0; i < 8; ++i) {
                    const bool fine = i >= 2 && i <= 5 && j >= 2 && j <= 5
                        && k >= 2 && k <= 5;
                    require(composite.grid.values[voxel(composite.grid, i, j, k)]
                            == (fine ? 2.0F : 1.0F),
                        "the fine level did not overwrite exactly its own cells");
                }
            }
        }
        // ExactLevel 1: only the fine block; the rest uncovered (NaN).
        request.composition = amrvis::CompositionPolicy::ExactLevel;
        const auto exact = query.execute(request);
        require(exact.grid.dims == (std::array<int, 3>{8, 8, 8})
                && exact.grid.coveredVoxels == 64
                && exact.metrics.candidateBlocks == 1,
            "ExactLevel did not restrict the sample to the fine level");
        for (int k = 0; k < 8; ++k) {
            for (int j = 0; j < 8; ++j) {
                for (int i = 0; i < 8; ++i) {
                    const bool fine = i >= 2 && i <= 5 && j >= 2 && j <= 5
                        && k >= 2 && k <= 5;
                    const auto value = exact.grid.values[voxel(exact.grid, i, j, k)];
                    require(fine ? value == 2.0F : std::isnan(value),
                        "ExactLevel left the wrong voxels covered");
                }
            }
        }
        // Maximum level 0: the coarse grid alone, 4^3 of 1.0.
        request.composition = amrvis::CompositionPolicy::FinestAvailable;
        request.maximumLevel = 0;
        const auto coarseOnly = query.execute(request);
        require(coarseOnly.grid.dims == (std::array<int, 3>{4, 4, 4})
                && coarseOnly.grid.coveredVoxels == 64
                && coarseOnly.grid.maximumLevel == 0
                && coarseOnly.metrics.candidateBlocks == 1,
            "a coarser maximum level still read the fine block");
        for (const auto value : coarseOnly.grid.values) {
            require(value == 1.0F, "the coarse-only grid is not uniformly coarse");
        }
        // A budget below the fine resolution: 2^3 with centres at 0.25 and
        // 0.75. The fine block spans [0.25, 0.75): the centre 0.25 is its
        // first cell (fine cell 2) and reads 2.0, while 0.75 is the coarse
        // cell past its edge and reads 1.0 -- fine still overwrites coarse
        // at whatever pitch the budget allows, and only where it exists.
        request.maximumLevel = 1;
        request.maximumVoxels = 8;
        const auto budgeted = query.execute(request);
        require(budgeted.grid.dims == (std::array<int, 3>{2, 2, 2})
                && budgeted.grid.coveredVoxels == 8,
            "the budgeted composite has the wrong shape");
        for (int k = 0; k < 2; ++k) {
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const bool fine = i == 0 && j == 0 && k == 0;
                    require(budgeted.grid.values[voxel(budgeted.grid, i, j, k)]
                            == (fine ? 2.0F : 1.0F),
                        "a budgeted voxel did not read the level under its centre");
                }
            }
        }
    }

    // --- a voxel centre on a block seam ----------------------------------
    {
        const auto seamRoot = writeSeamFixture(scratch / "seam");
        amrvis::PlotfileDataset dataset(
            seamRoot, amrvis::DatasetId{5}, 1024 * 1024);
        amrvis::VolumeQuery query(dataset);
        amrvis::VolumeSampleRequest request;
        request.dataset.value = 5;
        request.field.value = 0;
        request.maximumLevel = 0;
        request.region = box(0.0, 0.0, 0.0, 1.6, 0.1, 0.1);
        request.maximumVoxels = 2;

        // Voxel 1's centre is 1.5 * (1.6/2), which rounds to a hair above 1.2
        // and so belongs to cell 12 -- the upper block's first cell. Inverting
        // the centre formula puts it a hair outside that block's window, so a
        // window that trusts its own arithmetic paints neither block here and
        // leaves a hole in data that has none.
        const auto grid = query.execute(request).grid;
        require(grid.dims == (std::array<int, 3>{2, 1, 1}),
            "the seam grid has the wrong shape");
        require(grid.coveredVoxels == 2,
            "a voxel on the block seam was left uncovered");
        require(grid.values[0] == 1.0F && grid.values[1] == 2.0F,
            "a seam voxel read the wrong block");
    }

    // --- a level whose index domain starts below zero ---------------------
    {
        const auto negativeRoot = writeNegativeIndexFixture(scratch / "negative");
        amrvis::PlotfileDataset dataset(
            negativeRoot, amrvis::DatasetId{6}, 1024 * 1024);
        amrvis::VolumeQuery query(dataset);
        amrvis::VolumeSampleRequest request;
        request.dataset.value = 6;
        request.field.value = 0;
        request.maximumLevel = 0;
        request.region = box(-0.5, -0.5, -0.5, 0.5, 0.5, 0.5);
        request.maximumVoxels = 64;

        // Negative cell indices are ordinary indices: all 64 voxels are
        // covered, and each reads the cell its centre falls in rather than
        // being mistaken for a miss.
        const auto grid = query.execute(request).grid;
        require(grid.dims == (std::array<int, 3>{4, 4, 4}),
            "the negative-index grid has the wrong shape");
        require(grid.coveredVoxels == 63,
            "the negative-index grid covered the wrong number of voxels");
        for (int k = 0; k < 4; ++k) {
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 4; ++i) {
                    if (i == 3 && j == 3 && k == 3) {
                        continue;
                    }
                    const auto expected = static_cast<float>(
                        (i - 2) + 10 * (j - 2) + 100 * (k - 2));
                    require(grid.values[voxel(grid, i, j, k)] == expected,
                        "a voxel over a negative index read the wrong cell");
                }
            }
        }

        // 1e300 is finite as a double but has no float to round to, and the
        // grid promises NaN for anything it cannot hold.
        require(std::isnan(grid.values[voxel(grid, 3, 3, 3)]),
            "a value outside float's range was not reported as uncovered");
    }

    // --- the budget holds for a level too large to multiply out -----------
    {
        // Each axis may spend the whole budget -- a thin region legitimately
        // does -- so three of them can overflow the product that the budget
        // test and the buffer size are both taken from.
        amrvis::DatasetMetadata metadata;
        metadata.dimension = 3;
        metadata.finestLevel = 0;
        metadata.physicalDomain = box(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
        amrvis::LevelMetadata level;
        level.domain.lower = {{0, 0, 0}};
        level.domain.upper = {{(1 << 27) - 1, (1 << 27) - 1, (1 << 27) - 1}};
        level.cellSize = {{1.0 / (1 << 27), 1.0 / (1 << 27), 1.0 / (1 << 27)}};
        metadata.levels.push_back(level);

        const auto budget = static_cast<std::uint64_t>(
            amrvis::maxVolumeVoxelBudget);
        const auto dims = amrvis::volumeGridDims(
            metadata, metadata.physicalDomain, 0, budget);
        const auto product = static_cast<double>(dims[0])
            * static_cast<double>(dims[1]) * static_cast<double>(dims[2]);
        require(product <= static_cast<double>(budget),
            "an oversized level was returned over the voxel budget");
        // And it still spends the budget: shrinking by the true ratio keeps
        // the cube a cube, where shrinking by the saturated one collapses it.
        require(dims == (std::array<int, 3>{512, 512, 512}),
            "an oversized level did not scale by its true ratio");

        // A thin region still gets the whole budget on its one long axis.
        const auto thin = amrvis::volumeGridDims(metadata,
            box(0.0, 0.0, 0.0, 1.0, 1.0 / (1 << 27), 1.0 / (1 << 27)), 0, 64);
        require(thin == (std::array<int, 3>{64, 1, 1}),
            "a thin region did not spend its budget on the long axis");
    }

    std::error_code removeError;
    std::filesystem::remove_all(scratch, removeError);
    return 0;
}
