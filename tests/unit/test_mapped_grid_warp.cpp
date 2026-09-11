#include <amrexplorer/render2d/MappedGridWarp.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {

int g_failures = 0;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

bool near(double a, double b, double tolerance = 1e-12)
{
    return std::abs(a - b) <= tolerance;
}

// A W x H raster whose pixel (col, row) carries an opaque colour encoding
// its own index, so a warped pixel names its source.
amrvis::ImageBuffer indexedRaster(int width, int height)
{
    amrvis::ImageBuffer image;
    image.width = width;
    image.height = height;
    image.strideBytes = width * 4;
    image.rgba.resize(static_cast<std::size_t>(width * height));
    for (std::size_t pixel = 0; pixel < image.rgba.size(); ++pixel) {
        image.rgba[pixel] = 0xFF000000U | static_cast<std::uint32_t>(pixel + 1);
    }
    return image;
}

// Nodes at a = i * pitch, b = rows[j] over a unit-pitch logical region.
amrvis::MappedGridPlane planeWithRows(int width, int height,
    const std::vector<double>& rows, double pitch = 1.0)
{
    amrvis::MappedGridPlane nodes;
    nodes.width = width + 1;
    nodes.height = height + 1;
    nodes.physicalRegion.lower = {{0.0, 0.0, 0.0}};
    nodes.physicalRegion.upper = {{pitch * width, pitch * height, pitch * height}};
    for (int j = 0; j <= height; ++j) {
        for (int i = 0; i <= width; ++i) {
            nodes.a.push_back(pitch * i);
            nodes.b.push_back(rows[static_cast<std::size_t>(j)]);
        }
    }
    return nodes;
}

void testIdentity()
{
    const auto src = indexedRaster(4, 3);
    const auto nodes = planeWithRows(4, 3, {0.0, 1.0, 2.0, 3.0});
    const auto warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, 4096, 1);
    require(warped.image.width == 4 && warped.image.height == 3,
        "identity nodes keep the raster size at supersample 1");
    require(warped.image.rgba == src.rgba,
        "identity nodes reproduce the raster pixel for pixel");
    require(warped.sourceIndex != nullptr, "identity warp carries a source index");
    if (warped.sourceIndex) {
        bool identity = true;
        for (std::size_t pixel = 0; pixel < warped.sourceIndex->size(); ++pixel) {
            identity = identity
                && (*warped.sourceIndex)[pixel] == static_cast<std::int32_t>(pixel);
        }
        require(identity, "identity source index names each pixel itself");
    }
    require(near(warped.displayRegion.lower[0], 0.0)
            && near(warped.displayRegion.upper[0], 4.0)
            && near(warped.displayRegion.lower[1], 0.0)
            && near(warped.displayRegion.upper[1], 3.0),
        "identity display region equals the node bounds");
    require(near(warped.displayRegion.lower[2], 0.0)
            && near(warped.displayRegion.upper[2], 3.0),
        "the normal axis keeps the logical bounds");
}

void testVerticalStretch()
{
    // Two rows of cells; the top row is twice as tall. At supersample 1 the
    // output is 3 pixels tall: row 0 from cell row 0, rows 1-2 from cell row 1.
    const auto src = indexedRaster(2, 2);
    const auto nodes = planeWithRows(2, 2, {0.0, 1.0, 3.0});
    const auto warped = amrvis::warpMappedGrid(src, nodes, {0, 2}, 4096, 1);
    require(warped.image.width == 2 && warped.image.height == 3,
        "a doubled top row grows the output by one pixel row");
    const auto& rgba = warped.image.rgba;
    require(rgba.size() == 6, "stretched raster has 2x3 pixels");
    if (rgba.size() == 6) {
        require(rgba[0] == src.rgba[0] && rgba[1] == src.rgba[1],
            "bottom output row shows cell row 0");
        require(rgba[2] == src.rgba[2] && rgba[3] == src.rgba[3]
                && rgba[4] == src.rgba[2] && rgba[5] == src.rgba[3],
            "the two upper output rows show cell row 1");
    }
    require(near(warped.displayRegion.lower[2], 0.0)
            && near(warped.displayRegion.upper[2], 3.0)
            && near(warped.displayRegion.upper[0], 2.0),
        "display region is indexed on the requested dataset axes");
    require(near(warped.displayRegion.lower[1], 0.0)
            && near(warped.displayRegion.upper[1], 2.0),
        "the axis not in the plane keeps the logical bounds");
}

void testSourceIndexInvertsOpaquePixels()
{
    const auto src = indexedRaster(3, 4);
    const auto nodes = planeWithRows(3, 4, {0.0, 0.05, 0.2, 1.0, 4.0});
    const auto warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, 4096, 4);
    require(warped.sourceIndex != nullptr, "stretched warp carries a source index");
    if (!warped.sourceIndex) {
        return;
    }
    // Unit raster pitch over supersample 4 -> 12 x 16 output.
    require(warped.image.width == 12 && warped.image.height == 16,
        "supersample 4 gives a quarter-pitch output");
    bool inverts = true;
    bool allOpaque = true;
    for (std::size_t pixel = 0; pixel < warped.image.rgba.size(); ++pixel) {
        const auto colour = warped.image.rgba[pixel];
        const auto index = (*warped.sourceIndex)[pixel];
        if ((colour >> 24U) == 0U) {
            allOpaque = false;
            inverts = inverts && index == -1;
            continue;
        }
        inverts = inverts && index >= 0
            && static_cast<std::size_t>(index) < src.rgba.size()
            && src.rgba[static_cast<std::size_t>(index)] == colour;
    }
    require(inverts, "each opaque pixel's source index names the pixel it copied");
    // The nodes cover the whole output rectangle (a is uniform, b spans the
    // full height), so a strongly stretched grid must leave no hole.
    require(allOpaque, "no transparent pixel inside the node hull");
}

