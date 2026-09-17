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

std::uint32_t alphaOf(std::uint32_t argb)
{
    return (argb >> 24U) & 0xFFU;
}

std::uint32_t redOf(std::uint32_t argb)
{
    return (argb >> 16U) & 0xFFU;
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

amrvis::RealBox wholeWindow()
{
    return amrvis::RealBox{};  // invalid on every axis: the whole node box
}

amrvis::RealBox windowOn(double a0, double a1, double b0, double b1)
{
    amrvis::RealBox window;
    window.lower = {{a0, b0, 0.0}};
    window.upper = {{a1, b1, 1.0}};
    return window;
}

void testIdentity()
{
    // One output pixel per cell over the whole box: every subsample of a
    // pixel lies in its own cell, so the raster comes back exactly.
    const auto src = indexedRaster(4, 3);
    const auto nodes = planeWithRows(4, 3, {0.0, 1.0, 2.0, 3.0});
    const auto warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, wholeWindow(), {4, 3});
    require(warped.image.width == 4 && warped.image.height == 3,
        "the output has the requested pixel count");
    require(warped.image.rgba == src.rgba,
        "identity nodes at one pixel per cell reproduce the raster exactly");
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
        "an empty window draws the whole node box");
    require(warped.mappedBounds == warped.displayRegion,
        "the whole-box window is the plane's node box");
    require(near(warped.displayRegion.lower[2], 0.0)
            && near(warped.displayRegion.upper[2], 3.0),
        "the normal axis keeps the logical bounds");

    // Zero pixels mean the raster's own size.
    const auto sized = amrvis::warpMappedGrid(src, nodes, {0, 1}, wholeWindow(), {0, 0});
    require(sized.image.width == 4 && sized.image.height == 3 && sized.image.rgba == src.rgba,
        "zero pixels fall back to the raster size");
}

