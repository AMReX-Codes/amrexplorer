#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/Request.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// A mapped (stretched) grid: the plotfile's extra nodal MultiFab gives each
// cell corner a physical position prob_lo + (i*dx, j*dy, k*dz) + nu. A slice
// drawn on it needs the corner positions of every raster cell in the slice
// plane, which is what a MappedGridPlane carries.

namespace amrvis {

// Mirrors the display SliceRequest it accompanies: the same plane, region,
// levels and raster size. The plane it yields has one more node than the
// raster has cells along each in-plane axis.
struct MappedGridPlaneRequest {
    DatasetId dataset;
    int normalDirection = 2;
    double physicalPosition = 0.0;
    RealBox visibleRegion;
    int maximumLevel = 0;
    CompositionPolicy composition = CompositionPolicy::FinestAvailable;
    std::array<int, 2> outputSize{0, 0};  // the raster size W x H
};

struct MappedGridPlane {
    // Node counts: W+1 x H+1 for a W x H raster. Row-major, first in-plane
    // axis fastest, row 0 = bottom (the ScalarPlane convention).
    int width = 0;
    int height = 0;
    // The raster's logical region (the request's visible region).
    RealBox physicalRegion;
    // Node coordinates in physical units on planeAxes(dimension, normal)[0]
    // (`a`) and [1] (`b`). For a 3-D slice at cell index c along the normal
    // these are the average of node layers c and c+1, which is exact for a
    // terrain-following grid and the cell's mid-plane otherwise.
    std::vector<double> a;
    std::vector<double> b;
    // The levels drawn on the plane, ascending: one block of faces below per
    // level, in this order (3-D only; empty in 2-D).
    std::vector<int> faceLevels;
    // The cells' two faces along the normal, in physical units: one value per
    // node for each level in `faceLevels`, blocks in that order. A level's
    // block holds that level's node layers -- its cell index c and c + 1 --
    // at every node, so a cell reads its own level at all four corners, the
    // ones it shares with a finer level's cells included. A point is in the
    // cell its faces bracket, which the logical slab bounds do not say on a
    // terrain-following grid.
    std::vector<double> normalLower;
    std::vector<double> normalUpper;
};

// Where `level`'s block of faces starts in `normalLower` and `normalUpper`,
// or nothing when the plane drew no cell at that level.
[[nodiscard]] inline std::optional<std::size_t> mappedFaceOffset(
    const MappedGridPlane& plane, int level)
{
    const auto found = std::find(
        plane.faceLevels.begin(), plane.faceLevels.end(), level);
    if (found == plane.faceLevels.end()) {
        return std::nullopt;
    }
    const auto nodes = static_cast<std::size_t>(std::max(0, plane.width))
        * static_cast<std::size_t>(std::max(0, plane.height));
    return static_cast<std::size_t>(found - plane.faceLevels.begin()) * nodes;
}

[[nodiscard]] std::vector<std::string> validateMappedGridPlaneRequest(
    const MappedGridPlaneRequest& request, int datasetDimension);

// A session that could not deliver a node plane the slice itself did not
// need: a remote peer that refused or failed the request. The slice stands
// on its logical grid and the message says why (see applyMappedGrid).
class MappedGridUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace amrvis
