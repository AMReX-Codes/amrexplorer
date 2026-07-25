#pragma once

#include <amrexplorer/core/Result.hpp>

#include <vector>

namespace amrvis {

struct VectorSegment {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
};

// Legacy Amrvis vector field glyphs (AmrPicture::DrawVectorField). The
// longest plane side is partitioned into count segments, giving a sampling
// stride; one arrow is drawn per stride cell, anchored at the cell center
// (i + 0.5, j + 0.5) in plane pixel coordinates (x = column, y = row,
// row 0 at the bottom of the plane, so a positive v component points toward
// increasing y). Arrow components are scaled by arrowMax / maxSpeed, where
// maxSpeed is the maximum speed over the plane and arrowMax = 1.25 *
// (longestSide / count), so the fastest sample gets an arrow of length
// arrowMax; the sampling stride is the integer floor of longestSide / count
// (at least 1). The arrow
// head is two short barbs from the tip, each set back 0.25 of the arrow
// vector and offset 0.125 of it to either side (~26.6 degrees off the shaft,
// the legacy constants). Samples that are invalid or non-finite in either
// component are skipped, as are near-zero-length arrows; a plane whose
// maximum speed is below 1e-8 yields no segments at all. The output lists,
// per arrow, the shaft segment followed by its two head segments.
[[nodiscard]] std::vector<VectorSegment> generateVectorGlyphs(
    const ScalarPlane& uComponent, const ScalarPlane& vComponent, int count);

} // namespace amrvis