void testVerticalStretch()
{
    // Two rows of cells; the top row is twice as tall. Three pixel rows over
    // the box: row 0 from cell row 0, rows 1-2 from cell row 1, exactly,
    // because every cell edge lies on a pixel boundary.
    const auto src = indexedRaster(2, 2);
    const auto nodes = planeWithRows(2, 2, {0.0, 1.0, 3.0});
    const auto warped = amrvis::warpMappedGrid(src, nodes, {0, 2}, wholeWindow(), {2, 3});
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

void testWindowClipsToTheCellsUnderIt()
{
    // A 4 x 4 identity plane; the window covers cells (1..2, 0..1) at one
    // pixel each. Cells outside contribute nothing, and the region reports
    // the window while mappedBounds keeps the whole box.
    const auto src = indexedRaster(4, 4);
    const auto nodes = planeWithRows(4, 4, {0.0, 1.0, 2.0, 3.0, 4.0});
    const auto warped = amrvis::warpMappedGrid(
        src, nodes, {0, 1}, windowOn(1.0, 3.0, 0.0, 2.0), {2, 2});
    require(warped.image.width == 2 && warped.image.height == 2,
        "the window's pixel count is honoured");
    if (warped.image.rgba.size() == 4) {
        require(warped.image.rgba[0] == src.rgba[1] && warped.image.rgba[1] == src.rgba[2]
                && warped.image.rgba[2] == src.rgba[5] && warped.image.rgba[3] == src.rgba[6],
            "the window shows exactly the cells under it");
        require((*warped.sourceIndex)[0] == 1 && (*warped.sourceIndex)[3] == 6,
            "source indices name the windowed cells");
    }
    require(near(warped.displayRegion.lower[0], 1.0)
            && near(warped.displayRegion.upper[0], 3.0)
            && near(warped.displayRegion.lower[1], 0.0)
            && near(warped.displayRegion.upper[1], 2.0),
        "displayRegion is the window");
    require(near(warped.mappedBounds.upper[0], 4.0)
            && near(warped.mappedBounds.upper[1], 4.0),
        "mappedBounds is the whole plane's node box");

    // A window reaching past the box keeps its extent and pitch: one pixel
    // per unit over [3, 9] x [-5, 1] holds cell (3, 0) in pixel (0, 5), and
    // nothing anywhere else.
    const auto over = amrvis::warpMappedGrid(
        src, nodes, {0, 1}, windowOn(3.0, 9.0, -5.0, 1.0), {6, 6});
    const bool overSized = over.image.rgba.size() == 36U && over.sourceIndex
        && over.sourceIndex->size() == 36U;
    bool clearElsewhere = overSized;
    for (std::size_t pixel = 0; clearElsewhere && pixel < 36U; ++pixel) {
        clearElsewhere = pixel == 30U
            || (over.image.rgba[pixel] == 0U && (*over.sourceIndex)[pixel] == -1);
    }
    require(near(over.displayRegion.lower[0], 3.0) && near(over.displayRegion.upper[0], 9.0)
            && near(over.displayRegion.lower[1], -5.0)
            && near(over.displayRegion.upper[1], 1.0),
        "a window past the node box keeps its extent");
    require(overSized && clearElsewhere && over.image.rgba[30] == src.rgba[3]
            && (*over.sourceIndex)[30] == 3,
        "a window past the node box draws its cells at the window's pitch");

    // A window wholly off the box is still a warp: a transparent image of
    // that window whose source index names no cell.
    const auto off = amrvis::warpMappedGrid(
        src, nodes, {0, 1}, windowOn(5.0, 6.0, 0.0, 1.0), {2, 2});
    bool empty = off.image.rgba.size() == 4U && off.sourceIndex
        && off.sourceIndex->size() == 4U;
    for (std::size_t pixel = 0; empty && pixel < 4U; ++pixel) {
        empty = off.image.rgba[pixel] == 0U && (*off.sourceIndex)[pixel] == -1;
    }
    require(empty && near(off.displayRegion.lower[0], 5.0)
            && near(off.displayRegion.upper[0], 6.0),
        "a window off the node box is a transparent warp of that window");
}

void testWindowHelper()
{
    const auto nodes = planeWithRows(4, 4, {0.0, 1.0, 2.0, 3.0, 4.0});
    const auto whole = amrvis::mappedGridWindow(nodes, {0, 1}, wholeWindow());
    require(whole && near(whole->upper[0], 4.0) && near(whole->upper[1], 4.0),
        "an invalid window is the whole box");
    const auto part = amrvis::mappedGridWindow(
        nodes, {0, 1}, windowOn(-1.0, 2.5, 1.0, 9.0));
    require(part && near(part->lower[0], -1.0) && near(part->upper[0], 2.5)
            && near(part->lower[1], 1.0) && near(part->upper[1], 9.0),
        "a window partly outside the box is kept as asked");
    require(part && near(part->lower[2], 0.0) && near(part->upper[2], 4.0),
        "the normal axis keeps the plane's bounds");
    const auto off = amrvis::mappedGridWindow(nodes, {0, 1}, windowOn(4.0, 5.0, 0.0, 1.0));
    require(off && near(off->lower[0], 4.0) && near(off->upper[0], 5.0),
        "a window off the box is kept as asked");
    auto broken = nodes;
    broken.b[3] = std::numeric_limits<double>::quiet_NaN();
    require(!amrvis::mappedGridWindow(broken, {0, 1}, wholeWindow()),
        "an unusable plane has no window");
}

void testSlantedEdgeIsAntialiased()
{
    // One column, two cells: the shared edge climbs from b = 0.5 at a = 0 to
    // b = 1.5 at a = 1. Cell 0 is black, cell 1 white. At 4 x 8 pixels over
    // [0,1] x [0,2] (pitch 0.25) each column crosses the edge once: below it
    // pure black, above it pure white, and the pixel the edge passes through
    // a grey strictly between -- an antialiased, not stepped, edge. Alpha
    // stays full throughout: together the two cells cover every pixel.
    amrvis::ImageBuffer src;
    src.width = 1;
    src.height = 2;
    src.strideBytes = 4;
    src.rgba = {0xFF000000U, 0xFFFFFFFFU};
    amrvis::MappedGridPlane nodes;
    nodes.width = 2;
    nodes.height = 3;
    nodes.physicalRegion.lower = {{0.0, 0.0, 0.0}};
    nodes.physicalRegion.upper = {{1.0, 2.0, 1.0}};
    nodes.a = {0.0, 1.0, 0.0, 1.0, 0.0, 1.0};
    nodes.b = {0.0, 0.0, 0.5, 1.5, 2.0, 2.0};
    const auto warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, wholeWindow(), {4, 8});
    require(warped.image.width == 4 && warped.image.height == 8, "4 x 8 output");
    if (warped.image.rgba.size() != 32) {
        return;
    }
    bool allOpaque = true;
    bool monotone = true;
    int blended = 0;
    bool centreIndexRight = true;
    for (int col = 0; col < 4; ++col) {
        const double x = (col + 0.5) / 4.0;
        const double edge = 0.5 + x;  // edge height at the column centre
        std::uint32_t previous = 0;
        for (int row = 0; row < 8; ++row) {
            const auto offset = static_cast<std::size_t>(row * 4 + col);
            const auto colour = warped.image.rgba[offset];
            allOpaque = allOpaque && alphaOf(colour) == 255U;
            const auto grey = redOf(colour);
            monotone = monotone && grey >= previous;
            previous = grey;
            if (grey > 0U && grey < 255U) {
                ++blended;
            }
            const double y = (row + 0.5) / 4.0;
            const auto expected = y < edge ? 0 : 1;
            centreIndexRight = centreIndexRight
                && (*warped.sourceIndex)[offset] == expected;
        }
    }
    require(allOpaque, "two cells sharing an edge leave every pixel opaque");
    require(monotone, "grey rises monotonically across the slanted edge");
    require(blended >= 4, "each column has at least one blended edge pixel");
    require(centreIndexRight, "the source index is the cell under the pixel centre");
}

