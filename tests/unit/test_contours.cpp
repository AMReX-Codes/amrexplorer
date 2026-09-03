#include <amrexplorer/render2d/Contours.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool nearlyEqual(double a, double b, double tolerance = 1.0e-5)
{
    return std::fabs(a - b) <= tolerance;
}

// 4x4 plane with the analytic field value(x, y) = x + 2y.
amrvis::ScalarPlane makePlane()
{
    amrvis::ScalarPlane plane;
    plane.width = 4;
    plane.height = 4;
    plane.values.resize(16);
    plane.valid.assign(16, 1);
    plane.sourceLevel.assign(16, 0);
    for (int y = 0; y < plane.height; ++y) {
        for (int x = 0; x < plane.width; ++x) {
            plane.values[static_cast<std::size_t>(x + y * plane.width)]
                = static_cast<float>(x + 2 * y);
        }
    }
    return plane;
}

bool onLine(const amrvis::ContourSegment& segment, double contour)
{
    return nearlyEqual(segment.x0 + 2.0 * segment.y0, contour)
        && nearlyEqual(segment.x1 + 2.0 * segment.y1, contour);
}

// size x size plane with the radial field value(x, y) = (x-cx)^2 + (y-cy)^2.
// A half-integer center keeps every corner value away from integer contour
// levels, so no corner hits the contour exactly.
amrvis::ScalarPlane makeRadialPlane(int size, double cx, double cy)
{
    amrvis::ScalarPlane plane;
    plane.width = size;
    plane.height = size;
    const auto count = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    plane.values.resize(count);
    plane.valid.assign(count, 1);
    plane.sourceLevel.assign(count, 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = static_cast<double>(x) - cx;
            const double dy = static_cast<double>(y) - cy;
            plane.values[static_cast<std::size_t>(x + y * size)]
                = static_cast<float>(dx * dx + dy * dy);
        }
    }
    return plane;
}

// Bit-exact endpoint coincidence: the point must equal one of the segment
// endpoints exactly (shared cell-edge crossings are computed identically in
// both adjacent cells, so chained endpoints are bit-identical).
bool matchesSegmentEndpoint(const std::vector<amrvis::ContourSegment>& segments,
    const std::array<float, 2>& point)
{
    for (const auto& segment : segments) {
        if ((segment.x0 == point[0] && segment.y0 == point[1])
            || (segment.x1 == point[0] && segment.y1 == point[1])) {
            return true;
        }
    }
    return false;
}

