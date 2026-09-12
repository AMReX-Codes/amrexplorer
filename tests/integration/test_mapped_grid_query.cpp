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
        require(plane.normalLower.size() == 25 && plane.normalUpper.size() == 25,
            "normal faces match the node count");
        bool faces = true;
        for (std::size_t n = 0; n < 25; ++n) {
            faces = faces && near(plane.normalLower[n], dx)
                && near(plane.normalUpper[n], 2 * dx);
        }
        require(faces, "y-normal faces are layers 1 and 2, which nu_y leaves put");
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
        // The slice is through cell k = 0, so its faces are node layers 0 and
        // 1, both displaced by the terrain: the cell at x = y = 0.875 spans
        // [0.109375, 0.33203125), not its logical [0, 0.25).
        bool faces = true;
        for (int j = 0; j <= 4; ++j) {
            for (int i = 0; i <= 4; ++i) {
                const auto n = node(plane, i, j);
                faces = faces && near(plane.normalLower[n], nuZ(i, j, 0))
                    && near(plane.normalUpper[n], dx + nuZ(i, j, 1));
            }
        }
        require(faces, "z-normal faces carry the terrain of layers 0 and 1");
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

// The two-level fixture (tests/data/plotfile_3d_mapped_refined): the same
// terrain, stored on level 0 everywhere and on level 1 (dx = 0.125) for
// x < 0.5 only. In physical position the recipe is
//   nu_z(x, y, z) = 0.125 * (1 - z) * (x + y) / 2
// which every level reproduces at its own nodes, and which is bilinear in
// each in-plane pair and linear along a slice normal, so the interpolated
// node positions must match it exactly wherever a coarse level alone covers
// a node.
double nuZAt(double x, double y, double z)
{
    return 0.125 * (1.0 - z) * (x + y) / 2.0;
}

