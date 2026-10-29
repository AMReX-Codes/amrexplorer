#pragma once

#include <amrexplorer/core/Geometry.hpp>

#include <vector>

// Per-cell areas of spheres and z-aligned cylinders, for flux integrals
// through them. The geometry is Kangaroo's tangent-plane approximation, not
// the exact curved-surface area:
//
// - a cell gets nonzero area only if the true sphere or finite cylinder meets
//   its box; only then is the tangent-plane area computed, so a plane that
//   clips a corner the real surface misses contributes nothing;
// - sphere: the plane perpendicular to the unit vector from the center to the
//   cell center, at distance `radius`, sectioned by the cell box; the normal
//   is that unit vector;
// - cylinder walls: the same construction in the x-y plane, with the cell's z
//   extent clipped to |z - center.z| <= halfHeight;
// - cylinder end caps: the clipped disk-rectangle footprint, computed
//   analytically with its first moments (integrals of x dA and y dA,
//   relative to the center) for lever-arm-weighted fluxes;
// - faces: when a wall or cap lies exactly on a cell face, the cell on the
//   interior side takes it and the face is counted once.
//
// All coordinates are relative to `center`. A cell crosses the surface while
// its center may lie outside a tight region, so a caller planning leaf chunks
// for these must pad the region by half the coarsest leaf cell's diagonal
// (surfaceRegionPadding).
//
// The arithmetic follows Kangaroo's flux_geometry_support.cpp (BSD-3-Clause,
// Copyright 2026 Benjamin Wibking) operation for operation, so the two agree
// to rounding on the same plotfile.

namespace amrvis {

class LeafChunk;

struct SphereSection {
    std::vector<double> area;
    // Outward unit normal, zero where the area is.
    std::vector<double> normalX;
    std::vector<double> normalY;
    std::vector<double> normalZ;
};

struct CylinderSection {
    std::vector<double> wallArea;
    // Outward wall normal in the x-y plane, zero where the area is.
    std::vector<double> wallNormalX;
    std::vector<double> wallNormalY;
    // Each cap's area in the cell (normal +z for the upper cap, -z for the
    // lower), and the footprint's first moments, shared by the two caps since
    // they have the same footprint; zero where the cell holds no cap.
    std::vector<double> upperCapArea;
    std::vector<double> lowerCapArea;
    std::vector<double> capMomentX;
    std::vector<double> capMomentY;
};

// Throw AnalysisError unless the chunk is 3-D and the shape is finite and
// positive.
[[nodiscard]] SphereSection sphereAreaInCells(
    const LeafChunk& chunk, const Real3& center, double radius);
[[nodiscard]] CylinderSection cylinderAreaInCells(const LeafChunk& chunk,
    const Real3& center, double radius, double halfHeight);

// Half the diagonal of a level-0 cell: padding a region around a surface by
// this much keeps every leaf cell that can meet the surface, whatever level
// it is on.
[[nodiscard]] double surfaceRegionPadding(const Real3& coarseCellSize) noexcept;

// The exact area and first moments of a disk of `radius` about the origin
// intersected with the rectangle [x0, x1] x [y0, y1].
struct DiskSectionMoments {
    double area = 0.0;
    double x = 0.0;
    double y = 0.0;
};
[[nodiscard]] DiskSectionMoments diskRectangleMoments(
    double radius, double x0, double x1, double y0, double y1);

} // namespace amrvis