// True when the segment list contains a segment between the two points, in
// either direction.
bool hasSegment(const std::vector<amrvis::ContourSegment>& segments,
    double x0, double y0, double x1, double y1)
{
    for (const auto& segment : segments) {
        const auto forward = nearlyEqual(segment.x0, x0) && nearlyEqual(segment.y0, y0)
            && nearlyEqual(segment.x1, x1) && nearlyEqual(segment.y1, y1);
        const auto reverse = nearlyEqual(segment.x0, x1) && nearlyEqual(segment.y0, y1)
            && nearlyEqual(segment.x1, x0) && nearlyEqual(segment.y1, y0);
        if (forward || reverse) {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    const auto values = amrvis::contourValues(0.0, 1.0, 10);
    require(values.size() == 10, "contourValues returned the wrong count");
    require(nearlyEqual(values.front(), 0.05), "first contour value mismatch");
    require(nearlyEqual(values.back(), 0.95), "last contour value mismatch");
    require(nearlyEqual(values[5] - values[4], 0.1), "contour spacing mismatch");

    const auto logValues = amrvis::contourValues(1.0, 1000.0, 3, true);
    require(logValues.size() == 3, "log contourValues returned the wrong count");
    require(nearlyEqual(logValues[0], std::sqrt(10.0)),
        "first logarithmic contour value mismatch");
    require(nearlyEqual(logValues[1], std::sqrt(1000.0)),
        "middle logarithmic contour value mismatch");
    require(nearlyEqual(logValues[2], 100.0 * std::sqrt(10.0)),
        "last logarithmic contour value mismatch");
    require(nearlyEqual(logValues[1] / logValues[0], 10.0)
            && nearlyEqual(logValues[2] / logValues[1], 10.0),
        "logarithmic contours are not evenly spaced by ratio");

    bool threw = false;
    try {
        (void)amrvis::contourValues(0.0, 1.0, 0, false);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "contourValues accepted a zero count");
    threw = false;
    try {
        (void)amrvis::contourValues(1.0, 1.0, 4, false);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "contourValues accepted an empty range");
    threw = false;
    try {
        (void)amrvis::contourValues(0.0, 1.0, 4, true);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "contourValues accepted a non-positive logarithmic range");
    threw = false;
    try {
        (void)amrvis::contourValues(
            -std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max(), 4, false);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "contourValues accepted a range whose span overflows to infinity");
    threw = false;
    try {
        (void)amrvis::contourValues(
            -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity(), 4, false);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "contourValues accepted an infinite range");
    threw = false;
    try {
        (void)amrvis::contourValues(
            std::numeric_limits<double>::quiet_NaN(), 1.0, 4, false);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "contourValues accepted a NaN range endpoint");

    // Cells whose value range brackets 2.5: (0,0), (1,0), (2,0), (0,1).
    const auto plane = makePlane();
    const auto segments = amrvis::generateContours(plane, {2.5});
    require(segments.size() == 4, "analytic plane produced the wrong segment count");
    for (const auto& segment : segments) {
        require(onLine(segment, 2.5), "segment endpoint does not lie on the contour line");
        require(nearlyEqual(segment.value, 2.5), "segment value field mismatch");
    }

    auto tinyPlane = makePlane();
    for (auto& value : tinyPlane.values) {
        value *= 1.0e-24F;
    }
    const auto tinySegments =
        amrvis::generateContours(tinyPlane, {2.5e-24});
    require(tinySegments.size() == 4,
        "small-magnitude varying field was treated as constant");

    // Invalidating corner (1, 1) must suppress the four cells touching it,
    // leaving only cell (2, 0).
    auto masked = makePlane();
    masked.valid[static_cast<std::size_t>(1 + 1 * masked.width)] = 0;
    const auto maskedSegments = amrvis::generateContours(masked, {2.5});
    require(maskedSegments.size() == 1, "invalid cell was not skipped");
    const auto& survivor = maskedSegments.front();
    require(onLine(survivor, 2.5), "surviving segment does not lie on the contour line");
    require(survivor.x0 >= 2.0F && survivor.x1 >= 2.0F
            && survivor.y0 <= 1.0F && survivor.y1 <= 1.0F,
        "surviving segment lies outside cell (2, 0)");

    const auto outside = amrvis::generateContours(plane, {100.0});
    require(outside.empty(), "out-of-range contour produced segments");

    // Cells (1, 2) and (2, 1) bracket 7.5; both touch corner (2, 2).
    const auto nonFinite = amrvis::generateContours(plane, {7.5});
    require(nonFinite.size() == 2, "two-cell contour produced the wrong count");
    auto withNaN = makePlane();
    withNaN.values[static_cast<std::size_t>(2 + 2 * withNaN.width)] = std::nanf("");
    const auto nanSegments = amrvis::generateContours(withNaN, {7.5});
    require(nanSegments.empty(), "non-finite corner was not skipped");

    // A uniform field is effectively constant, so it has no meaningful
    // iso-lines: generateContours must return nothing for it whether the
    // requested level matches the value exactly or not, and the same holds
    // once chained into polylines. (Without the degenerate-range guard the
    // level matching the value marks every cell edge as crossed and tiles the
    // plane with spurious saddle segments.)
    amrvis::ScalarPlane constant;
    constant.width = 4;
    constant.height = 4;
    constant.values.assign(16, 3.5F);
    constant.valid.assign(16, 1);
    constant.sourceLevel.assign(16, 0);
    require(amrvis::generateContours(constant, {3.5}).empty(),
        "constant field contoured at its own value produced segments");
    require(amrvis::generateContours(constant, {3.5001}).empty(),
        "constant field produced segments for an off-value");
    const auto constantPolylines =
        amrvis::generateContourPolylines(constant, {3.5}, 0);
    require(constantPolylines.empty(),
        "constant field produced contour polylines");

    // (a) A radial field contoured at r^2 = 100 chains into a single closed
    // polyline. The r = 10 circle around (15.5, 15.5) stays inside the
    // 32x32 plane and never passes exactly through a corner.
    const auto radial = makeRadialPlane(32, 15.5, 15.5);
    const auto radialSegments = amrvis::generateContours(radial, {100.0});
    const auto loop = amrvis::generateContourPolylines(radial, {100.0}, 0);
    require(loop.size() == 1, "radial contour did not chain into a single polyline");
    require(loop.front().closed, "radial contour polyline is not closed");
    require(nearlyEqual(loop.front().value, 100.0), "polyline value mismatch");
    require(loop.front().points.size() > 8, "radial polyline has too few points");

    // (b) Two Chaikin iterations quadruple the point count of a closed loop
    // (n -> 2n per pass) and preserve closure.
    const auto smoothedLoop = amrvis::generateContourPolylines(radial, {100.0}, 2);
    require(smoothedLoop.size() == 1, "smoothing changed the polyline count");
    require(smoothedLoop.front().closed, "smoothing lost loop closure");
    require(smoothedLoop.front().points.size() == 4 * loop.front().points.size(),
        "two Chaikin iterations did not quadruple the point count");

    // (c) Smoothed points are convex combinations of the chained points, so
    // they stay inside the chained bounding box.
    auto minX = loop.front().points.front()[0];
    auto maxX = minX;
    auto minY = loop.front().points.front()[1];
    auto maxY = minY;
    for (const auto& point : loop.front().points) {
        minX = std::min(minX, point[0]);
        maxX = std::max(maxX, point[0]);
        minY = std::min(minY, point[1]);
        maxY = std::max(maxY, point[1]);
    }
    constexpr float boxEpsilon = 1.0e-4F;
    for (const auto& point : smoothedLoop.front().points) {
        require(point[0] >= minX - boxEpsilon && point[0] <= maxX + boxEpsilon
            && point[1] >= minY - boxEpsilon && point[1] <= maxY + boxEpsilon,
            "smoothed point escaped the chained bounding box");
    }

    // (d) The value = 2.5 contour of the linear plane is an open polyline
    // entering at the bottom edge (2.5, 0) and leaving at the left edge
    // (0, 1.25); smoothing keeps both endpoints exactly fixed.
    const auto openRaw = amrvis::generateContourPolylines(plane, {2.5}, 0);
    require(openRaw.size() == 1, "linear contour did not chain into one polyline");
    require(!openRaw.front().closed, "linear contour should stay open");
    require(openRaw.front().points.size() == segments.size() + 1,
        "open polyline point/segment accounting mismatch");
    const auto openSmooth = amrvis::generateContourPolylines(plane, {2.5}, 2);
    require(openSmooth.size() == 1 && !openSmooth.front().closed,
        "smoothing changed the open polyline topology");
    require(openSmooth.front().points.front() == openRaw.front().points.front(),
        "smoothing moved the first point of an open polyline");
    require(openSmooth.front().points.back() == openRaw.front().points.back(),
        "smoothing moved the last point of an open polyline");
    require(openRaw.front().points.front()[0] == 2.5F
        && openRaw.front().points.front()[1] == 0.0F,
        "open polyline starts at the wrong boundary crossing");
    require(openRaw.front().points.back()[0] == 0.0F
        && openRaw.front().points.back()[1] == 1.25F,
        "open polyline ends at the wrong boundary crossing");

    // (e) Invalidating a corner on each side of the circle suppresses the
    // cells touching it and splits the loop into two separate open arcs.
    auto cut = makeRadialPlane(32, 15.5, 15.5);
    cut.valid[static_cast<std::size_t>(25 + 15 * cut.width)] = 0;
    cut.valid[static_cast<std::size_t>(6 + 16 * cut.width)] = 0;
    const auto arcs = amrvis::generateContourPolylines(cut, {100.0}, 0);
    require(arcs.size() == 2, "invalid cells did not split the loop into two arcs");
    for (const auto& arc : arcs) {
        require(!arc.closed, "an arc cut by invalid cells was reported closed");
    }

    // (f) smoothIterations <= 0 returns the chained but unsmoothed points:
    // every polyline point coincides bit-exactly with a segment endpoint
    // from generateContours, and a closed loop has one point per segment.
    require(loop.front().points.size() == radialSegments.size(),
        "closed loop point/segment accounting mismatch");
    for (const auto& point : loop.front().points) {
        require(matchesSegmentEndpoint(radialSegments, point),
            "chained point does not coincide with a segment endpoint");
    }
    for (const auto& point : openRaw.front().points) {
        require(matchesSegmentEndpoint(segments, point),
            "open chain point does not coincide with a segment endpoint");
    }
    const auto negative = amrvis::generateContourPolylines(radial, {100.0}, -3);
    require(negative.size() == 1
        && negative.front().points.size() == loop.front().points.size(),
        "negative smoothIterations did not return unsmoothed polylines");
    const auto none = amrvis::generateContourPolylines(plane, {100.0});
    require(none.empty(), "out-of-range contour produced polylines");

    // (l) Saddle cells resolve with the asymptotic decider: the contour
    // value is compared against the mean of the four corner values (the
    // bilinear interpolant's cell-center value). With the main diagonal
    // corners high (1) and the anti-diagonal corners low (0), a contour
    // above the mean (0.6) wraps the high corners (left-bottom and
    // top-right pairings), a contour below the mean (0.4) wraps the low
    // corners (left-top and bottom-right), and a contour at the mean (0.5)
    // follows the same center rule as above the mean.
    amrvis::ScalarPlane saddle;
    saddle.width = 2;
    saddle.height = 2;
    saddle.values = {1.0F, 0.0F, 0.0F, 1.0F};
    saddle.valid.assign(4, 1);
    const auto saddleHigh = amrvis::generateContours(saddle, {0.6});
    require(saddleHigh.size() == 2, "saddle cell produced the wrong segment count");
    require(hasSegment(saddleHigh, 0.0, 0.4, 0.4, 0.0)
            && hasSegment(saddleHigh, 0.6, 1.0, 1.0, 0.6),
        "saddle above the mean did not wrap the high corners");
    const auto saddleLow = amrvis::generateContours(saddle, {0.4});
    require(saddleLow.size() == 2, "saddle cell produced the wrong segment count");
    require(hasSegment(saddleLow, 0.0, 0.6, 0.4, 1.0)
            && hasSegment(saddleLow, 0.6, 0.0, 1.0, 0.4),
        "saddle below the mean did not wrap the low corners");
    const auto saddleMean = amrvis::generateContours(saddle, {0.5});
    require(saddleMean.size() == 2, "saddle cell produced the wrong segment count");
    require(hasSegment(saddleMean, 0.0, 0.5, 0.5, 0.0)
            && hasSegment(saddleMean, 0.5, 1.0, 1.0, 0.5),
        "saddle at the mean did not follow the center rule");

    // (m) contourPolylinesForDisplay on a NON-square 4x8 plane against a
    // non-square 640x320 display, so scaleX (160) != scaleY (40): swapping the
    // two axes' scales would move every contour point off the raster, which a
    // square plane/display cannot catch. The field v = i is constant in j, so
    // the 1.5 iso-line is the vertical line i = 1.5 spanning all j; under the
    // cell-center mapping d = ((c + 0.5) * scale) - 0.5 that is display x = 319.5
    // with y running from 19.5 (j = 0) to 299.5 (j = 7).
    amrvis::ScalarPlane data;
    data.width = 4;
    data.height = 8;
    data.values.resize(32);
    data.valid.assign(32, 1);
    for (int j = 0; j < 8; ++j) {
        for (int i = 0; i < 4; ++i) {
            data.values[static_cast<std::size_t>(i + 4 * j)]
                = static_cast<float>(i);
        }
    }
    // The slice pipeline supplies a contour plane already at contour
    // resolution, so exercise that mapping directly.
    const auto displayLines = amrvis::contourPolylinesForDisplay(
        data, {1.5}, 640, 320);
    require(displayLines.size() == 1, "display contour split into pieces");
    require(!displayLines.front().closed, "display contour should stay open");
    for (const auto& point : displayLines.front().points) {
        require(std::fabs(static_cast<double>(point[0]) - 319.5) < 0.5,
            "display contour x off the vertical iso-line (scaleX misapplied)");
        require(static_cast<double>(point[1]) >= 19.0
                && static_cast<double>(point[1]) <= 300.0,
            "display contour y outside the mapped span (scaleY misapplied)");
    }
    // Chaikin keeps an open polyline's endpoints fixed, so the two ends are the
    // mapped edge crossings at j = 0 and j = 7, in either chaining direction.
    const auto& firstPoint = displayLines.front().points.front();
    const auto& lastPoint = displayLines.front().points.back();
    const auto nearPoint = [](const std::array<float, 2>& point, double x, double y) {
        return std::fabs(static_cast<double>(point[0]) - x) <= 1.0
            && std::fabs(static_cast<double>(point[1]) - y) <= 1.0;
    };
    require((nearPoint(firstPoint, 319.5, 19.5) && nearPoint(lastPoint, 319.5, 299.5))
            || (nearPoint(firstPoint, 319.5, 299.5) && nearPoint(lastPoint, 319.5, 19.5)),
        "display contour endpoints miss the expected display pixels");

    // (n) One-pixel-thick planes have no complete 2x2 marching-squares cell
    // (the cell loop runs to width-1 and height-1), so a 1xN or Nx1 plane
    // yields nothing even though its values straddle the requested level.
    {
        amrvis::ScalarPlane column;   // 1 x 5
        column.width = 1;
        column.height = 5;
        column.values = {0.0F, 1.0F, 2.0F, 3.0F, 4.0F};
        column.valid.assign(5, 1);
        column.sourceLevel.assign(5, 0);
        require(amrvis::generateContours(column, {2.5}).empty(),
            "a 1xN plane produced contour segments");
        require(amrvis::generateContourPolylines(column, {2.5}, 0).empty(),
            "a 1xN plane produced contour polylines");

        amrvis::ScalarPlane row;      // 5 x 1
        row.width = 5;
        row.height = 1;
        row.values = {0.0F, 1.0F, 2.0F, 3.0F, 4.0F};
        row.valid.assign(5, 1);
        row.sourceLevel.assign(5, 0);
        require(amrvis::generateContours(row, {2.5}).empty(),
            "an Nx1 plane produced contour segments");
        require(amrvis::generateContourPolylines(row, {2.5}, 0).empty(),
            "an Nx1 plane produced contour polylines");
    }

    // (o) A plateau exactly on a contour level must not hatch its interior.
    // 5x5 field: an interior 3x3 block of points (indices 1..3) at 5.0, a
    // border ring at 10.0, so the value-5 contour traces the plateau boundary
    // (the square x,y in {1,3}) and leaves the interior empty. The inclusive
    // predicate emitted the left+bottom edge of every interior cell, hatching
    // x=2 / y=2 with a grid of segments (contour-plateau-and-corner-artifacts).
    {
        amrvis::ScalarPlane plateau;
        plateau.width = 5;
        plateau.height = 5;
        plateau.values.resize(25);
        plateau.valid.assign(25, 1);
        plateau.sourceLevel.assign(25, 0);
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 5; ++x) {
                const bool interior = x >= 1 && x <= 3 && y >= 1 && y <= 3;
                plateau.values[static_cast<std::size_t>(x + 5 * y)]
                    = interior ? 5.0F : 10.0F;
            }
        }
        const auto plateauSegments = amrvis::generateContours(plateau, {5.0});
        require(!plateauSegments.empty(),
            "the plateau boundary contour was fully suppressed");
        for (const auto& segment : plateauSegments) {
            const bool startInside = segment.x0 > 1.0F && segment.x0 < 3.0F
                && segment.y0 > 1.0F && segment.y0 < 3.0F;
            const bool endInside = segment.x1 > 1.0F && segment.x1 < 3.0F
                && segment.y1 > 1.0F && segment.y1 < 3.0F;
            require(!startInside && !endInside,
                "a segment hatched the interior of a value-exact plateau");
        }
    }

    // (p) A contour passing exactly through grid corners must emit no
    // zero-length segments. value(x, y) = x - y on a 3x3 grid: the v = 0
    // iso-line runs along the main diagonal through the corners (0,0), (1,1),
    // (2,2). Cells straddling the diagonal interpolate their crossings onto
    // those shared corners; the reader must draw the diagonal while dropping
    // the degenerate zero-length touches (else chaining injects a duplicate
    // vertex or a one-point "closed" polyline).
    {
        amrvis::ScalarPlane corner;
        corner.width = 3;
        corner.height = 3;
        corner.values.resize(9);
        corner.valid.assign(9, 1);
        corner.sourceLevel.assign(9, 0);
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                corner.values[static_cast<std::size_t>(x + 3 * y)]
                    = static_cast<float>(x - y);
            }
        }
        const auto cornerSegments = amrvis::generateContours(corner, {0.0});
        for (const auto& segment : cornerSegments) {
            require(!(segment.x0 == segment.x1 && segment.y0 == segment.y1),
                "a corner-exact contour emitted a zero-length segment");
        }
        require(hasSegment(cornerSegments, 0.0, 0.0, 1.0, 1.0)
                && hasSegment(cornerSegments, 1.0, 1.0, 2.0, 2.0),
            "corner-exact contour dropped the real diagonal iso-line");
        const auto cornerLines =
            amrvis::generateContourPolylines(corner, {0.0}, 0);
        for (const auto& line : cornerLines) {
            require(line.points.size() >= 2,
                "corner-exact contour produced a one-point polyline");
        }
    }

    const auto symlog = amrvis::contourValues(-100.0, 100.0, 5,
        {amrvis::ColorScale::SymLogarithmic, 1.0});
    require(symlog.size() == 5 && std::abs(symlog[2]) < 1.0e-14,
        "symmetric-log contours did not retain zero");
    return 0;
}