void testDegenerateInputFallsBack()
{
    const auto src = indexedRaster(3, 2);
    auto nodes = planeWithRows(2, 2, {0.0, 1.0, 2.0});  // wrong size
    auto warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, 4096, 2);
    require(warped.image.rgba == src.rgba && warped.image.width == 3,
        "a node plane that does not fit the raster falls back to the source");
    require(warped.sourceIndex == nullptr, "fallback carries no source index");
    require(warped.displayRegion == nodes.physicalRegion,
        "fallback display region is the logical region");

    nodes = planeWithRows(3, 2, {0.0, 1.0, 2.0});
    nodes.b[4] = std::numeric_limits<double>::quiet_NaN();
    warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, 4096, 2);
    require(warped.image.rgba == src.rgba && warped.sourceIndex == nullptr,
        "a non-finite node falls back to the source");

    nodes = planeWithRows(3, 2, {1.0, 1.0, 1.0});  // zero vertical extent
    warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, 4096, 2);
    require(warped.image.rgba == src.rgba && warped.sourceIndex == nullptr,
        "zero-extent node bounds fall back to the source");
}

void testAnisotropicPitchFollowsTheRaster()
{
    // A 2 x 2 raster over a region 2 wide and 0.5 tall: the raster pitch is
    // 1 along a and 0.25 along b. Identity nodes at supersample 1 must give
    // back 2 x 2 pixels -- one per cell on each axis -- not 8 x 2, which a
    // square pitch of 0.25 would produce. The pixel budget stays on the
    // cells, whichever axis is thin.
    const auto src = indexedRaster(2, 2);
    amrvis::MappedGridPlane nodes;
    nodes.width = 3;
    nodes.height = 3;
    nodes.physicalRegion.lower = {{0.0, 0.0, 0.0}};
    nodes.physicalRegion.upper = {{2.0, 0.5, 1.0}};
    for (int j = 0; j <= 2; ++j) {
        for (int i = 0; i <= 2; ++i) {
            nodes.a.push_back(1.0 * i);
            nodes.b.push_back(0.25 * j);
        }
    }
    auto warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, 4096, 1);
    require(warped.image.width == 2 && warped.image.height == 2,
        "the output pitch follows the raster pitch on each axis");
    require(warped.image.rgba == src.rgba,
        "identity nodes on an anisotropic raster reproduce it");
    // Supersample 3 multiplies both axes alike.
    warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, 4096, 3);
    require(warped.image.width == 6 && warped.image.height == 6,
        "supersampling scales both axes by the same factor");
    // Doubling the top row of cells doubles only the b extent.
    for (int i = 0; i <= 2; ++i) {
        nodes.b[static_cast<std::size_t>(2 * 3 + i)] = 0.75;
    }
    warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, 4096, 1);
    require(warped.image.width == 2 && warped.image.height == 3,
        "a stretched row adds pixels along b only");
}

void testMaximumDimensionCap()
{
    const auto src = indexedRaster(4, 4);
    const auto nodes = planeWithRows(4, 4, {0.0, 1.0, 2.0, 3.0, 4.0});
    const auto warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, 6, 16);
    require(warped.image.width <= 6 && warped.image.height <= 6,
        "maxDimension caps the supersampled output");
}

void testDisplayPosition()
{
    const auto nodes = planeWithRows(2, 2, {0.0, 1.0, 3.0}, 0.5);
    auto p = amrvis::mappedDisplayPosition(nodes, 0.0, 0.0);
    require(near(p[0], 0.0) && near(p[1], 0.0), "corner (0,0) is node (0,0)");
    p = amrvis::mappedDisplayPosition(nodes, 2.0, 2.0);
    require(near(p[0], 1.0) && near(p[1], 3.0), "corner (2,2) is node (2,2)");
    p = amrvis::mappedDisplayPosition(nodes, 1.0, 1.0);
    require(near(p[0], 0.5) && near(p[1], 1.0), "interior corner is its node");
    p = amrvis::mappedDisplayPosition(nodes, 0.5, 1.5);
    require(near(p[0], 0.25) && near(p[1], 2.0),
        "a cell centre interpolates its four nodes");
    p = amrvis::mappedDisplayPosition(nodes, -1.0, 5.0);
    require(near(p[0], 0.0) && near(p[1], 3.0), "positions clamp to the plane");
}

} // namespace

int main()
{
    testIdentity();
    testVerticalStretch();
    testSourceIndexInvertsOpaquePixels();
    testDegenerateInputFallsBack();
    testAnisotropicPitchFollowsTheRaster();
    testMaximumDimensionCap();
    testDisplayPosition();
    if (g_failures != 0) {
        std::cerr << g_failures << " mapped-grid warp check(s) failed\n";
        return 1;
    }
    std::cout << "mapped-grid warp checks passed\n";
    return 0;
}
