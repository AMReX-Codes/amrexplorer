#pragma once

#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/AmrDataset.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace amrvis {

// The dataset at `path`, in whichever format it is: an openPMD series
// (isOpenPmdSeriesPath), else a plotfile, MultiFab or FAB.
[[nodiscard]] std::shared_ptr<AmrDataset> openDataset(
    const std::filesystem::path& path, DatasetId id,
    std::uint64_t cacheBudgetBytes, StopToken cancellation = {},
    std::vector<DerivedFieldDefinition> derivedFields = {});

// The dataset described by metadata already read with readDatasetMetadata:
// through the result's own source when it carries one, else as a plotfile
// whose block paths are relative to `dataRoot`.
[[nodiscard]] std::shared_ptr<AmrDataset> openDataset(
    std::filesystem::path dataRoot, DatasetId id,
    std::uint64_t cacheBudgetBytes, PlotfileMetadataResult metadata,
    StopToken cancellation = {},
    std::vector<DerivedFieldDefinition> derivedFields = {});

} // namespace amrvis
