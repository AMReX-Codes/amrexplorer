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

// One level of 4 x 1 x 1 cells over [0,1] whose two grids overlap on cells
// 1 and 2 -- malformed, but the composition has to resolve it the way the
// slice's first match does, so the lowest grid index wins.
std::filesystem::path writeOverlapFixture(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nq\n"
        "3\n0.0\n0\n"
        "0.0 0.0 0.0\n1.0 0.25 0.25\n\n"
        "((0,0,0) (3,0,0) (0,0,0))\n"
        "0\n"
        "0.25 0.25 0.25\n"
        "0\n0\n"
        "0 2 0.0\n0\n"
        "0.0 0.75\n0.0 0.25\n0.0 0.25\n"
        "0.25 1.0\n0.0 0.25\n0.0 0.25\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(2 0\n((0,0,0) (2,0,0) (0,0,0))\n((1,0,0) (3,0,0) (0,0,0))\n)\n"
        "2\nFabOnDisk: Cell_D_00000 0\nFabOnDisk: Cell_D_00001 0\n\n"
        "2,1\n1.0,\n2.0,\n\n2,1\n1.0,\n2.0,\n\n");
    const std::vector<double> first(3, 1.0);
    const std::vector<double> second(3, 2.0);
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0,0) (2,0,0) (0,0,0))",
        first);
    writeFab(root / "Level_0" / "Cell_D_00001", "((1,0,0) (3,0,0) (0,0,0))",
        second);
    return root;
}

// 8 x 1 x 1 cells of dx = 0.1 split into two equal 4-cell grids, 1.0 then
// 2.0. Equal on purpose: the cache charges a block its vector's capacity, so
// only equal blocks let a test say "fits one of these but not both" without
// knowing what that capacity came out as.
std::filesystem::path writePairFixture(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nq\n"
        "3\n0.0\n0\n"
        "0.0 0.0 0.0\n0.8 0.1 0.1\n\n"
        "((0,0,0) (7,0,0) (0,0,0))\n"
        "0\n"
        "0.1 0.1 0.1\n"
        "0\n0\n"
        "0 2 0.0\n0\n"
        "0.0 0.4\n0.0 0.1\n0.0 0.1\n"
        "0.4 0.8\n0.0 0.1\n0.0 0.1\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(2 0\n((0,0,0) (3,0,0) (0,0,0))\n((4,0,0) (7,0,0) (0,0,0))\n)\n"
        "2\nFabOnDisk: Cell_D_00000 0\nFabOnDisk: Cell_D_00001 0\n\n"
        "2,1\n1.0,\n2.0,\n\n2,1\n1.0,\n2.0,\n\n");
    const std::vector<double> first(4, 1.0);
    const std::vector<double> second(4, 2.0);
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0,0) (3,0,0) (0,0,0))",
        first);
    writeFab(root / "Level_0" / "Cell_D_00001", "((4,0,0) (7,0,0) (0,0,0))",
        second);
    return root;
}

// 16 x 1 x 1 cells of dx = 0.01 whose domain sits at x = 1e14, split into
// two grids at cell 8. At that distance ulp is 0.015625 -- more than a cell
// -- so a block window derived by inverting the voxel-centre formula is off
// by whole voxels, beyond any fixed widening. The domain still spans ten
// representable values, so the plotfile itself remains well formed.
std::filesystem::path writeFarFromOriginFixture(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nq\n"
        "3\n0.0\n0\n"
        "1e14 0.0 0.0\n100000000000000.16 0.01 0.01\n\n"
        "((0,0,0) (15,0,0) (0,0,0))\n"
        "0\n"
        "0.01 0.01 0.01\n"
        "0\n0\n"
        "0 2 0.0\n0\n"
        "1e14 100000000000000.08\n0.0 0.01\n0.0 0.01\n"
        "100000000000000.08 100000000000000.16\n0.0 0.01\n0.0 0.01\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(2 0\n((0,0,0) (7,0,0) (0,0,0))\n((8,0,0) (15,0,0) (0,0,0))\n)\n"
        "2\nFabOnDisk: Cell_D_00000 0\nFabOnDisk: Cell_D_00001 0\n\n"
        "2,1\n1.0,\n2.0,\n\n2,1\n1.0,\n2.0,\n\n");
    const std::vector<double> lower(8, 1.0);
    const std::vector<double> upper(8, 2.0);
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0,0) (7,0,0) (0,0,0))",
        lower);
    writeFab(root / "Level_0" / "Cell_D_00001", "((8,0,0) (15,0,0) (0,0,0))",
        upper);
    return root;
}

