#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/render2d/SphericalWarp.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numbers>
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

bool near(double a, double b, double tolerance = 1e-9)
{
    return std::abs(a - b) <= tolerance;
}

std::uint32_t alphaOf(std::uint32_t argb)
{
    return (argb >> 24U) & 0xFFU;
}

// A W x H (r, theta) raster whose pixel (col, row) carries an opaque colour
// encoding its own index, so a warped pixel names its source.
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

amrvis::ImageBuffer uniformRaster(int width, int height, std::uint32_t colour)
{
    amrvis::ImageBuffer image;
    image.width = width;
    image.height = height;
    image.strideBytes = width * 4;
    image.rgba.assign(static_cast<std::size_t>(width * height), colour);
    return image;
}

amrvis::RealBox sector(double r0, double r1, double t0, double t1)
{
    amrvis::RealBox box;
    box.lower = {{r0, t0, 0.0}};
    box.upper = {{r1, t1, 1.0}};
    return box;
}

amrvis::RealBox wholeWindow()
{
    return amrvis::RealBox{};  // invalid on every axis: the sector's box
}

amrvis::RealBox windowOn(double rLo, double rHi, double zLo, double zHi)
{
    amrvis::RealBox window;
    window.lower = {{rLo, zLo, 0.0}};
    window.upper = {{rHi, zHi, 1.0}};
    return window;
}

// The physical (R, Z) position of output pixel (col, row)'s centre.
std::array<double, 2> centreOf(const amrvis::MappedWarpedRaster& warped, int col, int row)
{
    const auto& region = warped.displayRegion;
    const double spanR = region.upper[0] - region.lower[0];
    const double spanZ = region.upper[1] - region.lower[1];
    return {region.lower[0] + (col + 0.5) / warped.image.width * spanR,
        region.lower[1] + (row + 0.5) / warped.image.height * spanZ};
}

std::size_t offsetOf(const amrvis::MappedWarpedRaster& warped, int col, int row)
{
    return static_cast<std::size_t>(row) * static_cast<std::size_t>(warped.image.width)
        + static_cast<std::size_t>(col);
}

// One cell over a quarter of an annulus: the sector's outer arc crosses the
// output at an angle everywhere, so along each row the alpha goes from full
// inside to nothing outside through partial values, never in one step.
void testArcIsAntialiased()
{
    constexpr std::uint32_t colour = 0xFF204060U;
    const auto region = sector(1.0, 2.0, 0.0, std::numbers::pi / 2.0);
    const auto warped = amrvis::warpSphericalRZ(
        uniformRaster(1, 1, colour), region, wholeWindow(), {64, 64});
    require(warped.image.width == 64 && warped.image.height == 64, "pixels as asked");
    require(warped.sourceIndex != nullptr, "the warp drew");
    const auto& bounds = warped.mappedBounds;
    require(near(bounds.lower[0], 0.0) && near(bounds.upper[0], 2.0)
            && near(bounds.lower[1], 0.0) && near(bounds.upper[1], 2.0),
        "sector bounds are the quarter annulus box");
    std::size_t partial = 0;
    std::size_t opaque = 0;
    std::size_t clear = 0;
    bool colourKept = true;
    for (int row = 0; row < 64; ++row) {
        std::uint32_t previous = 255;
        bool monotone = true;
        for (int col = 0; col < 64; ++col) {
            const auto centre = centreOf(warped, col, row);
            const double r = std::hypot(centre[0], centre[1]);
            const auto pixel = warped.image.rgba[offsetOf(warped, col, row)];
            const auto alpha = alphaOf(pixel);
            if (alpha == 255) {
                ++opaque;
                colourKept = colourKept && pixel == colour;
            } else if (alpha == 0) {
                ++clear;
            } else {
                ++partial;
                colourKept = colourKept && (pixel & 0x00FFFFFFU) == (colour & 0x00FFFFFFU);
            }
            // Past the outer arc, alpha only falls along a row (R rises).
            if (r > 1.5) {
                monotone = monotone && alpha <= previous;
                previous = alpha;
            }
        }
        require(monotone, "alpha falls monotonically across the outer arc");
    }
    require(opaque > 0 && clear > 0, "the sector has an inside and an outside");
    require(partial >= 64, "every row crosses the outer arc through a partial pixel");
    require(colourKept, "the warp carries the source colour");
}

