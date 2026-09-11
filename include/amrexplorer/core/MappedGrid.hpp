#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/Request.hpp>

#include <array>
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
};

[[nodiscard]] std::vector<std::string> validateMappedGridPlaneRequest(
    const MappedGridPlaneRequest& request, int datasetDimension);

} // namespace amrvis