void testDomainEdgeFades()
{
    // One cell whose bottom edge is slanted (b = 0.5 at a = 0 to 0 at a = 1)
    // and top flat at 1, over pixels 4 x 8 spanning [0,1] x [0,1]: below the
    // edge nothing is drawn, above it the cell is solid, and the pixel the
    // edge crosses carries a partial alpha -- the domain boundary fades
    // rather than steps. The pixel centre below the edge has no source.
    amrvis::ImageBuffer src;
    src.width = 1;
    src.height = 1;
    src.strideBytes = 4;
    src.rgba = {0xFF0000FFU};
    amrvis::MappedGridPlane nodes;
    nodes.width = 2;
    nodes.height = 2;
    nodes.physicalRegion.lower = {{0.0, 0.0, 0.0}};
    nodes.physicalRegion.upper = {{1.0, 1.0, 1.0}};
    nodes.a = {0.0, 1.0, 0.0, 1.0};
    nodes.b = {0.5, 0.0, 1.0, 1.0};
    const auto warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, wholeWindow(), {4, 8});
    if (warped.image.rgba.size() != 32) {
        require(false, "4 x 8 output for the fading edge");
        return;
    }
    bool monotone = true;
    int partial = 0;
    bool clearBelow = true;
    bool solidAbove = true;
    bool indexMatches = true;
    for (int col = 0; col < 4; ++col) {
        const double x = (col + 0.5) / 4.0;
        const double edge = 0.5 * (1.0 - x);
        std::uint32_t previous = 0;
        for (int row = 0; row < 8; ++row) {
            const auto offset = static_cast<std::size_t>(row * 4 + col);
            const auto alpha = alphaOf(warped.image.rgba[offset]);
            monotone = monotone && alpha >= previous;
            previous = alpha;
            if (alpha > 0U && alpha < 255U) {
                ++partial;
            }
            const double y = (row + 0.5) / 8.0;
            const double lo = static_cast<double>(row) / 8.0;
            const double hi = (row + 1.0) / 8.0;
            if (hi <= edge) {
                clearBelow = clearBelow && alpha == 0U;
            } else if (lo >= edge) {
                solidAbove = solidAbove && alpha == 255U
                    && warped.image.rgba[offset] == src.rgba[0];
            }
            indexMatches = indexMatches
                && (*warped.sourceIndex)[offset] == (y < edge ? -1 : 0);
        }
    }
    require(monotone, "alpha rises monotonically across the domain edge");
    require(partial >= 4, "the domain edge has partially covered pixels");
    require(clearBelow, "pixels wholly below the domain edge stay transparent");
    require(solidAbove, "pixels wholly inside the cell are solid and unblended");
    require(indexMatches, "the source index follows the pixel centre");
}

void testTransparentSourceStaysClear()
{
    // An invalid (transparent) cell contributes no colour, so its pixels stay
    // clear, but the probe can still name the cell under the centre.
    auto src = indexedRaster(2, 1);
    src.rgba[1] = 0x00000000U;
    const auto nodes = planeWithRows(2, 1, {0.0, 1.0});
    const auto warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, wholeWindow(), {2, 1});
    require(warped.image.rgba.size() == 2 && warped.image.rgba[0] == src.rgba[0]
            && alphaOf(warped.image.rgba[1]) == 0U,
        "a transparent source cell draws nothing");
    require(warped.sourceIndex && (*warped.sourceIndex)[1] == 1,
        "a transparent cell still owns its pixel centre");
}