// A pixel centre outside the sector names no cell; the boundary pixels fade.
void testSectorBoundaryFadesAndIndexStops()
{
    const auto region = sector(1.0, 2.0, 0.2, 1.2);
    const auto warped = amrvis::warpSphericalRZ(
        indexedRaster(4, 4), region, wholeWindow(), {96, 96});
    require(warped.sourceIndex != nullptr, "the warp drew");
    bool indexAgrees = true;
    std::size_t fading = 0;
    for (int row = 0; row < warped.image.height; ++row) {
        for (int col = 0; col < warped.image.width; ++col) {
            const auto centre = centreOf(warped, col, row);
            const auto rtheta = amrvis::displayToSpherical(centre[0], centre[1]);
            const bool inside = rtheta[0] >= 1.0 && rtheta[0] <= 2.0
                && rtheta[1] >= 0.2 && rtheta[1] <= 1.2;
            const auto index = (*warped.sourceIndex)[offsetOf(warped, col, row)];
            // A centre within a subsample's reach of the boundary may go
            // either way; well inside or outside it must not.
            const double margin = 2.0 / warped.image.width;
            const bool decided = std::abs(rtheta[0] - 1.0) > margin
                && std::abs(rtheta[0] - 2.0) > margin
                && std::abs(rtheta[1] - 0.2) > margin
                && std::abs(rtheta[1] - 1.2) > margin;
            if (decided && ((index >= 0) != inside)) {
                indexAgrees = false;
            }
            const auto alpha = alphaOf(warped.image.rgba[offsetOf(warped, col, row)]);
            if (alpha > 0 && alpha < 255) {
                ++fading;
            }
        }
    }
    require(indexAgrees, "a centre inside the sector names a cell, one outside none");
    require(fading > 0, "the sector boundary fades");
}

// Every indexed pixel's centre lies in the (r, theta) cell its index names.
void testSourceIndexRoundTrip()
{
    constexpr int cells = 8;
    const auto region = sector(1.0, 2.0, 0.0, 1.0);
    const auto warped = amrvis::warpSphericalRZ(
        indexedRaster(cells, cells), region, wholeWindow(), {256, 256});
    require(warped.sourceIndex != nullptr, "the warp drew");
    std::size_t named = 0;
    std::size_t wrongCell = 0;
    std::size_t wrongColour = 0;
    for (int row = 0; row < warped.image.height; ++row) {
        for (int col = 0; col < warped.image.width; ++col) {
            const auto index = (*warped.sourceIndex)[offsetOf(warped, col, row)];
            if (index < 0) {
                continue;
            }
            ++named;
            const auto centre = centreOf(warped, col, row);
            const auto rtheta = amrvis::displayToSpherical(centre[0], centre[1]);
            const double fracR = (rtheta[0] - 1.0) / 1.0 * cells;
            const double fracT = (rtheta[1] - 0.0) / 1.0 * cells;
            const int sourceCol = index % cells;
            const int sourceRow = index / cells;
            // Half-open cells, with a subsample and the chord's sagitta of
            // slack at the shared edges (a pixel is 0.05 cells here).
            const double slack = 0.05;
            const bool inR = fracR >= sourceCol - slack && fracR <= sourceCol + 1 + slack;
            const bool inT = fracT >= sourceRow - slack && fracT <= sourceRow + 1 + slack;
            if (!inR || !inT) {
                ++wrongCell;
            }
            // Well inside the cell the colour drawn is that cell's alone; a
            // pixel an edge crosses blends the neighbours.
            const double interior = 0.15;
            const bool deep = fracR > sourceCol + interior && fracR < sourceCol + 1 - interior
                && fracT > sourceRow + interior && fracT < sourceRow + 1 - interior;
            const auto pixel = warped.image.rgba[offsetOf(warped, col, row)];
            if (deep && pixel != (0xFF000000U | static_cast<std::uint32_t>(index + 1))) {
                ++wrongColour;
            }
        }
    }
    require(named > 1000, "the sector fills a good part of its box");
    require(wrongCell == 0, "every indexed pixel centre lies in the cell it names");
    require(wrongColour == 0, "a pixel inside a cell shows that cell's colour");
}