void testRefinedFixture(const std::filesystem::path& fixture)
{
    amrvis::LocalDatasetSession session(
        fixture, amrvis::DatasetId{1}, 64ULL << 20U);
    require(session.supportsMappedGrid(), "the refined fixture has a grid");
    require(session.metadata().finestLevel == 1, "the refined fixture has two levels");
    constexpr double fine = 0.125;
    const auto atLevel1 = [](int normal, double position) {
        auto request = fullRequest(normal, position, 8, 8);
        request.maximumLevel = 1;
        return request;
    };

    // y-normal slice through fine cell j = 3 (y in [0.375, 0.5]): the two
    // fine node layers y = 0.375 and 0.5 straddle coarse layer 1.5. For
    // x >= 0.5 only level 0 answers; without normal interpolation both layers
    // snap to the coarse node at y = 0.5.
    {
        const auto plane = session.requestMappedGridPlane(atLevel1(1, 0.4375));
        require(plane.width == 9 && plane.height == 9, "8x8 raster at level 1");
        bool ok = true;
        for (int row = 0; row <= 8; ++row) {
            for (int column = 0; column <= 8; ++column) {
                const auto n = node(plane, column, row);
                const double x = fine * column;
                const double z = fine * row;
                const double expected = z
                    + 0.5 * (nuZAt(x, 0.375, z) + nuZAt(x, 0.5, z));
                ok = ok && near(plane.a[n], x) && near(plane.b[n], expected);
            }
        }
        require(ok, "y-normal nodes in the coarse half interpolate along y");
    }

    // x-normal slice through fine cell i = 5 (x in [0.625, 0.75]), covered by
    // level 0 alone: layers x = 0.625 and 0.75 bracket coarse layer 2.5.
    {
        const auto plane = session.requestMappedGridPlane(atLevel1(0, 0.6875));
        bool ok = true;
        for (int row = 0; row <= 8; ++row) {
            for (int column = 0; column <= 8; ++column) {
                const auto n = node(plane, column, row);
                const double y = fine * column;
                const double z = fine * row;
                const double expected = z
                    + 0.5 * (nuZAt(0.625, y, z) + nuZAt(0.75, y, z));
                ok = ok && near(plane.a[n], y) && near(plane.b[n], expected);
            }
        }
        require(ok, "x-normal nodes in the coarse-only half are interpolated");
    }

    // The same slice in the refined half (fine cell i = 1) is exact from
    // level 1's own nodes.
    {
        const auto plane = session.requestMappedGridPlane(atLevel1(0, 0.1875));
        bool ok = true;
        for (int row = 0; row <= 8; ++row) {
            for (int column = 0; column <= 8; ++column) {
                const auto n = node(plane, column, row);
                const double y = fine * column;
                const double z = fine * row;
                const double expected = z
                    + 0.5 * (nuZAt(0.125, y, z) + nuZAt(0.25, y, z));
                ok = ok && near(plane.a[n], y) && near(plane.b[n], expected);
            }
        }
        require(ok, "x-normal nodes in the refined half are exact");
    }

    // z-normal slice through fine cell k = 1 (z in [0.125, 0.25]): the plane
    // carries one block of faces per level drawn, each holding that level's own
    // node layers at every node, shared ones included. Level 0's cell is twice
    // as thick and reaches down to z = 0 even at the nodes it shares with
    // level 1's cells, where reading the node's own level would shorten it.
    {
        const auto plane = session.requestMappedGridPlane(atLevel1(2, 0.1875));
        require(plane.faceLevels.size() == 2 && plane.faceLevels[0] == 0
                && plane.faceLevels[1] == 1,
            "the refined slice draws both levels");
        require(plane.normalLower.size() == 162 && plane.normalUpper.size() == 162,
            "one block of faces per level drawn");
        bool faces = true;
        for (std::size_t block = 0; block < 2; ++block) {
            const auto offset
                = amrvis::mappedFaceOffset(plane, plane.faceLevels[block]);
            require(offset && *offset == block * 81,
                "face blocks follow the level order");
            const double low = block == 0 ? 0.0 : 0.125;
            for (int row = 0; row <= 8; ++row) {
                for (int column = 0; column <= 8; ++column) {
                    const auto n = *offset + node(plane, column, row);
                    const double x = fine * column;
                    const double y = fine * row;
                    faces = faces
                        && near(plane.normalLower[n], low + nuZAt(x, y, low))
                        && near(plane.normalUpper[n], 0.25 + nuZAt(x, y, 0.25));
                }
            }
        }
        require(faces, "a level's faces are its own layers at every node");
    }

    // x-normal slice through level 0's cell i = 2 (x in [0.5, 0.75]), just
    // past the refined half. Every cell drawn is level 0's, so that is the
    // only block of faces -- though the lower node layer at x = 0.5 is still
    // level 1's last one, which is why the levels cannot be read off it. The
    // faces are undisplaced here: the terrain moves z alone.
    {
        const auto plane = session.requestMappedGridPlane(atLevel1(0, 0.5625));
        require(plane.faceLevels.size() == 1 && plane.faceLevels[0] == 0,
            "a slice past the refined half draws level 0 alone");
        bool faces = true;
        for (std::size_t n = 0; n < 81; ++n) {
            faces = faces && near(plane.normalLower[n], 0.5)
                && near(plane.normalUpper[n], 0.75);
        }
        require(faces, "the drawn level's cell reaches from x = 0.5 to 0.75");
    }

    // Showing level 0 only: the fine raster interpolates in-plane, and the
    // layers are level 0's own, so nothing is interpolated along the normal.
    {
        auto request = fullRequest(1, 0.375, 8, 8);
        request.maximumLevel = 0;
        const auto plane = session.requestMappedGridPlane(request);
        bool ok = true;
        for (int row = 0; row <= 8; ++row) {
            for (int column = 0; column <= 8; ++column) {
                const auto n = node(plane, column, row);
                const double x = fine * column;
                const double z = fine * row;
                const double expected = z
                    + 0.5 * (nuZAt(x, 0.25, z) + nuZAt(x, 0.5, z));
                ok = ok && near(plane.b[n], expected);
            }
        }
        require(ok, "level 0 alone averages its own layers 1 and 2");
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
    if (argc < 4) {
        std::cerr << "usage: test_mapped_grid_query <materialized mapped fixture>"
                     " <plain plotfile> <materialized refined mapped fixture>\n";
        return 2;
    }
    try {
        testMappedFixture(argv[1]);
        testPlainPlotfile(argv[2]);
        testRefinedFixture(argv[3]);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
        return 1;
    }
    std::cout << "mapped-grid query checks passed\n";
    return 0;
}
