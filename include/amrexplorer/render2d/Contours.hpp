#pragma once

#include <amrexplorer/core/Result.hpp>

#include <array>
#include <vector>

namespace amrvis {

struct ContourSegment {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
    double value = 0.0;
};

struct ContourPolyline {
    // Plane pixel coordinates (x = column, y = row), row 0 at the bottom,
    // same space as ContourSegment.
    std::vector<std::array<float, 2>> points;
    double value = 0.0;
    bool closed = false;
};

// Places count lines at the midpoint of equal-width intervals in either
// linear value space or logarithmic value space.
[[nodiscard]] std::vector<double> contourValues(
    double minimum, double maximum, int count, bool logarithmic = false);

// Marching squares over the plane's corner samples. A cell is the quad formed
// by samples (i, j), (i + 1, j), (i, j + 1), (i + 1, j + 1), so segment
// coordinates are plane pixel coordinates (x = column, y = row) spanning
// 0 .. width - 1 and 0 .. height - 1, with row 0 at the bottom of the plane.
// Cells with an invalid (valid == 0) or non-finite corner are skipped.
// Saddle cells (two diagonally opposite corners above the contour value) are
// resolved with the standard asymptotic decider: the contour value is
// compared against the mean of the four corner values (the bilinear
// interpolant's cell-center value), and the segments wrap the high corners
// when the contour value exceeds it, the low corners otherwise.
[[nodiscard]] std::vector<ContourSegment> generateContours(
    const ScalarPlane& plane, const std::vector<double>& values);

// Chains the raw segments from generateContours into polylines by joining
// segments at identical endpoints, then optionally smooths them with Chaikin
// corner cutting. Shared cell-edge crossings are computed from the same
// corner pair by the same formula in both adjacent cells, so endpoints match
// bit-exactly; chaining keys on the float bit patterns. A chain whose start
// and end coincide is reported as a closed loop (without a duplicated
// closing point). Polylines are returned grouped by contour value, in the
// order of `values`.
//
// Smoothing: each iteration replaces every segment (a, b) with the points at
// 1/4 and 3/4 along it (one iteration quarters corners; two looks genuinely
// smooth). Open polylines keep their first and last point fixed; closed
// loops cut every corner, wrapping around. Smoothed points are convex
// combinations of the chained points, so they stay inside the chained
// bounding box. This smoothing is a visual enhancement beyond legacy Amrvis
// behavior, which drew the raw segments. smoothIterations <= 0 returns the
// chained but unsmoothed polylines.
[[nodiscard]] std::vector<ContourPolyline> generateContourPolylines(
    const ScalarPlane& plane, const std::vector<double>& values,
    int smoothIterations = 2);

// Marching squares + chaining + one Chaikin corner-cutting pass on a contour
// plane, with the output mapped from contour-plane pixel space into
// display-plane pixel space. fineFactor is the refinement factor the plane
// was produced with (1 when the plane is already at contour resolution, which
// is what the slice pipeline produces): fine coordinate f corresponds to
// original sample coordinate f / fineFactor, and original sample center j
// maps to display pixel ((j + 0.5) * display / original) - 0.5 (cell-center
// to cell-center; display-plane sample i sits at scene coordinate i).
// Throws std::invalid_argument when fineFactor < 1.
[[nodiscard]] std::vector<ContourPolyline> contourPolylinesForDisplay(
    const ScalarPlane& finePlane, int fineFactor,
    const std::vector<double>& values, int displayWidth, int displayHeight);

} // namespace amrvis
