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

    return 0;
}
