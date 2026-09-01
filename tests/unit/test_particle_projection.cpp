#include <amrexplorer/pipeline/ParticleProjection.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool nearlyEqual(double a, double b, double tolerance = 1.0e-12)
{
    return std::fabs(a - b)
           <= tolerance * std::max({1.0, std::fabs(a), std::fabs(b)});
}

amrvis::ParticlePoint point(double x, double y, double z = 0.0)
{
    amrvis::ParticlePoint result;
    result.position = {{x, y, z}};
    return result;
}

amrvis::ScalarPlane plane(int width, int height, amrvis::Real3 lower,
                          amrvis::Real3 upper)
{
    amrvis::ScalarPlane result;
    result.width = width;
    result.height = height;
    result.physicalRegion.lower = lower;
    result.physicalRegion.upper = upper;
    return result;
}

amrvis::LevelMetadata level(int index, double cellSize, int upperIndex)
{
    amrvis::LevelMetadata result;
    result.level = index;
    result.domain.lower = {{0, 0, 0}};
    result.domain.upper = {{upperIndex, upperIndex, upperIndex}};
    result.cellSize = {{cellSize, cellSize, cellSize}};
    return result;
}

// A unit-origin 3-D hierarchy over [0, 8) with dx = 1 on level 0 and dx = 1/2
// on level 1, so a plane cuts a cell twice as thick in the coarse regions.
amrvis::DatasetMetadata twoLevelMetadata()
{
    amrvis::DatasetMetadata metadata;
    metadata.dimension = 3;
    metadata.finestLevel = 1;
    metadata.levels = {level(0, 1.0, 7), level(1, 0.5, 15)};
    return metadata;
}

// An XY raster over x, y in [0, 10) whose left half was supplied by level 0
// and right half by level 1 -- the composition the filter has to respect.
amrvis::ScalarPlane splitLevelPlane(int width, int height)
{
    auto result = plane(width, height, {{0.0, 0.0, 0.0}}, {{10.0, 10.0, 8.0}});
    result.sourceLevel.reserve(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            result.sourceLevel.push_back(column < width / 2 ? 0 : 1);
        }
    }
    return result;
}

// sourceLevel varying by row *and* column, on a non-square raster: the
// quadrants pin the row mapping, the column mapping, and their pairing at
// once. Rows count the way a slice query writes them -- increasing physical y
// -- while the emitted scene y is flipped, so a lookup that reused the
// flipped value would read the wrong half.
//
//   y high |  uncovered  |  level 0   |
//   y low  |  level 0    |  level 1   |
//            x low          x high
amrvis::ScalarPlane quadrantLevelPlane(int width, int height)
{
    auto result = plane(width, height, {{0.0, 0.0, 0.0}}, {{10.0, 10.0, 8.0}});
    result.sourceLevel.reserve(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const bool lowY = row < height / 2;
            const bool lowX = column < width / 2;
            result.sourceLevel.push_back(
                lowY ? (lowX ? 0 : 1) : (lowX ? -1 : 0));
        }
    }
    return result;
}

} // namespace

