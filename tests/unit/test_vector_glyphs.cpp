#include <amrexplorer/render2d/VectorGlyphs.hpp>

#include <amrexplorer/core/CoordinateSystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool nearlyEqual(float a, float b, float tolerance = 1.0e-5F)
{
    return std::fabs(a - b) <= tolerance;
}

amrvis::ScalarPlane makePlane(int width, int height, float value)
{
    amrvis::ScalarPlane plane;
    plane.width = width;
    plane.height = height;
    const auto pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    plane.values.assign(pixelCount, value);
    plane.valid.assign(pixelCount, 1);
    plane.sourceLevel.assign(pixelCount, 0);
    return plane;
}

} // namespace

int main()
{
    // Constant (u, v) = (1, 0) on a 20x10 plane, count 10: longest side 20
    // partitions into 10 segments of 2 pixels, so stride 2, arrowMax 2.5,
    // arrows at columns 0..18 step 2 and rows 0..8 step 2 (50 arrows,
    // each contributing one shaft and two head segments).
    auto uComponent = makePlane(20, 10, 1.0F);
    const auto vComponent = makePlane(20, 10, 0.0F);
    const auto segments = amrvis::generateVectorGlyphs(uComponent, vComponent, 10);
    require(segments.size() == 150, "wrong total segment count");

    float minBaseX = 1.0e30F;
    float maxBaseX = -1.0e30F;
    float minBaseY = 1.0e30F;
    float maxBaseY = -1.0e30F;
    for (std::size_t arrow = 0; arrow + 2 < segments.size(); arrow += 3) {
        const auto& shaft = segments[arrow];
        const auto& headA = segments[arrow + 1];
        const auto& headB = segments[arrow + 2];

        require(nearlyEqual(shaft.y0, shaft.y1), "shaft is not horizontal");
        require(nearlyEqual(shaft.x1 - shaft.x0, 2.5F), "shaft length is not arrowMax");
        require(nearlyEqual(std::fmod(shaft.x0 - 0.5F, 2.0F), 0.0F)
                && nearlyEqual(std::fmod(shaft.y0 - 0.5F, 2.0F), 0.0F),
            "arrow is not anchored on a stride cell center");
        minBaseX = std::min(minBaseX, shaft.x0);
        maxBaseX = std::max(maxBaseX, shaft.x0);
        minBaseY = std::min(minBaseY, shaft.y0);
        maxBaseY = std::max(maxBaseY, shaft.y0);

        // Head barbs start at the shaft tip, set back 0.25 of the arrow
        // vector and offset 0.125 of it to either side.
        require(nearlyEqual(headA.x0, shaft.x1) && nearlyEqual(headA.y0, shaft.y1),
            "head segment does not start at the shaft tip");
        require(nearlyEqual(headB.x0, shaft.x1) && nearlyEqual(headB.y0, shaft.y1),
            "head segment does not start at the shaft tip");
        require(nearlyEqual(headA.x1, shaft.x1 - 0.625F)
                && nearlyEqual(headA.y1, shaft.y1 + 0.3125F),
            "first head barb geometry mismatch");
        require(nearlyEqual(headB.x1, shaft.x1 - 0.625F)
                && nearlyEqual(headB.y1, shaft.y1 - 0.3125F),
            "second head barb geometry mismatch");
    }
    require(nearlyEqual(minBaseX, 0.5F) && nearlyEqual(maxBaseX, 18.5F)
            && nearlyEqual(minBaseY, 0.5F) && nearlyEqual(maxBaseY, 8.5F),
        "arrow placement does not cover the expected stride cells");

    // An invalid sample suppresses exactly its arrow (3 segments).
    uComponent.valid[0] = 0;
    const auto masked = amrvis::generateVectorGlyphs(uComponent, vComponent, 10);
    require(masked.size() == 147, "invalid sample was not skipped");

    const auto zeroU = makePlane(20, 10, 0.0F);
    const auto zeroV = makePlane(20, 10, 0.0F);
    const auto zero = amrvis::generateVectorGlyphs(zeroU, zeroV, 10);
    require(zero.empty(), "zero field produced segments");

    // count > longestSide used to give sight = 0 (integer division) and thus
    // zero glyphs; floating-point division keeps sight nonzero. stride is
    // floor(8/10) clamped to 1, so every one of the 8x8 = 64 cells draws an
    // arrow of 3 segments -> 192 total (pins both the zero-glyph regression
    // and the stride=1 density).
    const auto smallU = makePlane(8, 8, 1.0F);
    const auto smallV = makePlane(8, 8, 0.0F);
    const auto dense = amrvis::generateVectorGlyphs(smallU, smallV, 10);
    require(dense.size() == 192, "count > longestSide produced wrong glyph count");

    const auto mismatched = makePlane(10, 10, 0.0F);
    bool threw = false;
    try {
        (void)amrvis::generateVectorGlyphs(uComponent, mismatched, 10);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "mismatched component sizes were accepted");
    threw = false;
    try {
        (void)amrvis::generateVectorGlyphs(uComponent, uComponent, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a zero glyph count was accepted");

    // A non-finite sample is skipped like an invalid one -- and, crucially, it
    // must not poison maxSpeed (an unguarded max would go NaN/Inf and collapse
    // every other arrow to zero length). A NaN u at a stride cell drops only
    // that arrow; the other 49 render normally (150 - 3 = 147).
    {
        auto nanU = makePlane(20, 10, 1.0F);
        const auto flatV = makePlane(20, 10, 0.0F);
        nanU.values[0] = std::numeric_limits<float>::quiet_NaN();
        const auto withNaN = amrvis::generateVectorGlyphs(nanU, flatV, 10);
        require(withNaN.size() == 147, "a NaN sample was not skipped");

        auto infV = makePlane(20, 10, 0.0F);
        const auto flatU = makePlane(20, 10, 1.0F);
        infV.values[0] = std::numeric_limits<float>::infinity();
        const auto withInf = amrvis::generateVectorGlyphs(flatU, infV, 10);
        require(withInf.size() == 147, "an infinite sample was not skipped");

        // A non-finite sample away from any stride cell (column 1 with stride 2)
        // is seen only by the maxSpeed scan; skipping it there leaves all 50
        // arrows intact, proving the scan's finiteness guard.
        auto offGrid = makePlane(20, 10, 1.0F);
        const auto offGridV = makePlane(20, 10, 0.0F);
        offGrid.values[1] = std::numeric_limits<float>::quiet_NaN();
        const auto intact = amrvis::generateVectorGlyphs(offGrid, offGridV, 10);
        require(intact.size() == 150,
            "a non-finite off-grid sample corrupted the glyph field");
    }

    // Spherical R-Z glyphs: anchors at (r sin theta, r cos theta), the
    // physical (v_r, v_theta) pair rotated into display directions, and
    // segments in display physical (R, Z) coordinates.
    {
        const auto logicalBox = [](double r0, double r1, double t0, double t1) {
            amrvis::RealBox box;
            box.lower[0] = r0;
            box.upper[0] = r1;
            box.lower[1] = t0;
            box.upper[1] = t1;
            return box;
        };

        // One sample centered at theta ~ 0 (on the +Z axis), pure v_r = 1:
        // the arrow must point along +Z with the full arrowMax length, anchored
        // at (R, Z) ~ (0, r).
        {
            auto u = makePlane(1, 1, 1.0F);
            auto v = makePlane(1, 1, 0.0F);
            const auto region = logicalBox(1.5, 2.5, 0.0, 1.0e-6);
            u.physicalRegion = region;
            v.physicalRegion = region;
            const auto display = amrvis::sphericalDisplayBounds(region);
            const auto arrows = amrvis::generateSphericalRZVectorGlyphs(
                u, v, 1, display);
            require(arrows.size() == 3, "radial sample did not yield one arrow");
            const auto& shaft = arrows.front();
            require(nearlyEqual(shaft.x0, 0.0F, 1.0e-4F)
                    && nearlyEqual(shaft.y0, 2.0F, 1.0e-4F),
                "radial arrow anchor is not at (0, r)");
            const double span = std::max(
                display.upper[0] - display.lower[0],
                display.upper[1] - display.lower[1]);
            const auto arrowMax = static_cast<float>(1.25 * span);
            require(nearlyEqual(shaft.x1 - shaft.x0, 0.0F, 1.0e-4F)
                    && nearlyEqual(shaft.y1 - shaft.y0, arrowMax, 1.0e-4F),
                "pure v_r at theta=0 does not point along +Z at arrowMax");
        }

        // Same geometry, pure v_theta = 1: e_theta at theta ~ 0 is (+R, 0), so
        // the arrow points along +R, and the lone sample carries the maximum
        // speed so it reaches the full arrowMax length.
        {
            auto u = makePlane(1, 1, 0.0F);
            auto v = makePlane(1, 1, 1.0F);
            const auto region = logicalBox(1.5, 2.5, 0.0, 1.0e-6);
            u.physicalRegion = region;
            v.physicalRegion = region;
            const auto display = amrvis::sphericalDisplayBounds(region);
            const auto arrows = amrvis::generateSphericalRZVectorGlyphs(
                u, v, 1, display);
            require(arrows.size() == 3, "v_theta sample did not yield one arrow");
            const auto& shaft = arrows.front();
            const double span = std::max(
                display.upper[0] - display.lower[0],
                display.upper[1] - display.lower[1]);
            require(nearlyEqual(shaft.x1 - shaft.x0,
                        static_cast<float>(1.25 * span), 1.0e-4F)
                    && nearlyEqual(shaft.y1 - shaft.y0, 0.0F, 1.0e-4F),
                "pure v_theta at theta=0 does not point along +R at arrowMax");
        }

        // The components are physical velocities, so equal v_theta at two
        // different radii means equal speeds: both arrows reach arrowMax with
        // no radius weighting, anchored at their own (0, r).
        {
            auto u = makePlane(2, 1, 0.0F);
            auto v = makePlane(2, 1, 1.0F);
            const auto region = logicalBox(1.0, 3.0, 0.0, 1.0e-6);
            u.physicalRegion = region;
            v.physicalRegion = region;
            const auto display = amrvis::sphericalDisplayBounds(region);
            const auto arrows = amrvis::generateSphericalRZVectorGlyphs(
                u, v, 2, display);
            require(arrows.size() == 6, "two radii did not yield two arrows");
            const auto innerLength = std::hypot(
                arrows[0].x1 - arrows[0].x0, arrows[0].y1 - arrows[0].y0);
            const auto outerLength = std::hypot(
                arrows[3].x1 - arrows[3].x0, arrows[3].y1 - arrows[3].y0);
            require(nearlyEqual(
                    static_cast<float>(innerLength / outerLength), 1.0F, 1.0e-4F),
                "equal physical speeds did not yield equal arrow lengths");
            // Cell centers at r = 1.5 and 2.5 anchor at (R, Z) ~ (0, r).
            require(nearlyEqual(arrows[0].y0, 1.5F, 1.0e-4F)
                    && nearlyEqual(arrows[3].y0, 2.5F, 1.0e-4F),
                "arrows are not anchored at their sample radii");
        }

        // At theta ~ pi/2 (the equator), pure v_r points along +R and pure
        // v_theta points along -Z (e_theta = (cos, -sin) = (0, -1)).
        {
            auto u = makePlane(1, 1, 1.0F);
            auto v = makePlane(1, 1, 0.0F);
            const double half = 1.5707963267948966;
            const auto region = logicalBox(1.5, 2.5, half - 5.0e-7, half + 5.0e-7);
            u.physicalRegion = region;
            v.physicalRegion = region;
            const auto display = amrvis::sphericalDisplayBounds(region);
            const auto arrows = amrvis::generateSphericalRZVectorGlyphs(
                u, v, 1, display);
            require(arrows.size() == 3, "equator sample did not yield one arrow");
            const auto& shaft = arrows.front();
            require(shaft.x1 - shaft.x0 > 0.0F
                    && nearlyEqual(shaft.y1 - shaft.y0, 0.0F, 1.0e-3F),
                "pure v_r at the equator does not point along +R");

            auto angularV = makePlane(1, 1, 1.0F);
            auto equatorU = makePlane(1, 1, 0.0F);
            equatorU.physicalRegion = region;
            angularV.physicalRegion = region;
            const auto angularArrows = amrvis::generateSphericalRZVectorGlyphs(
                equatorU, angularV, 1, display);
            require(angularArrows.size() == 3,
                "equator v_theta sample did not yield one arrow");
            const auto& angularShaft = angularArrows.front();
            require(angularShaft.y1 - angularShaft.y0 < 0.0F
                    && nearlyEqual(
                        angularShaft.x1 - angularShaft.x0, 0.0F, 1.0e-3F),
                "pure v_theta at the equator does not point along -Z");
        }
    }

    return 0;
}
