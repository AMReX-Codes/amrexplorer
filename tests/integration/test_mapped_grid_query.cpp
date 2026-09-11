// Node positions of the materialized mapped-grid fixture
// (tests/data/plotfile_3d_mapped) through LocalDatasetSession. The
// displacement recipe is the fixture materializer's, in node indices with
// imax = jmax = kmax = 4 (keep in sync with fixture_materializer/main.cpp):
//   nu_x = nu_y = 0
//   nu_z(i, j, k) = 0.125 * (1 - k/kmax) * (i + j) / (imax + jmax)
// The domain is [0,1]^3 with dx = 0.25, so node (i, j, k) sits at
// (0.25 i, 0.25 j, 0.25 k + nu_z).

#include <amrexplorer/data/LocalDatasetSession.hpp>

#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

constexpr double dx = 0.25;
constexpr int imax = 4;
constexpr int jmax = 4;
constexpr int kmax = 4;

double nuZ(int i, int j, int k)
{
    return 0.125 * (1.0 - static_cast<double>(k) / kmax)
        * static_cast<double>(i + j) / static_cast<double>(imax + jmax);
}

bool near(double a, double b, double tolerance = 1e-12)
{
    return std::abs(a - b) <= tolerance;
}

amrvis::MappedGridPlaneRequest fullRequest(int normal, double position,
    int width, int height)
{
    amrvis::MappedGridPlaneRequest request;
    request.dataset = amrvis::DatasetId{1};
    request.normalDirection = normal;
    request.physicalPosition = position;
    request.visibleRegion.lower = {{0.0, 0.0, 0.0}};
    request.visibleRegion.upper = {{1.0, 1.0, 1.0}};
    request.maximumLevel = 0;
    request.outputSize = {width, height};
    return request;
}

std::size_t node(const amrvis::MappedGridPlane& plane, int column, int row)
{
    return static_cast<std::size_t>(row) * static_cast<std::size_t>(plane.width)
        + static_cast<std::size_t>(column);
}

