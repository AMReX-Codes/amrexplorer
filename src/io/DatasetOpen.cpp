#include <amrexplorer/io/DatasetOpen.hpp>
#include <amrexplorer/io/OpenPmd.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <utility>

namespace amrvis {

std::shared_ptr<AmrDataset> openDataset(const std::filesystem::path& path,
    DatasetId id, std::uint64_t cacheBudgetBytes, StopToken cancellation,
    std::vector<DerivedFieldDefinition> derivedFields)
{
    if (isOpenPmdSeriesPath(path)) {
        auto metadata = readOpenPmdMetadata(path, cancellation);
        return std::make_shared<AmrDataset>(std::move(metadata.source), id,
            cacheBudgetBytes, std::move(derivedFields));
    }
    return std::make_shared<PlotfileDataset>(
        path, id, cacheBudgetBytes, cancellation, std::move(derivedFields));
}

std::shared_ptr<AmrDataset> openDataset(std::filesystem::path dataRoot,
    DatasetId id, std::uint64_t cacheBudgetBytes,
    PlotfileMetadataResult metadata, StopToken cancellation,
    std::vector<DerivedFieldDefinition> derivedFields)
{
    if (metadata.source) {
        return std::make_shared<AmrDataset>(std::move(metadata.source), id,
            cacheBudgetBytes, std::move(derivedFields));
    }
    return std::make_shared<PlotfileDataset>(std::move(dataRoot), id,
        cacheBudgetBytes, std::move(metadata), cancellation,
        std::move(derivedFields));
}

} // namespace amrvis