void testDegenerateInputFallsBack()
{
    const auto src = indexedRaster(3, 2);
    auto nodes = planeWithRows(2, 2, {0.0, 1.0, 2.0});  // wrong size
    auto warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, wholeWindow(), {3, 2});
    require(warped.image.rgba == src.rgba && warped.image.width == 3,
        "a node plane that does not fit the raster falls back to the source");
    require(warped.sourceIndex == nullptr, "fallback carries no source index");
    require(warped.displayRegion == nodes.physicalRegion,
        "fallback display region is the logical region");

    nodes = planeWithRows(3, 2, {0.0, 1.0, 2.0});
    nodes.b[4] = std::numeric_limits<double>::quiet_NaN();
    warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, wholeWindow(), {3, 2});
    require(warped.image.rgba == src.rgba && warped.sourceIndex == nullptr,
        "a non-finite node falls back to the source");

    nodes = planeWithRows(3, 2, {1.0, 1.0, 1.0});  // zero vertical extent
    warped = amrvis::warpMappedGrid(src, nodes, {0, 1}, wholeWindow(), {3, 2});
    require(warped.image.rgba == src.rgba && warped.sourceIndex == nullptr,
        "zero-extent node bounds fall back to the source");
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

void testDeepZoomInsideOneCell()
{
    // A 100 x 100 window a hundred-millionth of a unit wide inside one unit
    // cell: the cell's corners land some 1e10 pixels out, past any int, and
    // every pixel must still show that cell.
    const auto src = indexedRaster(1, 1);
    const auto nodes = planeWithRows(1, 1, {0.0, 1.0});
    const auto warped = amrvis::warpMappedGrid(src, nodes, {0, 1},
        windowOn(0.5, 0.50000001, 0.5, 0.50000001), {100, 100});
    bool full = warped.image.rgba.size() == 10000U && warped.sourceIndex
        && warped.sourceIndex->size() == 10000U;
    for (std::size_t pixel = 0; full && pixel < 10000U; ++pixel) {
        full = warped.image.rgba[pixel] == src.rgba[0]
            && (*warped.sourceIndex)[pixel] == 0;
    }
    require(full, "a window deep inside one cell shows that cell in every pixel");
}

void testDeepZoomAcrossASlantedEdge()
{
    // Two cells whose shared edge slopes, under a 100 x 100 window a
    // hundred-millionth of a unit wide sitting on that edge. Every pixel more
    // than two from the edge must show the cell it falls in: the edge
    // tolerance is a fraction of a pixel, not of the (by now enormous)
    // triangle.
    const auto src = indexedRaster(1, 2);
    amrvis::MappedGridPlane nodes;
    nodes.width = 2;
    nodes.height = 3;
    nodes.physicalRegion.lower = {{0.0, 0.0, 0.0}};
    nodes.physicalRegion.upper = {{1.0, 2.0, 1.0}};
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 2; ++i) {
            nodes.a.push_back(static_cast<double>(i));
            nodes.b.push_back(static_cast<double>(j) + 0.5 * i);
        }
    }
    // The shared edge is b = 1 + 0.5 a, so it passes through (0.5, 1.25).
    constexpr double half = 5.0e-9;
    constexpr int pixels = 100;
    const auto warped = amrvis::warpMappedGrid(src, nodes, {0, 1},
        windowOn(0.5 - half, 0.5 + half, 1.25 - half, 1.25 + half),
        {pixels, pixels});
    require(warped.sourceIndex != nullptr, "the deep-zoom warp carries a source index");
    if (!warped.sourceIndex) {
        return;
    }
    const double span = 2.0 * half;
    const double perUnit = pixels / span;  // pixels per unit, both axes
    int wrong = 0;
    for (int row = 0; row < pixels; ++row) {
        for (int column = 0; column < pixels; ++column) {
            const double a = (0.5 - half) + (column + 0.5) / pixels * span;
            const double b = (1.25 - half) + (row + 0.5) / pixels * span;
            const double side = b - (1.0 + 0.5 * a);
            if (std::abs(side) * perUnit / std::sqrt(1.25) <= 2.0) {
                continue;  // on the edge itself: either cell will do
            }
            const auto offset = static_cast<std::size_t>(row * pixels + column);
            const auto expected = static_cast<std::size_t>(side > 0.0 ? 1 : 0);
            if ((*warped.sourceIndex)[offset] != static_cast<std::int32_t>(expected)
                || warped.image.rgba[offset] != src.rgba[expected]) {
                ++wrong;
            }
        }
    }
    require(wrong == 0, "deep zoom put pixels in the wrong cell along a slanted edge");
}

} // namespace

int main()
{
    testIdentity();
    testDeepZoomInsideOneCell();
    testDeepZoomAcrossASlantedEdge();
    testVerticalStretch();
    testWindowClipsToTheCellsUnderIt();
    testWindowHelper();
    testSlantedEdgeIsAntialiased();
    testDomainEdgeFades();
    testTransparentSourceStaysClear();
    testDegenerateInputFallsBack();
    testDisplayPosition();
    if (g_failures != 0) {
        std::cerr << g_failures << " mapped-grid warp check(s) failed\n";
        return 1;
    }
    std::cout << "mapped-grid warp checks passed\n";
    return 0;
}
