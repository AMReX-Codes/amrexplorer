#pragma once

#include <amrexplorer/core/MappedGrid.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

namespace amrvis {

// The node positions of a slice raster's cells on a mapped grid. `data` is
// the plotfile dataset the slice was taken from, `grid` its Nu_nd
// sub-dataset (PlotfileDataset::mappedGrid()). The displacement components
// are read with the ordinary SliceQuery on the nodal grid dataset at one node
// more than the raster per in-plane axis, so a node's displacement is the
// nearest stored node's whatever the raster resolution. In 3-D the two node
// layers bounding the slice's cell are averaged; a node no grid block covers
// keeps its uniform position.
[[nodiscard]] MappedGridPlane queryMappedGridPlane(PlotfileDataset& data,
    PlotfileDataset& grid, const MappedGridPlaneRequest& request,
    StopToken cancellation = {});

} // namespace amrvis
