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

// Nearest original sample index for a fine coordinate, rounding halves up;
// mirrors supersamplePlane's nearest-sample selection.
int nearestIndex(int fineIndex, int factor, int extent)
{
    const int base = fineIndex / factor;
    const int rem = fineIndex % factor;
    const int index = base + (2 * rem >= factor ? 1 : 0);
    return index < extent ? index : extent - 1;
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

    // (g) supersamplePlane: factor validation, the dimension formula, exact
    // reproduction of the original samples at multiples of factor, and
    // bilinear interpolation in between.
    threw = false;
    try {
        (void)amrvis::supersamplePlane(plane, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "supersamplePlane accepted a zero factor");

    const auto copy = amrvis::supersamplePlane(plane, 1);
    require(copy.width == plane.width && copy.height == plane.height
        && copy.values == plane.values && copy.valid == plane.valid,
        "factor 1 did not return a copy of the plane");

    constexpr int factor = 4;
    const auto fine = amrvis::supersamplePlane(plane, factor);
    require(fine.width == (plane.width - 1) * factor + 1
        && fine.height == (plane.height - 1) * factor + 1,
        "supersampled dimensions do not match the formula");
    require(fine.values.size()
        == static_cast<std::size_t>(fine.width) * static_cast<std::size_t>(fine.height),
        "supersampled storage does not match its dimensions");
    for (int y = 0; y < plane.height; ++y) {
        for (int x = 0; x < plane.width; ++x) {
            const auto coarseIndex = static_cast<std::size_t>(x + y * plane.width);
            const auto fineIndex = static_cast<std::size_t>(x * factor)
                + static_cast<std::size_t>(y * factor) * static_cast<std::size_t>(fine.width);
            require(fine.values[fineIndex] == plane.values[coarseIndex],
                "original sample not reproduced exactly on the fine grid");
            require(fine.valid[fineIndex] == plane.valid[coarseIndex],
                "original validity not reproduced on the fine grid");
            require(fine.sourceLevel[fineIndex] == plane.sourceLevel[coarseIndex],
                "source level does not track the nearest original sample");
        }
    }
    // Fine index (2, 2) is continuous coordinate (0.5, 0.5); the bilinear
    // interpolation of v = x + 2y there is exactly 1.5.
    require(nearlyEqual(fine.values[static_cast<std::size_t>(2 + 2 * fine.width)], 1.5),
        "bilinear interpolation mismatch");

    // (h) Supersampled contour of the linear field v = x + 2y at 2.3 follows
    // the analytic straight line with no staircase. 2.3 is not a multiple of
    // 1 / factor, so no fine sample hits the contour exactly.
    const auto fineLine = amrvis::generateContourPolylines(plane, {2.3}, 2, factor);
    require(fineLine.size() == 1, "supersampled linear contour split into pieces");
    require(!fineLine.front().closed, "supersampled linear contour should stay open");
    require(fineLine.front().points.size() > 4 * openRaw.front().points.size(),
        "supersampling did not densify the polyline");
    for (const auto& point : fineLine.front().points) {
        require(std::isfinite(point[0]) && std::isfinite(point[1]),
            "supersampled contour produced a non-finite point");
        require(point[0] >= 0.0F && point[0] <= 3.0F
            && point[1] >= 0.0F && point[1] <= 3.0F,
            "supersampled point escaped the original plane bounds");
        require(nearlyEqual(point[0] + 2.0 * point[1], 2.3, 1.0e-3),
            "supersampled linear contour deviates from the analytic line");
    }
    // The contour still enters at the bottom edge (2.3, 0) and leaves at the
    // left edge (0, 1.15), in either chaining direction.
    const auto& fineFirst = fineLine.front().points.front();
    const auto& fineLast = fineLine.front().points.back();
    const bool bottomFirst = nearlyEqual(fineFirst[0], 2.3, 1.0e-3)
        && nearlyEqual(fineFirst[1], 0.0, 1.0e-3);
    const bool leftLast = nearlyEqual(fineLast[0], 0.0, 1.0e-3)
        && nearlyEqual(fineLast[1], 1.15, 1.0e-3);
    const bool leftFirst = nearlyEqual(fineFirst[0], 0.0, 1.0e-3)
        && nearlyEqual(fineFirst[1], 1.15, 1.0e-3);
    const bool bottomLast = nearlyEqual(fineLast[0], 2.3, 1.0e-3)
        && nearlyEqual(fineLast[1], 0.0, 1.0e-3);
    require((bottomFirst && leftLast) || (leftFirst && bottomLast),
        "supersampled linear contour has the wrong boundary crossings");

    // (i) Mask behavior with corner (1, 1) invalidated: a fine sample is
    // valid if and only if its nearest original sample (rounding halves up)
    // is valid and finite. The invalid corner neither bleeds into the
    // bilinear interior nor shrinks the valid plateau region.
    const auto maskedFine = amrvis::supersamplePlane(masked, factor);
    for (int y = 0; y < maskedFine.height; ++y) {
        for (int x = 0; x < maskedFine.width; ++x) {
            const auto fineIndex = static_cast<std::size_t>(x)
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(maskedFine.width);
            const auto nearIndex = static_cast<std::size_t>(
                nearestIndex(x, factor, masked.width)
                + nearestIndex(y, factor, masked.height) * masked.width);
            const bool expectValid = masked.valid[nearIndex] != 0
                && std::isfinite(masked.values[nearIndex]);
            require((maskedFine.valid[fineIndex] != 0) == expectValid,
                "fine validity does not track the nearest original sample");
            require(!expectValid || std::isfinite(maskedFine.values[fineIndex]),
                "valid fine sample holds a non-finite value");
        }
    }
    // The supersampled contour at 2.5 must not enter the union of skipped
    // fine cells, the open box (0.25, 1.5) x (0.25, 1.5) around corner
    // (1, 1), and where the bilinear stencil stays fully valid (x >= 2) it
    // still follows the analytic line.
    const auto maskedContour = amrvis::generateContourPolylines(masked, {2.5}, 0, factor);
    require(!maskedContour.empty(), "supersampled contour vanished with one invalid corner");
    bool farPoint = false;
    for (const auto& polyline : maskedContour) {
        for (const auto& point : polyline.points) {
            require(std::isfinite(point[0]) && std::isfinite(point[1]),
                "masked supersampled contour produced a non-finite point");
            require(!(point[0] > 0.25F && point[0] < 1.5F
                && point[1] > 0.25F && point[1] < 1.5F),
                "supersampled contour entered the fully-invalid region");
            if (point[0] >= 2.0F) {
                farPoint = true;
                require(nearlyEqual(point[0] + 2.0 * point[1], 2.5, 1.0e-3),
                    "valid-region supersampled contour deviates from the analytic line");
            }
        }
    }
    require(farPoint, "supersampled contour lost the fully-valid region");

    // (j) NaN corner: NaN counts as an invalid contributor. No supersampled
    // value is non-finite, validity tracks the nearest finite original
    // sample, and samples whose bilinear stencil avoids the NaN corner keep
    // the exact interpolated (linear) values.
    const auto nanFine = amrvis::supersamplePlane(withNaN, factor);
    for (int y = 0; y < nanFine.height; ++y) {
        for (int x = 0; x < nanFine.width; ++x) {
            const auto fineIndex = static_cast<std::size_t>(x)
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(nanFine.width);
            require(std::isfinite(nanFine.values[fineIndex]),
                "NaN bled into a supersampled value");
            const auto nearIndex = static_cast<std::size_t>(
                nearestIndex(x, factor, withNaN.width)
                + nearestIndex(y, factor, withNaN.height) * withNaN.width);
            const bool expectValid = withNaN.valid[nearIndex] != 0
                && std::isfinite(withNaN.values[nearIndex]);
            require((nanFine.valid[fineIndex] != 0) == expectValid,
                "NaN corner did not count as an invalid contributor");
            const int i0 = x / factor;
            const int i1 = i0 + 1 < withNaN.width ? i0 + 1 : withNaN.width - 1;
            const int j0 = y / factor;
            const int j1 = j0 + 1 < withNaN.height ? j0 + 1 : withNaN.height - 1;
            const bool stencilHasNaN = i0 <= 2 && 2 <= i1 && j0 <= 2 && 2 <= j1;
            if (!stencilHasNaN) {
                const double exact = static_cast<double>(x) / factor
                    + 2.0 * static_cast<double>(y) / factor;
                require(nearlyEqual(nanFine.values[fineIndex], exact, 1.0e-4),
                    "bilinear interpolation mismatch away from the NaN corner");
            }
        }
    }
    const auto nanContour = amrvis::generateContourPolylines(withNaN, {7.5}, 2, factor);
    for (const auto& polyline : nanContour) {
        for (const auto& point : polyline.points) {
            require(std::isfinite(point[0]) && std::isfinite(point[1]),
                "supersampled NaN-plane contour produced a non-finite point");
        }
    }

    // (k) Supersampled radial contour: still a single closed loop, hugging
    // the analytic circle and staying inside the original plane pixel bounds;
    // factor 1 reproduces the two-argument behavior exactly. The contour
    // value 100.37 is not a multiple of 1 / 16: on this field every
    // supersampled value is an exact multiple of 1 / factor^2 (quadratic
    // field, exact bilinear interpolation), and a contour value that hits a
    // fine sample exactly would split chains into pieces, just like exact
    // corner hits do on the coarse grid.
    const auto fineLoop = amrvis::generateContourPolylines(radial, {100.37}, 2, factor);
    require(fineLoop.size() == 1, "supersampled radial contour split into pieces");
    require(fineLoop.front().closed, "supersampled radial contour is not closed");
    require(nearlyEqual(fineLoop.front().value, 100.37), "supersampled value mismatch");
    for (const auto& point : fineLoop.front().points) {
        require(point[0] >= 0.0F && point[0] <= 31.0F
            && point[1] >= 0.0F && point[1] <= 31.0F,
            "supersampled point escaped the original plane bounds");
        const double dx = point[0] - 15.5;
        const double dy = point[1] - 15.5;
        require(nearlyEqual(std::sqrt(dx * dx + dy * dy), std::sqrt(100.37), 0.15),
            "supersampled radial contour deviates from the analytic circle");
    }
    const auto defaultFactor = amrvis::generateContourPolylines(radial, {100.0}, 2);
    const auto explicitOne = amrvis::generateContourPolylines(radial, {100.0}, 2, 1);
    require(explicitOne.size() == defaultFactor.size(),
        "factor 1 changed the polyline count");
    for (std::size_t p = 0; p < explicitOne.size(); ++p) {
        require(explicitOne[p].points == defaultFactor[p].points
            && explicitOne[p].closed == defaultFactor[p].closed
            && explicitOne[p].value == defaultFactor[p].value,
            "factor 1 output differs from the two-argument behavior");
    }
    threw = false;
    try {
        (void)amrvis::generateContourPolylines(radial, {100.0}, 2, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "generateContourPolylines accepted a zero supersample factor");

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

    // (m) contourUpsampleFactor: targets ~2 display pixels per fine cell
    // with a minimum fine-grid size of 256 on the shorter axis.
    require(amrvis::contourUpsampleFactor(4, 4, 640, 640) == 16,
        "coarse data did not get the maximum factor");
    require(amrvis::contourUpsampleFactor(640, 640, 640, 640) == 1,
        "data at display resolution should not be refined");
    require(amrvis::contourUpsampleFactor(100, 100, 640, 640) == 4,
        "factor does not reach the 256 minimum fine-grid size");
    require(amrvis::contourUpsampleFactor(500, 500, 9000, 9000) == 2,
        "fine grid was not reduced to the 1024 cap");
    require(amrvis::contourUpsampleFactor(900, 900, 7000, 7000) == 1,
        "fine grid cap did not lower the factor to one");
    require(amrvis::contourUpsampleFactor(0, 0, 640, 640) == 1,
        "degenerate dimensions should fall back to one");

    // (n) contourPolylinesForDisplay on the 4x4 data plane v = (i + j) / 2:
    // the 2.3 iso-line (i + j = 4.6 in sample coordinates) stays straight,
    // and the known edge crossings at sample coordinates (3, 1.6) and
    // (1.6, 3) land at display pixels (559.5, 335.5) and (335.5, 559.5)
    // under the cell-center mapping d = ((c + 0.5) * 160) - 0.5.
    amrvis::ScalarPlane data;
    data.width = 4;
    data.height = 4;
    data.values.resize(16);
    data.valid.assign(16, 1);
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            data.values[static_cast<std::size_t>(i + 4 * j)]
                = 0.5F * static_cast<float>(i + j);
        }
    }
    const auto dataFactor = amrvis::contourUpsampleFactor(4, 4, 640, 640);
    const auto dataFine = amrvis::supersamplePlane(data, dataFactor);
    const auto displayLines = amrvis::contourPolylinesForDisplay(
        dataFine, dataFactor, {2.3}, 640, 640);
    require(displayLines.size() == 1, "display contour split into pieces");
    require(!displayLines.front().closed, "display contour should stay open");
    for (const auto& point : displayLines.front().points) {
        const auto ci = (static_cast<double>(point[0]) + 0.5) / 160.0 - 0.5;
        const auto cj = (static_cast<double>(point[1]) + 0.5) / 160.0 - 0.5;
        require(std::fabs(ci + cj - 4.6) / std::sqrt(2.0) < 0.05,
            "display contour deviates from the analytic iso-line");
    }
    // Chaikin keeps an open polyline's endpoints fixed, so the two ends are
    // the mapped edge crossings, in either chaining direction.
    const auto& firstPoint = displayLines.front().points.front();
    const auto& lastPoint = displayLines.front().points.back();
    const auto nearPoint = [](const std::array<float, 2>& point, double x, double y) {
        return std::fabs(static_cast<double>(point[0]) - x) <= 1.0
            && std::fabs(static_cast<double>(point[1]) - y) <= 1.0;
    };
    require((nearPoint(firstPoint, 559.5, 335.5) && nearPoint(lastPoint, 335.5, 559.5))
            || (nearPoint(firstPoint, 335.5, 559.5) && nearPoint(lastPoint, 559.5, 335.5)),
        "display contour endpoints miss the expected display pixels");

    return 0;
}
