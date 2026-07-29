#include <amrexplorer/core/CoordinateSystem.hpp>

#include <amrexplorer/core/Metadata.hpp>

#include <algorithm>
#include <array>
#include <numbers>

namespace amrvis {

bool isSpherical2D(const DatasetMetadata& metadata) noexcept
{
    return metadata.coordinateSystem == static_cast<int>(CoordinateSystem::Spherical)
        && metadata.dimension == 2 && metadata.hasPhysicalGeometry;
}

RealBox sphericalDisplayBounds(const RealBox& logicalRegion) noexcept
{
    const double r0 = logicalRegion.lower[0];
    const double r1 = logicalRegion.upper[0];
    const double t0 = logicalRegion.lower[1];
    const double t1 = logicalRegion.upper[1];

    // R = r*sin(theta) and Z = r*cos(theta) are monotonic in r (r >= 0). Z is
    // monotonic in theta (cos decreasing on [0, pi]); R is monotonic in theta
    // except for an interior maximum at theta = pi/2. So the extrema live at
    // the four corners, plus the pi/2 crossing for R.
    const std::array<std::array<double, 2>, 4> corners{{
        sphericalToDisplay(r0, t0),
        sphericalToDisplay(r0, t1),
        sphericalToDisplay(r1, t0),
        sphericalToDisplay(r1, t1),
    }};

    double rMin = corners[0][0];
    double rMax = corners[0][0];
    double zMin = corners[0][1];
    double zMax = corners[0][1];
    for (const auto& corner : corners) {
        rMin = std::min(rMin, corner[0]);
        rMax = std::max(rMax, corner[0]);
        zMin = std::min(zMin, corner[1]);
        zMax = std::max(zMax, corner[1]);
    }

    if (t0 <= std::numbers::pi / 2.0 && std::numbers::pi / 2.0 <= t1) {
        rMax = std::max(rMax, r1);  // sin(theta) peaks at theta = pi/2
    }

    RealBox bounds;
    bounds.lower[0] = rMin;
    bounds.upper[0] = rMax;
    bounds.lower[1] = zMin;
    bounds.upper[1] = zMax;
    return bounds;
}

} // namespace amrvis
