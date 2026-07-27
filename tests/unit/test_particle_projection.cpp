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

    return 0;
}
