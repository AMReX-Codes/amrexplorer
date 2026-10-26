#pragma once

#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/AmrDataset.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace amrvis {

// An AmrDataset read through a PlotfileSource: the two ways of opening one,
// and nothing else. Everything a dataset does is AmrDataset's.
class PlotfileDataset final : public AmrDataset {
public:
    PlotfileDataset(
        const std::filesystem::path& plotfile, DatasetId id,
        std::uint64_t cacheBudgetBytes, StopToken cancellation = {},
        std::vector<DerivedFieldDefinition> derivedFields = {});
    PlotfileDataset(std::filesystem::path dataRoot, DatasetId id,
        std::uint64_t cacheBudgetBytes, PlotfileMetadataResult metadata,
        StopToken cancellation = {},
        std::vector<DerivedFieldDefinition> derivedFields = {});
};

} // namespace amrvis
