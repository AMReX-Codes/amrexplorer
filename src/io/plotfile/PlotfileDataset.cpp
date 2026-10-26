#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/PlotfileSource.hpp>

#include <memory>
#include <utility>

namespace amrvis {

PlotfileDataset::PlotfileDataset(const std::filesystem::path& plotfile,
    DatasetId id, std::uint64_t cacheBudgetBytes, StopToken cancellation,
    std::vector<DerivedFieldDefinition> derivedFields)
    : AmrDataset(std::make_shared<PlotfileSource>(plotfile, cancellation), id,
          cacheBudgetBytes, std::move(derivedFields))
{
}

PlotfileDataset::PlotfileDataset(std::filesystem::path dataRoot, DatasetId id,
    std::uint64_t cacheBudgetBytes, PlotfileMetadataResult metadata,
    StopToken cancellation, std::vector<DerivedFieldDefinition> derivedFields)
    : AmrDataset(std::make_shared<PlotfileSource>(
                     std::move(dataRoot), std::move(metadata), cancellation),
          id, cacheBudgetBytes, std::move(derivedFields))
{
}

} // namespace amrvis
