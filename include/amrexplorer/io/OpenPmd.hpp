#pragma once

#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/DatasetLocation.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>

#include <filesystem>

namespace amrvis {

// Whether `path` names an openPMD series rather than an AMReX plotfile,
// MultiFab or FAB: an openPMD backend's extension (.h5, .bp, .json, ...) or a
// file-based series pattern (%T). Decided from the name alone, so it costs no
// I/O and gives the same answer whether or not this build can read openPMD.
[[nodiscard]] bool isOpenPmdSeriesPath(const std::filesystem::path& path);

// Whether this build links openPMD-api (AMREXPLORER_ENABLE_OPENPMD).
[[nodiscard]] bool openPmdSupported() noexcept;

// Opens the series and reads one iteration's meshes -- the location's, or
// the first -- as an AMR hierarchy, listing every iteration in the result's
// `iterations`. The result carries the open series as its `source`, which a
// dataset built from it reads blocks through. Throws MetadataReadError when
// the series cannot be read -- or when this build cannot read openPMD at all.
//
// The mapping (docs/openpmd-plan.md): a mesh named NAME_lvlN is level N of
// NAME (WarpX's convention), a vector mesh's components become fields
// NAME_x, NAME_y, ..., and every field is presented cell-centred -- a
// staggered or nodal component is interpolated to the cell centres as it is
// read. Values and coordinates are in SI (unitSI, gridUnitSI applied).
[[nodiscard]] PlotfileMetadataResult readOpenPmdMetadata(
    const DatasetLocation& location, StopToken cancellation = {});

} // namespace amrvis
