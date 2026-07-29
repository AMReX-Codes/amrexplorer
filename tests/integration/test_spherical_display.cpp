// Drives executeFrameLoad over a small synthetic 2-D spherical (coord 2)
// plotfile in each SphericalDisplay mode and checks the produced raster and
// display region: r-theta leaves the logical raster untouched, theta-r
// transposes it (dimensions, bounds, and content), and R-Z warps it into the
// sector's physical bounding box with transparent pixels outside the sector.

#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>
#include <amrexplorer/render2d/Palette.hpp>

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

bool approx(double a, double b, double tol = 1e-12)
{
    return std::abs(a - b) <= tol;
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

// One display per mode from the same plotfile; everything but the display
// layout uses the spec defaults (whole domain, finest native output size).
amrvis::SliceDisplayResult loadDisplay(
    const std::filesystem::path& root, amrvis::SphericalDisplay mode)
{
    amrvis::FrameSliceSpec spec;
    spec.palette = amrvis::builtinPalette(amrvis::BuiltinPalette::Rainbow);
    spec.sphericalDisplay = mode;
    auto result = amrvis::executeFrameLoad(root, amrvis::DatasetId{1}, spec,
        64ULL * 1024 * 1024, amrvis::StopToken{});
    require(result.displays.size() == 1, "one display per 2-D frame load");
    return std::move(result.displays.front());
}

} // namespace

int main()
{
    using namespace amrvis;

    // 16 (r) x 8 (theta) cells over r in [1, 2], theta in [0, 0.5]; coordinate
    // system 2 (spherical). phi(i, j) = (i + j) / 2, i-fastest, so the raster
    // content is asymmetric and any axis mix-up shows up in the pixels.
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("amrexplorer-spherical-display-" + std::to_string(unique));
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nphi\n"
        "2\n0.0\n0\n"
        "1.0 0.0\n2.0 0.5\n"
        "\n"
        "((0,0) (15,7) (0,0))\n"
        "0\n"
        "0.0625 0.0625\n"
        "2\n0\n"
        "0 1 0.0\n0\n"
        "1.0 2.0\n0.0 0.5\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0) (15,7) (0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n0.0,\n\n1,1\n11.0,\n\n");
    std::vector<double> values;
    for (int j = 0; j <= 7; ++j) {
        for (int i = 0; i <= 15; ++i) {
            values.push_back(0.5 * static_cast<double>(i + j));
        }
    }
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0) (15,7) (0,0))", values);

    const auto rtheta = loadDisplay(root, SphericalDisplay::RTheta);
    const auto thetar = loadDisplay(root, SphericalDisplay::ThetaR);
    const auto rz = loadDisplay(root, SphericalDisplay::RZ);

    // r-theta: the logical raster untouched, one pixel per cell, logical bounds.
    require(rtheta.coordinateSystem == 2, "coordinate system recorded");
    require(rtheta.sphericalDisplay == SphericalDisplay::RTheta,
        "r-theta mode recorded");
    require(rtheta.image.width == 16 && rtheta.image.height == 8,
        "r-theta raster is the logical grid");
    require(approx(rtheta.displayRegion.lower[0], 1.0)
            && approx(rtheta.displayRegion.upper[0], 2.0)
            && approx(rtheta.displayRegion.lower[1], 0.0)
            && approx(rtheta.displayRegion.upper[1], 0.5),
        "r-theta display region is the logical region");

    // theta-r: transposed dimensions, axis-swapped bounds, transposed pixels.
    require(thetar.image.width == 8 && thetar.image.height == 16,
        "theta-r raster is transposed");
    require(approx(thetar.displayRegion.lower[0], 0.0)
            && approx(thetar.displayRegion.upper[0], 0.5)
            && approx(thetar.displayRegion.lower[1], 1.0)
            && approx(thetar.displayRegion.upper[1], 2.0),
        "theta-r display region swaps the axes");
    for (int row = 0; row < rtheta.image.height; ++row) {
        for (int col = 0; col < rtheta.image.width; ++col) {
            const auto logical = rtheta.image.rgba[
                static_cast<std::size_t>(row) * 16 + static_cast<std::size_t>(col)];
            const auto transposed = thetar.image.rgba[
                static_cast<std::size_t>(col) * 8 + static_cast<std::size_t>(row)];
            require(logical == transposed, "theta-r pixels are the transpose");
        }
    }

    // R-Z: warped into the sector's physical bounding box at the supersampled
    // resolution, with opaque pixels inside the sector and transparent ones
    // outside (the sector never fills its axis-aligned bounding box).
    const auto bounds = sphericalDisplayBounds(rtheta.slice.plane.physicalRegion);
    require(approx(rz.displayRegion.lower[0], bounds.lower[0])
            && approx(rz.displayRegion.upper[0], bounds.upper[0])
            && approx(rz.displayRegion.lower[1], bounds.lower[1])
            && approx(rz.displayRegion.upper[1], bounds.upper[1]),
        "R-Z display region is the sector bounding box");
    require(rz.image.width > 16 && rz.image.height > 8,
        "R-Z raster is supersampled beyond the logical grid");
    std::size_t opaque = 0;
    std::size_t transparent = 0;
    for (const auto pixel : rz.image.rgba) {
        if (((pixel >> 24) & 0xFFU) == 0U) {
            ++transparent;
        } else {
            ++opaque;
        }
    }
    require(opaque > 0, "R-Z raster has sector interior");
    require(transparent > 0, "R-Z raster has transparent exterior");

    std::filesystem::remove_all(root);
    std::cout << "spherical_display OK\n";
    return 0;
}