// A window over part of the sector is drawn at the pixels asked, reports
// itself as the display region while the bounds stay the whole sector, and
// leaves pixels whose centre is outside the sector clear.
void testWindowIsDrawnAsAsked()
{
    const auto region = sector(1.0, 2.0, 0.0, 1.0);
    const auto window = windowOn(1.2, 1.8, 0.5, 1.1);
    const auto warped = amrvis::warpSphericalRZ(
        indexedRaster(8, 8), region, window, {40, 30});
    require(warped.image.width == 40 && warped.image.height == 30, "window pixels");
    require(warped.sourceIndex != nullptr, "the window drew");
    require(near(warped.displayRegion.lower[0], 1.2) && near(warped.displayRegion.upper[0], 1.8)
            && near(warped.displayRegion.lower[1], 0.5)
            && near(warped.displayRegion.upper[1], 1.1),
        "display region is the window");
    const auto bounds = amrvis::sphericalDisplayBounds(region);
    require(near(warped.mappedBounds.lower[0], bounds.lower[0])
            && near(warped.mappedBounds.upper[0], bounds.upper[0])
            && near(warped.mappedBounds.lower[1], bounds.lower[1])
            && near(warped.mappedBounds.upper[1], bounds.upper[1]),
        "mapped bounds stay the whole sector");
    bool outsideClear = true;
    std::size_t drawn = 0;
    for (int row = 0; row < 30; ++row) {
        for (int col = 0; col < 40; ++col) {
            const auto centre = centreOf(warped, col, row);
            const auto rtheta = amrvis::displayToSpherical(centre[0], centre[1]);
            const auto pixel = warped.image.rgba[offsetOf(warped, col, row)];
            const double margin = 0.05;
            const bool wellOutside = rtheta[0] > 2.0 + margin || rtheta[1] > 1.0 + margin;
            if (wellOutside && alphaOf(pixel) != 0) {
                outsideClear = false;
            }
            if (alphaOf(pixel) == 255) {
                ++drawn;
            }
        }
    }
    require(outsideClear, "pixels outside the sector stay clear in a window");
    require(drawn > 0, "the window shows the sector");
    // A window wholly off the sector is a transparent image naming no cell.
    const auto off = amrvis::warpSphericalRZ(
        indexedRaster(8, 8), region, windowOn(3.0, 4.0, 3.0, 4.0), {10, 10});
    require(off.sourceIndex != nullptr, "an off-sector window still draws");
    bool blank = true;
    for (std::size_t offset = 0; offset < off.image.rgba.size(); ++offset) {
        blank = blank && off.image.rgba[offset] == 0U && (*off.sourceIndex)[offset] < 0;
    }
    require(blank, "an off-sector window is transparent and names no cell");
}

// The subdivision follows the chord's sagitta, and at a zoom where a single
// theta cell spans many pixels the arc is still round: no pixel centre inside
// the sector goes unnamed, none outside is named.
void testSubdivisionKeepsArcsRound()
{
    require(amrvis::sphericalThetaSubdivisions(0.5, 2.0, 1.0) == 1,
        "a coarse view needs no subdivision");
    // dtheta 0.5 at r 2 and 800 px per length: sqrt(2 * 800 / 2) * 0.5 = 14.1.
    require(amrvis::sphericalThetaSubdivisions(0.5, 2.0, 800.0) == 15,
        "subdivisions follow the sagitta rule");
    require(amrvis::sphericalThetaSubdivisions(0.5, 2.0, 1.0e9) == 64,
        "subdivisions are capped");
    require(amrvis::sphericalThetaSubdivisions(0.0, 2.0, 800.0) == 1
            && amrvis::sphericalThetaSubdivisions(0.5, 0.0, 800.0) == 1,
        "degenerate input means one quad");

    // Two theta cells of 0.5 rad each: a window a tenth of a length wide on
    // the outer arc at 400 x 400 pixels (4000 px per length, k = 32). A
    // single chord would sit 0.06 lengths, 250 pixels, inside the arc.
    const auto region = sector(1.0, 2.0, 0.0, 1.0);
    const auto warped = amrvis::warpSphericalRZ(
        indexedRaster(2, 2), region, windowOn(0.95, 1.05, 1.68, 1.78), {400, 400});
    require(warped.sourceIndex != nullptr, "the deep window drew");
    std::size_t wrong = 0;
    std::size_t inside = 0;
    for (int row = 0; row < 400; ++row) {
        for (int col = 0; col < 400; ++col) {
            const auto centre = centreOf(warped, col, row);
            const double r = std::hypot(centre[0], centre[1]);
            const auto index = (*warped.sourceIndex)[offsetOf(warped, col, row)];
            // A pixel within a pixel of the arc may go either way.
            if (std::abs(r - 2.0) < 1.5 / 4000.0) {
                continue;
            }
            if (r < 2.0) {
                ++inside;
            }
            if ((index >= 0) != (r < 2.0)) {
                ++wrong;
            }
        }
    }
    require(inside > 10000, "the window straddles the outer arc");
    require(wrong == 0, "at deep zoom the arc is round, not chorded");
}

