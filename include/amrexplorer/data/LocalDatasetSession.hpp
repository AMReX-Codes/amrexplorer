#pragma once

#include <amrexplorer/data/DatasetSession.hpp>

#include <filesystem>
#include <memory>
#include <mutex>

namespace amrvis {

class PlotfileDataset;

class LocalDatasetSession final : public DatasetSession {
public:
    explicit LocalDatasetSession(
        std::shared_ptr<PlotfileDataset> dataset, std::string fileVersion = {});
    LocalDatasetSession(const std::filesystem::path& path, DatasetId id,
        std::uint64_t cacheBudgetBytes, StopToken cancellation = {});
    LocalDatasetSession(std::filesystem::path dataRoot, DatasetId id,
        std::uint64_t cacheBudgetBytes, PlotfileMetadataResult metadata,
        StopToken cancellation = {});

    [[nodiscard]] DatasetId id() const noexcept override;
    [[nodiscard]] const DatasetMetadata& metadata() const noexcept override;
    [[nodiscard]] const MetadataReadMetrics& metadataReadMetrics()
        const noexcept override;
    [[nodiscard]] const std::string& fileVersion() const noexcept override;
    [[nodiscard]] const std::vector<ParticleSpeciesMetadata>& particleSpecies()
        const noexcept override;

    [[nodiscard]] ViewDataResult requestView(
        const ViewDataRequest& request, StopToken cancellation = {}) override;
    [[nodiscard]] DatasetPage requestDatasetPage(
        const DatasetPageRequest& request, StopToken cancellation = {}) override;
    [[nodiscard]] std::optional<ValueRange> requestRange(
        const RangeRequest& request, StopToken cancellation = {}) override;
    [[nodiscard]] bool rangeAvailable(
        const RangeRequest& request) const noexcept override;
    [[nodiscard]] ParticleSample requestParticleSample(
        const std::string& species, double fraction, std::uint64_t seed,
        StopToken cancellation = {}) override;

    [[nodiscard]] CacheMetrics cacheMetrics() const override;
    [[nodiscard]] bool setCacheBudget(std::uint64_t bytes) override;
    void clearUnpinnedCache() override;
    void close() noexcept override;

private:
    [[nodiscard]] std::shared_ptr<PlotfileDataset> requireDataset() const;

    DatasetId m_id;
    DatasetMetadata m_metadata;
    MetadataReadMetrics m_metadataMetrics;
    std::string m_fileVersion;
    std::vector<ParticleSpeciesMetadata> m_particleSpecies;
    mutable std::mutex m_mutex;
    std::shared_ptr<PlotfileDataset> m_dataset;
};

} // namespace amrvis