// A 2 x 2 2-D plotfile, which the sampler must refuse rather than invent a
// third axis for.
std::filesystem::path writeTwoDimensionalFixture(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nq\n"
        "2\n0.0\n0\n"
        "0.0 0.0\n1.0 1.0\n\n"
        "((0,0) (1,1) (0,0))\n"
        "0\n"
        "0.5 0.5\n"
        "0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0) (1,1) (0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n0.0,\n\n1,1\n3.0,\n\n");
    const std::vector<double> values{0.0, 1.0, 2.0, 3.0};
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0) (1,1) (0,0))", values);
    return root;
}

// True when execute refuses `request` outright.
bool rejects(amrvis::VolumeQuery& query,
    const amrvis::VolumeSampleRequest& request)
{
    try {
        (void)query.execute(request);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

// The two-level fixture with the fine level holding 1e300, which no float
// can store: level 1 covers those voxels but puts nothing showable in them.
std::filesystem::path writeNanFineFixture(const std::filesystem::path& root)
{
    writeTwoLevelFixture(root);
    writeText(root / "Level_1" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((2,2,2) (5,5,5) (0,0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n1e300,\n\n1,1\n1e300,\n\n");
    std::array<double, 64> fine{};
    fine.fill(1.0e300);
    writeFab(root / "Level_1" / "Cell_D_00000", "((2,2,2) (5,5,5) (0,0,0))",
        fine);
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
        // And it still spends the budget.
        require(dims == (std::array<int, 3>{512, 512, 512}),
            "an oversized level did not spend its budget");

        // A budget past the cap buys no more than the cap: a server sizes an
        // inbound request with this before it has validated the budget, so
        // an unchecked one must not be able to ask for a bigger grid than
        // the sampler would ever allocate.
        require(amrvis::volumeGridDims(metadata, metadata.physicalDomain, 0,
                    std::numeric_limits<std::uint64_t>::max())
                == (std::array<int, 3>{512, 512, 512}),
            "an unchecked budget was not clamped to the cap");

        // A cube cannot tell the two ratios apart -- the trim loop drives
        // any overshoot back to equal axes -- so the aspect is what shows
        // that the scale came from the true product and not the saturated
        // one. 2^27 x 2^26 x 2^25 keeps its 4:2:1 at cbrt(2^-51) = 2^-17;
        // scaling by the saturated ratio instead overshoots to
        // 26010 x 13005 x 6502 and the trim loop equalises it to a cube.
        amrvis::DatasetMetadata skewed = metadata;
        skewed.levels[0].domain.upper
            = {{(1 << 27) - 1, (1 << 26) - 1, (1 << 25) - 1}};
        skewed.levels[0].cellSize = {{1.0 / (1 << 27), 1.0 / (1 << 26),
            1.0 / (1 << 25)}};
        require(amrvis::volumeGridDims(skewed, skewed.physicalDomain, 0, budget)
                == (std::array<int, 3>{1024, 512, 256}),
            "an oversized level did not scale by its true ratio");

        // A thin region still gets the whole budget on its one long axis.
        const auto thin = amrvis::volumeGridDims(metadata,
            box(0.0, 0.0, 0.0, 1.0, 1.0 / (1 << 27), 1.0 / (1 << 27)), 0, 64);
        require(thin == (std::array<int, 3>{64, 1, 1}),
            "a thin region did not spend its budget on the long axis");
    }

    // --- every refusal ----------------------------------------------------
    {
        amrvis::PlotfileDataset dataset(
            analyticRoot, amrvis::DatasetId{7}, 1024 * 1024);
        amrvis::VolumeQuery query(dataset);
        amrvis::VolumeSampleRequest good;
        good.dataset.value = 7;
        good.field.value = 0;
        good.maximumLevel = 0;
        good.region = box(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
        good.maximumVoxels = 64;
        require(!rejects(query, good), "a valid request was refused");

        auto request = good;
        request.maximumVoxels = 0;
        require(rejects(query, request), "a zero voxel budget was accepted");
        request.maximumVoxels = amrvis::maxVolumeVoxelBudget + 1;
        require(rejects(query, request),
            "a voxel budget past the cap was accepted");

        request = good;
        request.maximumLevel = -1;
        require(rejects(query, request), "a negative maximum level was accepted");

        request = good;
        request.component = 1;
        require(rejects(query, request), "a vector component was accepted");

        request = good;
        request.region = box(1.0, 0.0, 0.0, 0.0, 1.0, 1.0);
        require(rejects(query, request), "an inverted region was accepted");

        // A 2-D dataset has no volume to sample.
        const auto flatRoot = writeTwoDimensionalFixture(scratch / "flat");
        amrvis::PlotfileDataset flat(flatRoot, amrvis::DatasetId{8}, 1024 * 1024);
        amrvis::VolumeQuery flatQuery(flat);
        auto flatRequest = good;
        flatRequest.dataset.value = 8;
        require(rejects(flatQuery, flatRequest), "a 2-D dataset was accepted");

        // And the pure sizer gives the third axis one voxel rather than
        // measuring the region against the synthetic unit cell size a 2-D
        // level carries: 4 / 1.0 would otherwise read as four voxels deep.
        require(amrvis::volumeGridDims(flat.metadata(),
                    box(0.0, 0.0, 0.0, 1.0, 1.0, 4.0), 0, 64)
                == (std::array<int, 3>{2, 2, 1}),
            "the pure sizer invented a third dimension for a 2-D dataset");
    }

    // --- volumeGridDims is total -----------------------------------------
    {
        amrvis::PlotfileDataset dataset(
            analyticRoot, amrvis::DatasetId{9}, 1024 * 1024);
        const auto& metadata = dataset.metadata();
        const auto whole = box(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);

        // A NaN axis has no cell count to round to, and clamp cannot reject
        // it -- both of its comparisons are false -- so the conversion would
        // be undefined. It costs that axis one voxel and leaves the others
        // alone; converting the NaN instead poisons the whole product.
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        require(amrvis::volumeGridDims(metadata,
                    box(0.0, 0.0, 0.0, nan, 1.0, 1.0), 0, 64)
                == (std::array<int, 3>{1, 4, 4}),
            "a NaN on one axis was not confined to that axis");

        for (const auto& bad : {box(0.0, 0.0, 0.0, nan, 1.0, 1.0),
                 box(nan, nan, nan, nan, nan, nan),
                 box(1.0, 1.0, 1.0, 0.0, 0.0, 0.0),
                 box(0.0, 0.0, 0.0, 0.0, 0.0, 0.0)}) {
            const auto dims = amrvis::volumeGridDims(metadata, bad, 0, 64);
            require(dims[0] >= 1 && dims[1] >= 1 && dims[2] >= 1,
                "a malformed region produced a degenerate grid");
            require(static_cast<double>(dims[0]) * static_cast<double>(dims[1])
                        * static_cast<double>(dims[2])
                    <= 64.0,
                "a malformed region produced a grid over budget");
        }

        // The cbrt scale alone is usually enough -- 4 x 4 x 4 at budget 10
        // scales by 0.539 straight to 2 x 2 x 2 = 8, no trimming.
        require(amrvis::volumeGridDims(metadata, whole, 0, 10)
                == (std::array<int, 3>{2, 2, 2}),
            "an over-budget region was not scaled to fit");

        // This is the case where it is not: 4 x 4 x 1 native at budget 7
        // scales by cbrt(7/16) = 0.759 to 3 x 3 x 1, whose product is 9 --
        // still over. Only the trim loop lands it, and it takes the first
        // largest axis, so the answer is 2 x 3 x 1 rather than 3 x 2 x 1.
        require(amrvis::volumeGridDims(
                    metadata, box(0.0, 0.0, 0.0, 1.0, 1.0, 0.25), 0, 7)
                == (std::array<int, 3>{2, 3, 1}),
            "the trim loop did not land an over-budget grid");
    }

    // --- one block pinned at a time --------------------------------------
    {
        const auto pairRoot = writePairFixture(scratch / "pair");
        amrvis::VolumeSampleRequest request;
        request.field.value = 0;
        request.maximumLevel = 0;
        request.region = box(0.0, 0.0, 0.0, 0.8, 0.1, 0.1);
        request.maximumVoxels = 8;

        // What both blocks cost the cache, measured rather than assumed.
        std::uint64_t bothBytes = 0;
        {
            amrvis::PlotfileDataset roomy(
                pairRoot, amrvis::DatasetId{10}, 1024 * 1024);
            amrvis::VolumeQuery query(roomy);
            request.dataset.value = 10;
            (void)query.execute(request);
            bothBytes = roomy.cacheMetrics().residentBytes;
        }
        require(bothBytes > 0, "the pair fixture left nothing in the cache");

        // Three quarters of that holds either block on its own but never
        // both, so a query that pinned the level instead of one block at a
        // time would throw CacheBudgetExceeded here. Painting block by block
        // is what the design exists for; this is the test of it.
        amrvis::PlotfileDataset tight(
            pairRoot, amrvis::DatasetId{11}, bothBytes * 3 / 4);
        amrvis::VolumeQuery query(tight);
        request.dataset.value = 11;
        const auto grid = query.execute(request).grid;
        require(grid.dims == (std::array<int, 3>{8, 1, 1}),
            "the cache-pressure grid has the wrong shape");
        require(grid.coveredVoxels == 8,
            "a block was lost under cache pressure");
        for (std::size_t i = 0; i < 8; ++i) {
            require(grid.values[i] == (i < 4 ? 1.0F : 2.0F),
                "a voxel read the wrong block under cache pressure");
        }

        // And the budget really was too tight for two: holding both pins at
        // once throws, so the query above passed by not doing that.
        auto blockRequest = [&](int gridIndex) {
            amrvis::BlockRequest block;
            block.dataset = request.dataset;
            block.level = 0;
            block.gridIndex = gridIndex;
            block.field = request.field;
            return block;
        };
        auto threwOnPin = false;
        try {
            const auto pinned = tight.requestBlock(blockRequest(0));
            const auto second = tight.requestBlock(blockRequest(1));
        } catch (const amrvis::CacheBudgetExceeded&) {
            threwOnPin = true;
        }
        require(threwOnPin,
            "the cache-pressure budget was loose enough to pin both blocks");

        // A block holding no voxel centre is never read. At one voxel the
        // only centre is x = 0.4, which is grid 1's first cell, so grid 0 is
        // a candidate that costs nothing -- the point of settling the range
        // before the read rather than after it.
        amrvis::PlotfileDataset counted(
            pairRoot, amrvis::DatasetId{15}, 1024 * 1024);
        amrvis::VolumeQuery countedQuery(counted);
        request.dataset.value = 15;
        request.maximumVoxels = 1;
        const auto coarse = countedQuery.execute(request);
        require(coarse.grid.dims == (std::array<int, 3>{1, 1, 1})
                && coarse.grid.values[0] == 2.0F,
            "the one-voxel grid read the wrong grid");
        require(coarse.metrics.candidateBlocks == 2,
            "both grids should have been candidates");
        require(coarse.metrics.blocksRead == 1,
            "a grid with no voxel centre in it was read anyway");
    }

    // --- overlapping grids resolve to the lowest index --------------------
    {
        const auto overlapRoot = writeOverlapFixture(scratch / "overlap");
        amrvis::PlotfileDataset dataset(
            overlapRoot, amrvis::DatasetId{12}, 1024 * 1024);
        amrvis::VolumeQuery query(dataset);
        amrvis::VolumeSampleRequest request;
        request.dataset.value = 12;
        request.field.value = 0;
        request.maximumLevel = 0;
        request.region = box(0.0, 0.0, 0.0, 1.0, 0.25, 0.25);
        request.maximumVoxels = 4;

        const auto grid = query.execute(request).grid;
        require(grid.dims == (std::array<int, 3>{4, 1, 1}),
            "the overlap grid has the wrong shape");
        // Cells 1 and 2 are in both grids; grid 0 wins, as the slice's first
        // match does. Cell 3 is only in grid 1.
        const std::array<float, 4> expected{1.0F, 1.0F, 1.0F, 2.0F};
        for (std::size_t i = 0; i < 4; ++i) {
            require(grid.values[i] == expected[i],
                "an overlapped voxel did not resolve to the lowest grid index");
        }
    }

    // --- maximumLevel reports what contributed ----------------------------
    {
        amrvis::PlotfileDataset dataset(
            twoLevelRoot, amrvis::DatasetId{13}, 1024 * 1024);
        amrvis::VolumeQuery query(dataset);
        amrvis::VolumeSampleRequest request;
        request.dataset.value = 13;
        request.field.value = 0;
        request.maximumLevel = 1;
        request.maximumVoxels = 64;

        // Level 1 covers only the central [0.25,0.75]^3, so a corner region
        // is level-0 data however fine a level was asked for.
        request.region = box(0.0, 0.0, 0.0, 0.25, 0.25, 0.25);
        const auto corner = query.execute(request).grid;
        require(corner.coveredVoxels == corner.values.size(),
            "the corner region was not fully covered");
        require(corner.maximumLevel == 0,
            "maximumLevel reported a level that did not contribute");

        // The centre does reach level 1.
        request.region = box(0.25, 0.25, 0.25, 0.75, 0.75, 0.75);
        require(query.execute(request).grid.maximumLevel == 1,
            "maximumLevel missed the level that contributed");

        // ExactLevel over a corner level 1 does not cover: level 1 is the
        // only participating level and it paints nothing, so the grid is
        // empty and names no level. Reporting the requested level here would
        // claim fine data for a grid that has none at all.
        request.composition = amrvis::CompositionPolicy::ExactLevel;
        request.region = box(0.0, 0.0, 0.0, 0.25, 0.25, 0.25);
        const auto exactCorner = query.execute(request).grid;
        require(exactCorner.coveredVoxels == 0,
            "the corner is not covered at level 1");
        require(exactCorner.maximumLevel == 0,
            "an empty grid named a level that contributed nothing");
    }

    // --- a voxel and a slice pixel agree on the same point ----------------
    {
        // Both resolve a sample position over the same region, so a budgeted
        // voxel centre must land in the cell the slice reads at that point --
        // the composition the header promises is "the same as a slice".
        //
        // The seam fixture at 14 voxels is the case that tells the two
        // formulas apart: voxel 10's centre is 1.1999999999999999556 when the
        // extent is divided last and 1.2000000000000001776 when it is
        // pre-divided into a step, which is cell 11 in grid 0 against cell 12
        // in grid 1 -- a different value, not just a different index.
        const auto agreementRoot = writeSeamFixture(scratch / "agreement");
        amrvis::PlotfileDataset dataset(
            agreementRoot, amrvis::DatasetId{16}, 1024 * 1024);
        amrvis::VolumeQuery query(dataset);
        amrvis::VolumeSampleRequest request;
        request.dataset.value = 16;
        request.field.value = 0;
        request.maximumLevel = 0;
        request.region = box(0.0, 0.0, 0.0, 1.6, 0.1, 0.1);
        request.maximumVoxels = 14;
        const auto grid = query.execute(request).grid;
        require(grid.dims == (std::array<int, 3>{14, 1, 1}),
            "the agreement grid has the wrong shape");

        amrvis::SliceQuery slice(dataset);
        amrvis::SliceRequest sliceRequest;
        sliceRequest.dataset.value = 16;
        sliceRequest.field.value = 0;
        sliceRequest.maximumLevel = 0;
        sliceRequest.normalDirection = 2;
        sliceRequest.sampling = amrvis::SamplingPolicy::PiecewiseConstant;
        sliceRequest.outputSize = {grid.dims[0], grid.dims[1]};
        sliceRequest.visibleRegion = request.region;
        sliceRequest.maximumLevel = 0;
        for (int k = 0; k < grid.dims[2]; ++k) {
            sliceRequest.physicalPosition = request.region.lower[2]
                + (static_cast<double>(k) + 0.5)
                    * (request.region.upper[2] - request.region.lower[2])
                    / static_cast<double>(grid.dims[2]);
            const auto plane = slice.execute(sliceRequest).plane;
            for (int j = 0; j < grid.dims[1]; ++j) {
                for (int i = 0; i < grid.dims[0]; ++i) {
                    const auto pixel = static_cast<std::size_t>(i)
                        + static_cast<std::size_t>(grid.dims[0])
                            * static_cast<std::size_t>(j);
                    require(plane.valid[pixel] != 0,
                        "the slice left a pixel the volume covered");
                    require(plane.values[pixel] == grid.values[voxel(grid, i, j, k)],
                        "a voxel and the slice pixel over it disagree");
                }
            }
        }
    }

    // --- a level that writes only NaN contributed nothing ------------------
    {
        // Level 1 holds 1e300 throughout, so every voxel it covers lands in
        // the grid as NaN -- indistinguishable from a voxel no level
        // covered. It overwrote level 0 without putting anything showable
        // in its place, so the finest level that contributed is 0.
        const auto lostRoot = writeNanFineFixture(scratch / "nan-fine");
        amrvis::PlotfileDataset dataset(
            lostRoot, amrvis::DatasetId{17}, 1024 * 1024);
        amrvis::VolumeQuery query(dataset);
        amrvis::VolumeSampleRequest request;
        request.dataset.value = 17;
        request.field.value = 0;
        request.maximumLevel = 1;
        request.region = box(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
        request.maximumVoxels = 64;
        const auto grid = query.execute(request).grid;
        require(grid.dims == (std::array<int, 3>{4, 4, 4}),
            "the NaN-fine grid has the wrong shape");
        // Voxel centres are 0.125, 0.375, 0.625, 0.875; the fine level spans
        // [0.25, 0.75], so the middle two on each axis fall inside it.
        require(grid.coveredVoxels == 64 - 8,
            "the fine level's 1e300 cells were not reported as uncovered");
        for (int k = 0; k < 4; ++k) {
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 4; ++i) {
                    const auto inner = i >= 1 && i <= 2 && j >= 1 && j <= 2
                        && k >= 1 && k <= 2;
                    const auto value = grid.values[voxel(grid, i, j, k)];
                    require(inner ? std::isnan(value) : value == 1.0F,
                        "a voxel under the NaN-only fine level is wrong");
                }
            }
        }
        require(grid.maximumLevel == 0,
            "a level that wrote only NaN was reported as contributing");
    }

    // --- a domain far from the coordinate origin ---------------------------
    {
        // ulp(1e14) is 0.015625, more than a whole cell of 0.01, so a
        // window derived by inverting the centre formula lands voxels wrong
        // and leaves holes in data that has none. Bisecting on sampleIndex
        // is exact however far out the domain sits.
        const auto farRoot = writeFarFromOriginFixture(scratch / "far");
        amrvis::PlotfileDataset dataset(
            farRoot, amrvis::DatasetId{18}, 1024 * 1024);
        amrvis::VolumeQuery query(dataset);
        amrvis::VolumeSampleRequest request;
        request.dataset.value = 18;
        request.field.value = 0;
        request.maximumLevel = 0;
        request.region = dataset.metadata().physicalDomain;
        request.maximumVoxels = 16;

        const auto grid = query.execute(request).grid;
        require(grid.dims == (std::array<int, 3>{16, 1, 1}),
            "the far-from-origin grid has the wrong shape");
        // The point of the case: no voxel is dropped. At this distance
        // several voxel centres quantise onto the same coordinate, so the
        // voxel-to-cell map is not the identity -- which is exactly why the
        // expected value is taken from sampleIndex rather than from i.
        require(grid.coveredVoxels == 16,
            "voxels were lost in a domain far from the origin");
        const auto& level = dataset.metadata().levels[0];
        for (int i = 0; i < 16; ++i) {
            const auto centre = request.region.lower[0]
                + (static_cast<double>(i) + 0.5)
                    * (request.region.upper[0] - request.region.lower[0])
                    / 16.0;
            const auto cell = amrvis::sampleIndex(level, 0, centre);
            require(cell >= 0 && cell <= 15,
                "a far-from-origin voxel centre left the domain");
            require(grid.values[static_cast<std::size_t>(i)]
                    == (cell < 8 ? 1.0F : 2.0F),
                "a far-from-origin voxel read the wrong grid");
        }
    }

    // --- a thin region spends the budget it was given ---------------------
    {
        // Capping each axis at the budget before the cbrt shrinks an already
        // truncated axis a second time, so a pencil-thin region ends up with
        // a fraction of what it was allowed. The documented formula is the
        // native counts scaled by cbrt(budget / product) and trimmed.
        amrvis::DatasetMetadata metadata;
        metadata.dimension = 3;
        metadata.finestLevel = 0;
        metadata.physicalDomain = box(0.0, 0.0, 0.0, 200.0, 3.0, 3.0);
        amrvis::LevelMetadata level;
        level.domain.lower = {{0, 0, 0}};
        level.domain.upper = {{199, 2, 2}};
        level.cellSize = {{1.0, 1.0, 1.0}};
        metadata.levels.push_back(level);

        // 200 x 3 x 3 native at budget 64: cbrt(64/1800) = 0.329 gives
        // 65 x 1 x 1, which the trim lands at 64 x 1 x 1 -- the whole budget
        // on the axis that has the cells for it.
        require(amrvis::volumeGridDims(metadata, metadata.physicalDomain, 0, 64)
                == (std::array<int, 3>{64, 1, 1}),
            "a thin region did not spend its voxel budget");

        metadata.physicalDomain = box(0.0, 0.0, 0.0, 10000.0, 2.0, 2.0);
        metadata.levels[0].domain.upper = {{9999, 1, 1}};
        require(amrvis::volumeGridDims(metadata, metadata.physicalDomain, 0, 512)
                == (std::array<int, 3>{512, 1, 1}),
            "a long thin region did not spend its voxel budget");
    }

    std::error_code removeError;
    std::filesystem::remove_all(scratch, removeError);
    return 0;
}