// A thin annulus whose angular cells are wide: at the subdivision cap the
// chords lie several radial cells inside their arcs, so a window at a given
// radius is drawn by cells whose true radii are past it. Every pixel of a
// window inside the sector must still be named.
void testChordsReachDownIntoTheWindow()
{
    const auto region = sector(1.0, 1.0002, 0.0, std::numbers::pi);  // 64 x 4 cells
    const auto centre = amrvis::sphericalToDisplay(1.0001, 0.4);
    const double half = 1e-6;
    const auto warped = amrvis::warpSphericalRZ(indexedRaster(64, 4), region,
        windowOn(centre[0] - half, centre[0] + half, centre[1] - half, centre[1] + half),
        {64, 64});
    require(warped.sourceIndex != nullptr, "the thin annulus drew");
    std::size_t named = 0;
    for (const auto index : *warped.sourceIndex) {
        named += index >= 0 ? 1 : 0;
    }
    require(named == warped.sourceIndex->size(),
        "a window inside a thin annulus is drawn by the cells whose chords reach it");
}

void testDegenerateInputFallsBack()
{
    const auto src = indexedRaster(4, 4);
    const auto flat = amrvis::warpSphericalRZ(src, sector(1.0, 1.0, 0.0, 1.0), wholeWindow(), {32, 32});
    require(flat.sourceIndex == nullptr, "a zero radial extent falls back");
    require(flat.image.rgba == src.rgba && flat.image.width == 4, "the fallback is the source");
    require(near(flat.displayRegion.lower[0], 1.0) && near(flat.displayRegion.upper[1], 1.0),
        "the fallback's region is the logical one");
    const auto empty = amrvis::warpSphericalRZ(
        amrvis::ImageBuffer{}, sector(1.0, 2.0, 0.0, 1.0), wholeWindow(), {32, 32});
    require(empty.sourceIndex == nullptr, "an empty raster falls back");
    require(!amrvis::sphericalWindow(sector(1.0, 2.0, 1.0, 1.0), wholeWindow()).has_value(),
        "a zero theta extent has no window");
}

// No pixel count: the raster's natural size, a square pitch a quarter of
// the smaller of the radial cell and the outer tangential arc.
void testNaturalSize()
{
    const auto region = sector(1.0, 2.0, 0.0, 0.5);  // dr 0.125, r1 dtheta 0.125
    const auto warped = amrvis::warpSphericalRZ(
        indexedRaster(8, 4), region, wholeWindow(), {0, 0});
    require(warped.sourceIndex != nullptr, "the natural size drew");
    const auto bounds = amrvis::sphericalDisplayBounds(region);
    const double spanR = bounds.upper[0] - bounds.lower[0];
    const double spanZ = bounds.upper[1] - bounds.lower[1];
    require(warped.image.width == static_cast<int>(std::lround(spanR / 0.03125)),
        "natural width is the span over the pitch");
    require(warped.image.height == static_cast<int>(std::lround(spanZ / 0.03125)),
        "natural height is the span over the pitch");
    // A huge grid is capped.
    const auto wide = amrvis::warpSphericalRZ(
        uniformRaster(1, 1, 0xFF000000U), sector(1.0, 2.0, 0.0, 1.0e-6), wholeWindow(), {0, 0});
    require(wide.image.width <= 4096 && wide.image.height <= 4096, "the natural size is capped");
}

} // namespace

int main()
{
    testArcIsAntialiased();
    testSectorBoundaryFadesAndIndexStops();
    testSourceIndexRoundTrip();
    testWindowIsDrawnAsAsked();
    testSubdivisionKeepsArcsRound();
    testChordsReachDownIntoTheWindow();
    testDegenerateInputFallsBack();
    testNaturalSize();
    if (g_failures != 0) {
        std::cerr << g_failures << " spherical warp check(s) failed\n";
        return 1;
    }
    std::cout << "spherical warp checks passed\n";
    return 0;
}
