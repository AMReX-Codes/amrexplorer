#include <amrexplorer/query/SliceQuery.hpp>

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
    require(static_cast<bool>(output), "could not create slice fixture text");
    output << text;
}

void writeFab(const std::filesystem::path& path, std::string_view box,
    std::span<const double> values)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create slice fixture FAB");
    output << "FAB " << realDescriptor << box << " 1\n";
    output.write(reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(values.size() * sizeof(double)));
}

// 2-D linear fixture field: phi(i, j) = (i + j) / 2 in the level's cell
// indices, laid out i-fastest per grid like the FAB payload.
std::vector<double> linearField2d(int i0, int i1, int j0, int j1)
{
    std::vector<double> values;
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            values.push_back(0.5 * static_cast<double>(i + j));
        }
    }
    return values;
}

// 3-D linear fixture field: q(i, j, k) = (i + j + k) / 9 over 4x4x4 cells.
std::vector<double> linearField3d()
{
    std::vector<double> values;
    for (int k = 0; k <= 3; ++k) {
        for (int j = 0; j <= 3; ++j) {
            for (int i = 0; i <= 3; ++i) {
                values.push_back(static_cast<double>(i + j + k) / 9.0);
            }
        }
    }
    return values;
}

} // namespace

int main()
{
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("amrexplorer-slice-query-" + std::to_string(unique));
    std::filesystem::create_directories(root / "Level_0");
    std::filesystem::create_directories(root / "Level_1");

    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nphi\n"
        "2\n0.0\n1\n"
        "0.0 0.0\n1.0 1.0\n2\n"
        "((0,0) (3,3) (0,0))\n"
        "((0,0) (7,7) (0,0))\n"
        "0 0\n"
        "0.25 0.25\n0.125 0.125\n"
        "0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n"
        "1 1 0.0\n0\n"
        "0.25 0.75\n0.25 0.75\n"
        "Level_1/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0) (3,3) (0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n1.0,\n\n1,1\n1.0,\n\n");
    writeText(root / "Level_1" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((2,2) (5,5) (0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n2.0,\n\n1,1\n2.0,\n\n");

    std::array<double, 16> coarse{};
    std::array<double, 16> fine{};
    coarse.fill(1.0);
    fine.fill(2.0);
    writeFab(root / "Level_0" / "Cell_D_00000",
        "((0,0) (3,3) (0,0))", coarse);
    writeFab(root / "Level_1" / "Cell_D_00000",
        "((2,2) (5,5) (0,0))", fine);

    amrvis::PlotfileDataset dataset(root, amrvis::DatasetId{9}, 1024 * 1024);
    amrvis::SliceRequest request;
    request.dataset.value = 9;
    request.field.value = 0;
    request.normalDirection = 1;
    request.visibleRegion = {{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 0.0}}};
    request.maximumLevel = 1;
    request.outputSize = {4, 4};

    amrvis::SliceQuery query(dataset);
    const auto composite = query.execute(request);
    require(composite.metrics.blocksRead == 2, "slice did not read the two intersecting blocks");
    require(composite.metrics.cacheHits == 0, "first slice unexpectedly hit cache");
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const auto offset = static_cast<std::size_t>(x + 4 * y);
            const bool coveredByFine = x >= 1 && x <= 2 && y >= 1 && y <= 2;
            require(composite.plane.valid[offset] == 1, "composite slice left a coarse hole");
            require(composite.plane.values[offset] == (coveredByFine ? 2.0F : 1.0F),
                "fine-over-coarse value mismatch");
            require(composite.plane.sourceLevel[offset] == (coveredByFine ? 1 : 0),
                "source-level mask mismatch");
        }
    }

    const auto cached = query.execute(request);
    require(cached.metrics.blocksRead == 0 && cached.metrics.cacheHits == 2,
        "repeated slice did not reuse both blocks");
    require(cached.metrics.payloadBytesRead == 0, "cached slice performed payload I/O");

    const auto doubledField = dataset.addDerivedField({
        .name = "double_phi",
        .expression = "2*phi"
    });
    auto derivedRequest = request;
    derivedRequest.field = doubledField;
    const auto derived = query.execute(derivedRequest);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const auto offset = static_cast<std::size_t>(x + 4 * y);
            const bool coveredByFine = x >= 1 && x <= 2 && y >= 1 && y <= 2;
            require(derived.plane.valid[offset] == 1,
                "derived slice left a covered cell invalid");
            require(derived.plane.values[offset]
                    == (coveredByFine ? 4.0F : 2.0F),
                "derived field did not flow through the slice query");
        }
    }

    request.composition = amrvis::CompositionPolicy::ExactLevel;
    const auto exact = query.execute(request);
    require(exact.plane.valid[0] == 0, "exact-level slice filled a fine-level hole");
    require(exact.plane.valid[5] == 1 && exact.plane.values[5] == 2.0F,
        "exact-level slice missed fine data");

    // Linear sampling on the constant AMR fixture: across the fine/coarse
    // boundary the sample ramps smoothly between the coarse value 1.0 and
    // the fine value 2.0 instead of stepping. At 8x8 output, pixel (1, 3)
    // sits a quarter of a coarse cell into the transition, so bilinear
    // gives 1.25 where piecewise quantizes to 1.0; the far side mirrors it.
    amrvis::SliceRequest ramp;
    ramp.dataset.value = 9;
    ramp.field.value = 0;
    ramp.normalDirection = 1;
    ramp.visibleRegion = {{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 0.0}}};
    ramp.maximumLevel = 1;
    ramp.outputSize = {8, 8};
    const auto quantized = query.execute(ramp);
    ramp.sampling = amrvis::SamplingPolicy::Linear;
    const auto smooth = query.execute(ramp);
    require(smooth.metrics.candidateBlocks == quantized.metrics.candidateBlocks,
        "linear sampling on the whole domain touched extra blocks");
    for (int offset = 0; offset < 64; ++offset) {
        require(smooth.plane.valid[static_cast<std::size_t>(offset)] == 1,
            "linear slice left a hole");
        const auto value = smooth.plane.values[static_cast<std::size_t>(offset)];
        require(std::isfinite(value) && value >= 1.0F && value <= 2.0F,
            "linear slice value outside the bracketing cell range");
    }
    const auto at = [](const amrvis::ScalarPlane& plane, int x, int y) {
        return plane.values[static_cast<std::size_t>(x + 8 * y)];
    };
    require(at(quantized.plane, 1, 3) == 1.0F && at(quantized.plane, 6, 3) == 1.0F,
        "piecewise slice unexpectedly smooth at the boundary");
    require(std::fabs(at(smooth.plane, 1, 3) - 1.25F) < 1e-6F,
        "linear slice did not ramp across the fine/coarse boundary");
    require(std::fabs(at(smooth.plane, 6, 3) - 1.25F) < 1e-6F,
        "linear slice ramp is not symmetric at the far boundary");
    require(at(smooth.plane, 3, 3) == 2.0F && at(smooth.plane, 4, 3) == 2.0F,
        "linear slice inside the fine grid lost the fine value");
    require(smooth.plane.sourceLevel[1 + 8 * 3] == 0
            && smooth.plane.sourceLevel[3 + 8 * 3] == 1,
        "linear slice source level does not track the covering level");

    // Linear sampling of a globally linear field must reproduce it exactly
    // at interior pixels. Single level, two grids side by side, storing
    // phi(i, j) = (i + j) / 2 in level-0 cell indices, i.e.
    // phi(x, y) = 2(x + y) - 1/2 in physical coordinates.
    const auto linearRoot = std::filesystem::temp_directory_path()
        / ("amrexplorer-slice-linear-" + std::to_string(unique));
    std::filesystem::create_directories(linearRoot / "Level_0");
    writeText(linearRoot / "Header",
        "HyperCLaw-V1.1\n"
        "1\nphi\n"
        "2\n0.0\n0\n"
        "0.0 0.0\n1.0 1.0\n"
        "\n"
        "((0,0) (3,3) (0,0))\n"
        "0\n"
        "0.25 0.25\n"
        "0\n0\n"
        "0 2 0.0\n0\n"
        "0.0 0.5\n0.0 1.0\n"
        "0.5 1.0\n0.0 1.0\n"
        "Level_0/Cell\n");
    writeText(linearRoot / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(2 0\n"
        "((0,0) (1,3) (0,0))\n"
        "((2,0) (3,3) (0,0))\n"
        ")\n"
        "2\n"
        "FabOnDisk: Cell_D_00000 0\n"
        "FabOnDisk: Cell_D_00001 0\n\n"
        "2,1\n0.0,\n1.0,\n\n"
        "2,1\n2.0,\n3.0,\n\n");
    writeFab(linearRoot / "Level_0" / "Cell_D_00000",
        "((0,0) (1,3) (0,0))", linearField2d(0, 1, 0, 3));
    writeFab(linearRoot / "Level_0" / "Cell_D_00001",
        "((2,0) (3,3) (0,0))", linearField2d(2, 3, 0, 3));

    amrvis::PlotfileDataset linearDataset(
        linearRoot, amrvis::DatasetId{21}, 1024 * 1024);
    amrvis::SliceQuery linearQuery(linearDataset);
    amrvis::SliceRequest linearRequest;
    linearRequest.dataset.value = 21;
    linearRequest.field.value = 0;
    linearRequest.normalDirection = 1;
    linearRequest.visibleRegion = {{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 0.0}}};
    linearRequest.maximumLevel = 0;
    linearRequest.outputSize = {16, 16};
    linearRequest.sampling = amrvis::SamplingPolicy::Linear;
    const auto linearPlane = linearQuery.execute(linearRequest);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const auto offset = static_cast<std::size_t>(x + 16 * y);
            require(linearPlane.plane.valid[offset] == 1,
                "linear sampling left a hole in the covered domain");
            require(linearPlane.plane.sourceLevel[offset] == 0,
                "linear sampling reported the wrong covering level");
            const auto px = (static_cast<double>(x) + 0.5) / 16.0;
            const auto py = (static_cast<double>(y) + 0.5) / 16.0;
            const auto analytic = 2.0 * (px + py) - 0.5;
            const auto value = static_cast<double>(linearPlane.plane.values[offset]);
            if (x >= 4 && x <= 11 && y >= 4 && y <= 11) {
                require(std::fabs(value - analytic) < 1e-5,
                    "linear sampling does not reproduce the linear field");
            } else {
                // The outermost half-cell ring has uncovered bracketing
                // centers and clamps to the nearest covered center, so its
                // values stay inside the field's range without being exact.
                require(value >= -1e-5 && value <= 3.0 + 1e-5,
                    "clamped edge sample left the field range");
            }
        }
    }

    // The linear plan reads a one-cell halo: a region touching only the
    // left grid reads one block piecewise but both blocks linearly.
    amrvis::SliceRequest half;
    half.dataset.value = 21;
    half.field.value = 0;
    half.normalDirection = 1;
    half.visibleRegion = {{{0.0, 0.0, 0.0}}, {{0.5, 1.0, 0.0}}};
    half.maximumLevel = 0;
    half.outputSize = {8, 16};
    linearDataset.clearUnpinnedCache();
    const auto halfPiecewise = linearQuery.execute(half);
    require(halfPiecewise.metrics.candidateBlocks == 1
            && halfPiecewise.metrics.blocksRead == 1
            && halfPiecewise.metrics.cacheHits == 0,
        "piecewise plan touched the grid outside the region");
    half.sampling = amrvis::SamplingPolicy::Linear;
    linearDataset.clearUnpinnedCache();
    const auto halfLinear = linearQuery.execute(half);
    require(halfLinear.metrics.candidateBlocks == 2
            && halfLinear.metrics.blocksRead == 2
            && halfLinear.metrics.cacheHits == 0,
        "linear plan did not read the one-cell halo block");

    // A 3-D linear field sliced through a cell center: q(i, j, k) =
    // (i + j + k) / 9, so the k = 1 slice is 4(x + y) / 9 physically.
    const auto linear3dRoot = std::filesystem::temp_directory_path()
        / ("amrexplorer-slice-linear3d-" + std::to_string(unique));
    std::filesystem::create_directories(linear3dRoot / "Level_0");
    writeText(linear3dRoot / "Header",
        "HyperCLaw-V1.1\n"
        "1\nq\n"
        "3\n0.0\n0\n"
        "0.0 0.0 0.0\n1.0 1.0 1.0\n"
        "\n"
        "((0,0,0) (3,3,3) (0,0,0))\n"
        "0\n"
        "0.25 0.25 0.25\n"
        "0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n");
    writeText(linear3dRoot / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n"
        "((0,0,0) (3,3,3) (0,0,0))\n"
        ")\n"
        "1\n"
        "FabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n0.0,\n\n"
        "1,1\n1.0,\n\n");
    writeFab(linear3dRoot / "Level_0" / "Cell_D_00000",
        "((0,0,0) (3,3,3) (0,0,0))", linearField3d());

    amrvis::PlotfileDataset dataset3d(
        linear3dRoot, amrvis::DatasetId{22}, 1024 * 1024);
    amrvis::SliceQuery query3d(dataset3d);
    amrvis::SliceRequest slice3d;
    slice3d.dataset.value = 22;
    slice3d.field.value = 0;
    slice3d.normalDirection = 2;
    slice3d.physicalPosition = 0.375;  // cell center of k = 1
    slice3d.visibleRegion = {{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 1.0}}};
    slice3d.maximumLevel = 0;
    slice3d.outputSize = {16, 16};
    slice3d.sampling = amrvis::SamplingPolicy::Linear;
    const auto plane3d = query3d.execute(slice3d);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const auto offset = static_cast<std::size_t>(x + 16 * y);
            require(plane3d.plane.valid[offset] == 1,
                "3-D linear slice left a hole");
            const auto px = (static_cast<double>(x) + 0.5) / 16.0;
            const auto py = (static_cast<double>(y) + 0.5) / 16.0;
            const auto analytic = 4.0 * (px + py) / 9.0;
            const auto value = static_cast<double>(plane3d.plane.values[offset]);
            if (x >= 4 && x <= 11 && y >= 4 && y <= 11) {
                require(std::fabs(value - analytic) < 1e-5,
                    "3-D linear slice does not reproduce the linear field");
            }
        }
    }

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(linearRoot);
    std::filesystem::remove_all(linear3dRoot);
    return 0;
}