int main()
{
    {
        const auto display =
            plane(200, 100, {{10.0, -2.0, 0.0}}, {{20.0, 2.0, 0.0}});
        const std::vector particles{
            point(10.0, -2.0), point(15.0, 0.0),  point(20.0, 2.0), point(9.0, 0.0),
            point(21.0, 0.0),  point(15.0, -3.0), point(15.0, 3.0)};
        const auto projected =
            amrvis::projectParticlePoints(particles, display, 2, 1);
        require(projected.size() == 3,
                "2-D projection did not clip outside points");
        require(nearlyEqual(projected[0].x, 0.0)
                    && nearlyEqual(projected[0].y, 100.0),
                "lower physical boundary did not map to the lower-left raster "
                "edge");
        require(nearlyEqual(projected[1].x, 100.0)
                    && nearlyEqual(projected[1].y, 50.0),
                "physical midpoint did not map to the raster midpoint");
        require(nearlyEqual(projected[2].x, 200.0)
                    && nearlyEqual(projected[2].y, 0.0),
                "upper physical boundary did not map to the upper-right raster "
                "edge");
    }

    {
        const auto display =
            plane(100, 80, {{10.0, 20.0, -1.0}}, {{20.0, 40.0, 1.0}});
        const std::vector particles{point(12.0, 35.0, -0.5)};
        const auto yz = amrvis::projectParticlePoints(particles, display, 3, 0);
        const auto xz = amrvis::projectParticlePoints(particles, display, 3, 1);
        const auto xy = amrvis::projectParticlePoints(particles, display, 3, 2);
        require(yz.size() == 1 && nearlyEqual(yz[0].x, 75.0)
                    && nearlyEqual(yz[0].y, 60.0),
                "normal 0 did not project the YZ axes");
        require(xz.size() == 1 && nearlyEqual(xz[0].x, 20.0)
                    && nearlyEqual(xz[0].y, 60.0),
                "normal 1 did not project the XZ axes");
        require(xy.size() == 1 && nearlyEqual(xy[0].x, 20.0)
                    && nearlyEqual(xy[0].y, 20.0),
                "normal 2 did not project the XY axes");

        const std::vector throughVolume{point(12.0, 35.0, 100.0)};
        const auto projectedThroughVolume =
            amrvis::projectParticlePoints(throughVolume, display, 3, 2);
        require(projectedThroughVolume.size() == 1,
                "projection incorrectly clipped on the plane-normal coordinate");
    }

    {
        const auto display = plane(20, 10, {{0.5, 0.25, 0.0}}, {{0.75, 0.75, 0.0}});
        const std::vector particles{point(0.625, 0.5), point(0.25, 0.5),
                                    point(0.875, 0.5)};
        const auto projected =
            amrvis::projectParticlePoints(particles, display, 2, 1);
        require(projected.size() == 1 && nearlyEqual(projected[0].x, 10.0)
                    && nearlyEqual(projected[0].y, 5.0),
                "zoomed physical region did not remap and clip particles");
    }

    {
        const auto valid = plane(20, 10, {{0.0, 0.0, 0.0}}, {{1.0, 1.0, 1.0}});
        auto nonfinite = point(0.5, 0.5);
        nonfinite.position[0] = std::numeric_limits<double>::quiet_NaN();
        const std::vector particles{nonfinite};
        require(amrvis::projectParticlePoints(particles, valid, 2, 1).empty(),
                "non-finite particle coordinate was projected");

        auto empty = valid;
        empty.width = 0;
        require(amrvis::projectParticlePoints(particles, empty, 2, 1).empty(),
                "empty raster accepted a particle projection");

        auto degenerate = valid;
        degenerate.physicalRegion.upper[0] = degenerate.physicalRegion.lower[0];
        require(amrvis::projectParticlePoints(std::vector{point(0.0, 0.5)},
                                              degenerate, 2, 1)
                    .empty(),
                "degenerate physical region accepted a particle projection");
        require(
            amrvis::projectParticlePoints(std::vector{point(0.5, 0.5)}, valid, 3, 3)
                .empty(),
            "invalid 3-D normal accepted a particle projection");
    }

    // sliceCellSlabs: the cell each level puts around the slice position.
    {
        const auto metadata = twoLevelMetadata();
        const auto slabs = amrvis::sliceCellSlabs(metadata, 2, 4.25);
        require(slabs.size() == 2, "a slab per level was not produced");
        require(nearlyEqual(slabs[0].lower, 4.0)
                    && nearlyEqual(slabs[0].upper, 5.0),
                "the coarse slab is not the cell containing the position");
        require(nearlyEqual(slabs[1].lower, 4.0)
                    && nearlyEqual(slabs[1].upper, 4.5),
                "the fine slab is not the cell containing the position");
        require(nearlyEqual(slabs[1].upper - slabs[1].lower,
                            0.5 * (slabs[0].upper - slabs[0].lower)),
                "refining the level did not halve the slab");

        // A 2-D dataset has no normal to filter on.
        auto flat = metadata;
        flat.dimension = 2;
        require(amrvis::sliceCellSlabs(flat, 2, 4.25).empty(),
                "a 2-D dataset produced slabs");
        require(amrvis::sliceCellSlabs(metadata, 3, 4.25).empty(),
                "an out-of-range normal produced slabs");

        // A position off the end of every level: sampleIndex extrapolates an
        // index rather than throwing, so the slabs are only empty because the
        // index is checked against the level's own domain. The levels span
        // z in [0, 8).
        for (const auto& off : amrvis::sliceCellSlabs(metadata, 2, 12.5)) {
            require(!(off.lower < off.upper),
                    "a position past the domain produced a cell to keep "
                    "particles in");
        }
        for (const auto& below : amrvis::sliceCellSlabs(metadata, 2, -1.5)) {
            require(!(below.lower < below.upper),
                    "a position below the domain produced a cell to keep "
                    "particles in");
        }
        // The last cell is still a cell: the check is a domain test, not an
        // off-by-one that eats the top of the range.
        const auto top = amrvis::sliceCellSlabs(metadata, 2, 7.5);
        require(top.size() == 2 && nearlyEqual(top[0].lower, 7.0)
                    && nearlyEqual(top[0].upper, 8.0),
                "the level's last cell was rejected as out of domain");
    }

    // The decisive case: two particles the same distance from the plane, one
    // over a coarse pixel and one over a fine one. The coarse cell still holds
    // its particle; the fine cell does not reach that far. A filter that used
    // one thickness for the whole raster would keep or drop both.
    {
        const auto metadata = twoLevelMetadata();
        const auto slabs = amrvis::sliceCellSlabs(metadata, 2, 4.25);
        const auto display = splitLevelPlane(10, 4);
        const std::vector coarse{point(2.5, 5.0, 4.75)};
        const std::vector fine{point(7.5, 5.0, 4.75)};
        require(amrvis::projectParticlePoints(coarse, display, 3, 2, slabs)
                    .size() == 1,
                "a particle inside the coarse cell the plane cuts was dropped");
        require(amrvis::projectParticlePoints(fine, display, 3, 2, slabs)
                    .empty(),
                "a particle outside the fine cell the plane cuts was drawn");
        // Both are still drawn when nothing is filtered, and land where the
        // unfiltered projection puts them.
        const auto unfiltered =
            amrvis::projectParticlePoints(fine, display, 3, 2);
        require(unfiltered.size() == 1 && nearlyEqual(unfiltered[0].x, 7.5)
                    && nearlyEqual(unfiltered[0].y, 2.0),
                "the default no longer projects through the volume");
        const auto kept =
            amrvis::projectParticlePoints(coarse, display, 3, 2, slabs);
        require(nearlyEqual(kept[0].x, 2.5) && nearlyEqual(kept[0].y, 2.0),
                "filtering moved the point it kept");

        // Half-open on the normal, the rule the slice uses for its own cell.
        require(amrvis::projectParticlePoints(
                    std::vector{point(2.5, 5.0, 4.0)}, display, 3, 2, slabs)
                    .size() == 1,
                "the cell's lower face was excluded");
        require(amrvis::projectParticlePoints(
                    std::vector{point(2.5, 5.0, 5.0)}, display, 3, 2, slabs)
                    .empty(),
                "the cell's upper face was included");

        // No data drawn at a pixel means no cell there to be in.
        auto uncovered = display;
        uncovered.sourceLevel.assign(uncovered.sourceLevel.size(), -1);
        require(amrvis::projectParticlePoints(coarse, uncovered, 3, 2, slabs)
                    .empty(),
                "a particle over an uncovered pixel was drawn");

        // A plane whose sourceLevel does not match its raster cannot answer.
        auto mismatched = display;
        mismatched.sourceLevel.pop_back();
        require(amrvis::projectParticlePoints(coarse, mismatched, 3, 2, slabs)
                    .empty(),
                "a plane with no usable source levels was filtered anyway");
    }

    // The row mapping, and its pairing with the column mapping. Four
    // particles the same distance from the plane, one per quadrant: only the
    // two over a level-0 pixel survive, and which two says the rows were read
    // in the direction a slice query writes them.
    {
        const auto metadata = twoLevelMetadata();
        const auto slabs = amrvis::sliceCellSlabs(metadata, 2, 4.25);
        const auto display = quadrantLevelPlane(8, 4);
        // Inside the coarse cell [4, 5), outside the fine cell [4, 4.5).
        const auto atLowYLowX = point(1.25, 1.25, 4.75);    // level 0
        const auto atLowYHighX = point(8.75, 1.25, 4.75);   // level 1
        const auto atHighYLowX = point(1.25, 8.75, 4.75);   // uncovered
        const auto atHighYHighX = point(8.75, 8.75, 4.75);  // level 0
        const auto kept = amrvis::projectParticlePoints(
            std::vector{atLowYLowX, atLowYHighX, atHighYLowX, atHighYHighX},
            display, 3, 2, slabs);
        require(kept.size() == 2,
                "the quadrant pattern did not keep exactly the level-0 pair");
        // Scene y is flipped, so low physical y is the *larger* scene y.
        require(nearlyEqual(kept[0].x, 1.0) && nearlyEqual(kept[0].y, 3.5),
                "the low-y level-0 particle was not the one kept");
        require(nearlyEqual(kept[1].x, 7.0) && nearlyEqual(kept[1].y, 0.5),
                "the high-y level-0 particle was not the one kept");
        // Each rejected quadrant on its own, so a failure names the pixel
        // that misread rather than only the pair count.
        require(amrvis::projectParticlePoints(
                    std::vector{atLowYHighX}, display, 3, 2, slabs)
                    .empty(),
                "the low-y fine quadrant kept a particle outside its cell");
        require(amrvis::projectParticlePoints(
                    std::vector{atHighYLowX}, display, 3, 2, slabs)
                    .empty(),
                "the high-y uncovered quadrant drew a particle");
    }

    // A particle exactly on the face between two cells belongs to the upper
    // one, at every level -- so it is drawn from exactly one cell per level,
    // never from both and never from none. The plane standing at the
    // particle's own coordinate is always one that draws it, which is what
    // makes "never from none" hold whatever the refinement.
    {
        const auto metadata = twoLevelMetadata();
        const auto display = splitLevelPlane(10, 4);
        const std::vector onFace{point(2.5, 5.0, 4.5)};      // level-0 pixel
        const std::vector onFineFace{point(7.5, 5.0, 4.5)};  // level-1 pixel

        // Level 0, dx = 1: 4.5 is interior to cell 4, so the cell below is 3
        // and only cell 4 holds the particle.
        require(amrvis::projectParticlePoints(onFace, display, 3, 2,
                    amrvis::sliceCellSlabs(metadata, 2, 3.5))
                    .empty(),
                "the coarse cell below the particle drew it");
        require(amrvis::projectParticlePoints(onFace, display, 3, 2,
                    amrvis::sliceCellSlabs(metadata, 2, 4.25))
                    .size() == 1,
                "the coarse cell containing the particle did not draw it");

        // Level 1, dx = 1/2: 4.5 is exactly the face between cells 8 and 9.
        // The upper cell takes it; the lower one does not.
        require(amrvis::projectParticlePoints(onFineFace, display, 3, 2,
                    amrvis::sliceCellSlabs(metadata, 2, 4.25))
                    .empty(),
                "the fine cell below the face drew the particle");
        require(amrvis::projectParticlePoints(onFineFace, display, 3, 2,
                    amrvis::sliceCellSlabs(metadata, 2, 4.75))
                    .size() == 1,
                "the fine cell above the face did not draw the particle");

        // The general form: a plane at the particle's own coordinate draws it
        // whichever level supplies the pixel, because sampleIndex puts the
        // plane and the particle in the same cell by construction.
        for (const auto& probe : {onFace, onFineFace}) {
            const auto own = amrvis::sliceCellSlabs(
                metadata, 2, probe.front().position[2]);
            require(amrvis::projectParticlePoints(probe, display, 3, 2, own)
                        .size() == 1,
                    "a particle was invisible from its own coordinate");
        }
    }

    return 0;
}