void testMappedFixture(const std::filesystem::path& fixture)
{
    amrvis::LocalDatasetSession session(
        fixture, amrvis::DatasetId{1}, 64ULL << 20U);
    require(session.metadata().hasMappedGrid, "fixture metadata carries the grid");
    require(session.supportsMappedGrid(), "fixture session supports the mapped grid");

    // y-normal slice through cell j = 1: the plane spans x (a) and z (b); z
    // nodes average layers j = 1 and j = 2.
    {
        const auto plane = session.requestMappedGridPlane(
            fullRequest(1, 0.375, 4, 4));
        require(plane.width == 5 && plane.height == 5,
            "a 4x4 raster has 5x5 nodes");
        require(plane.a.size() == 25 && plane.b.size() == 25,
            "node arrays match the node count");
        bool ok = true;
        for (int k = 0; k <= 4; ++k) {
            for (int i = 0; i <= 4; ++i) {
                const auto n = node(plane, i, k);
                ok = ok && near(plane.a[n], dx * i);
                const auto expected
                    = dx * k + 0.5 * (nuZ(i, 1, k) + nuZ(i, 2, k));
                ok = ok && near(plane.b[n], expected);
            }
        }
        require(ok, "y-normal nodes are x uniform and z averaged over layers 1,2");
        require(plane.physicalRegion.upper[0] == 1.0,
            "plane keeps the logical region");
    }

    // x-normal slice through the last cell (i = 3): layers 3 and 4.
    {
        const auto plane = session.requestMappedGridPlane(
            fullRequest(0, 0.875, 4, 4));
        bool ok = true;
        for (int k = 0; k <= 4; ++k) {
            for (int j = 0; j <= 4; ++j) {
                const auto n = node(plane, j, k);
                ok = ok && near(plane.a[n], dx * j);
                ok = ok
                    && near(plane.b[n],
                        dx * k + 0.5 * (nuZ(3, j, k) + nuZ(4, j, k)));
            }
        }
        require(ok, "x-normal nodes average layers 3 and 4 at the domain edge");
    }

    // z-normal slice: x and y are undisplaced.
    {
        const auto plane = session.requestMappedGridPlane(
            fullRequest(2, 0.125, 4, 4));
        bool ok = true;
        for (int j = 0; j <= 4; ++j) {
            for (int i = 0; i <= 4; ++i) {
                const auto n = node(plane, i, j);
                ok = ok && near(plane.a[n], dx * i) && near(plane.b[n], dx * j);
            }
        }
        require(ok, "z-normal nodes are the uniform x-y grid");
    }

    // A raster coarser than the grid (2x2 over the whole domain): nodes at
    // every other grid node take that node's displacement.
    {
        const auto plane = session.requestMappedGridPlane(
            fullRequest(1, 0.375, 2, 2));
        require(plane.width == 3 && plane.height == 3, "a 2x2 raster has 3x3 nodes");
        bool ok = true;
        for (int row = 0; row <= 2; ++row) {
            for (int column = 0; column <= 2; ++column) {
                const auto n = node(plane, column, row);
                const int i = 2 * column;
                const int k = 2 * row;
                ok = ok && near(plane.a[n], dx * i);
                ok = ok
                    && near(plane.b[n],
                        dx * k + 0.5 * (nuZ(i, 1, k) + nuZ(i, 2, k)));
            }
        }
        require(ok, "a coarse raster samples every other node");
    }

    // A raster finer than the grid (8x8 over the whole domain, as a coarse
    // level is shown at the finest level's pitch): a node between two stored
    // nodes takes the interpolated displacement. The recipe is bilinear in
    // (i, k), so the interpolated value is the recipe at the half index.
    {
        const auto plane = session.requestMappedGridPlane(
            fullRequest(1, 0.375, 8, 8));
        require(plane.width == 9 && plane.height == 9, "an 8x8 raster has 9x9 nodes");
        const auto nuAt = [](double i, int j, double k) {
            return 0.125 * (1.0 - k / kmax) * (i + j) / (imax + jmax);
        };
        bool ok = true;
        for (int row = 0; row <= 8; ++row) {
            for (int column = 0; column <= 8; ++column) {
                const auto n = node(plane, column, row);
                const double i = 0.5 * column;
                const double k = 0.5 * row;
                ok = ok && near(plane.a[n], dx * i);
                ok = ok
                    && near(plane.b[n],
                        dx * k + 0.5 * (nuAt(i, 1, k) + nuAt(i, 2, k)));
            }
        }
        require(ok, "a fine raster interpolates between stored nodes");
    }

    // A sub-region: x in [0.25, 0.75], z in [0, 1] at native resolution.
    {
        auto request = fullRequest(1, 0.375, 2, 4);
        request.visibleRegion.lower[0] = 0.25;
        request.visibleRegion.upper[0] = 0.75;
        const auto plane = session.requestMappedGridPlane(request);
        require(plane.width == 3 && plane.height == 5, "sub-region node counts");
        bool ok = true;
        for (int k = 0; k <= 4; ++k) {
            for (int column = 0; column <= 2; ++column) {
                const auto n = node(plane, column, k);
                const int i = 1 + column;
                ok = ok && near(plane.a[n], dx * i);
                ok = ok
                    && near(plane.b[n],
                        dx * k + 0.5 * (nuZ(i, 1, k) + nuZ(i, 2, k)));
            }
        }
        require(ok, "sub-region nodes start at the region's lower edge");
    }

    // Requests the session must refuse.
    {
        auto request = fullRequest(1, 0.375, 4, 4);
        request.dataset = amrvis::DatasetId{2};
        bool threw = false;
        try {
            static_cast<void>(session.requestMappedGridPlane(request));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "the wrong dataset id is refused");

        request = fullRequest(1, 0.375, 0, 4);
        threw = false;
        try {
            static_cast<void>(session.requestMappedGridPlane(request));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "a zero output size is refused");
    }
}

void testPlainPlotfile(const std::filesystem::path& plotfile)
{
    amrvis::LocalDatasetSession session(
        plotfile, amrvis::DatasetId{1}, 64ULL << 20U);
    require(!session.metadata().hasMappedGrid, "a plain plotfile has no grid");
    require(!session.supportsMappedGrid(), "a plain plotfile session says no");
    bool threw = false;
    try {
        static_cast<void>(
            session.requestMappedGridPlane(fullRequest(1, 0.375, 4, 4)));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "a plain plotfile refuses a mapped-grid request");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: test_mapped_grid_query <materialized mapped fixture>"
                     " <plain plotfile>\n";
        return 2;
    }
    try {
        testMappedFixture(argv[1]);
        testPlainPlotfile(argv[2]);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
        return 1;
    }
    std::cout << "mapped-grid query checks passed\n";
    return 0;
}
