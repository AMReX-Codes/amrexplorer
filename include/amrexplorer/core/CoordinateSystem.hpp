#pragma once

#include <amrexplorer/core/Geometry.hpp>

#include <array>
#include <cmath>

namespace amrvis {

struct DatasetMetadata;

// AMReX coordinate-system codes as stored in the plotfile Header
// (DatasetMetadata::coordinateSystem).
enum class CoordinateSystem : int {
    Cartesian = 0,
    Cylindrical = 1,  // 2-D (R, Z) axisymmetric; already rectangular in display
    Spherical = 2,    // 2-D (r, theta): axis 0 = radius, axis 1 = polar angle
};

// True for a 2-D spherical plotfile carrying real geometry: the only
// coordinate system this build warps into physical (R, Z) display space.
// Everything else (Cartesian, cylindrical, 1-D/3-D, standalone FAB/MultiFab)
// renders exactly as before.
[[nodiscard]] bool isSpherical2D(const DatasetMetadata& metadata) noexcept;

// 2-D spherical display mapping. The stored in-plane axes are (r, theta) with
// theta the polar angle measured from the +Z axis; the physical display plane
// is Cartesian (R, Z) with R horizontal and Z vertical:
//   R = r*sin(theta),  Z = r*cos(theta).
[[nodiscard]] inline std::array<double, 2> sphericalToDisplay(
    double r, double theta) noexcept
{
    return {r * std::sin(theta), r * std::cos(theta)};
}

// Inverse of sphericalToDisplay for R >= 0: r = hypot(R, Z),
// theta = atan2(R, Z) (still measured from +Z).
[[nodiscard]] inline std::array<double, 2> displayToSpherical(
    double displayR, double displayZ) noexcept
{
    return {std::hypot(displayR, displayZ), std::atan2(displayR, displayZ)};
}

// Axis-aligned (R, Z) bounding box of the annular sector spanned by
// logicalRegion (r on axis 0, theta on axis 1). Only in-plane axes 0 and 1 of
// the returned box are meaningful.
[[nodiscard]] RealBox sphericalDisplayBounds(const RealBox& logicalRegion) noexcept;

} // namespace amrvis
