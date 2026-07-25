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

// Refines a scalar plane by bilinear interpolation so contour extraction on
// the result produces genuinely smooth iso-lines instead of cell-scale
// staircases. The fine grid has dimensions ((width - 1) * factor + 1) x
// ((height - 1) * factor + 1), so fine sample (i * factor + k, j * factor + l)
// sits at continuous coordinate (i + k / factor, j + l / factor) and every
// original sample lands exactly on a fine sample. physicalRegion is copied
// unchanged; sourceLevel holds the nearest original sample's level (left
// empty when the input plane has no per-sample levels).
//
// A fine sample takes the bilinear interpolation of the four surrounding
// original samples when all four are valid (valid != 0) and finite; otherwise
// it copies the value and validity of the nearest original sample. The
// nearest-sample fallback preserves plateaus and keeps NaN and invalid
// samples from bleeding across mask boundaries: a fine sample is valid if and
// only if its nearest original sample is valid and finite. Non-finite
// original samples count as invalid for this purpose and are never
// propagated; a fine sample that would copy a non-finite value is marked
// invalid and stores 0 instead.
//
// Throws std::invalid_argument when factor < 1. factor == 1 returns a copy
// of the plane. With factor > 1, throws std::invalid_argument when width or
// height is less than 1 or when the plane storage does not match its
// dimensions.
[[nodiscard]] ScalarPlane supersamplePlane(const ScalarPlane& plane, int factor);

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
//
// Supersampling: with supersampleFactor > 1 the plane is first refined with
// supersamplePlane, segments are chained and smoothed on the fine grid, and
// every output coordinate is finally scaled back by 1 / supersampleFactor, so
// polylines are always returned in the original plane pixel space. This
// removes the cell-scale staircase that Chaikin smoothing alone cannot fix.
// Throws std::invalid_argument when supersampleFactor < 1.
[[nodiscard]] std::vector<ContourPolyline> generateContourPolylines(
    const ScalarPlane& plane, const std::vector<double>& values,
    int smoothIterations = 2, int supersampleFactor = 1);

// Chooses the bilinear refinement factor for contour extraction on a
// data-resolution plane: ceil(display samples per contour-plane sample / 2)
// (the coarser axis wins), clamped to [1, 16], with a minimum fine-grid
// target of 256 on the shorter axis, then reduced while the fine grid would
// exceed 1024 samples on either axis. One fine cell then spans at most a
// couple of display pixels, so marching squares on the refined plane
// resolves the bilinear interpolant's iso-curve far below the visible scale.
[[nodiscard]] int contourUpsampleFactor(
    int contourWidth, int contourHeight, int displayWidth, int displayHeight);

// Marching squares + chaining + one Chaikin corner-cutting pass on an already
// refined plane (see supersamplePlane), with the output mapped from
// fine-plane pixel space into display-plane pixel space. fineFactor is the
// factor the plane was refined with, so fine coordinate f corresponds to
// original sample coordinate f / fineFactor, and original sample center j
// maps to display pixel ((j + 0.5) * display / original) - 0.5 (cell-center
// to cell-center; display-plane sample i sits at scene coordinate i).
// Throws std::invalid_argument when fineFactor < 1.
[[nodiscard]] std::vector<ContourPolyline> contourPolylinesForDisplay(
    const ScalarPlane& finePlane, int fineFactor,
    const std::vector<double>& values, int displayWidth, int displayHeight);

} // namespace amrvis
