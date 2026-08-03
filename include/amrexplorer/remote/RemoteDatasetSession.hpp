#pragma once

#include <amrexplorer/data/DatasetSession.hpp>
#include <amrexplorer/remote/Connection.hpp>

#include <memory>
#include <mutex>
#include <string>

namespace amrvis::remote {

class RemoteDatasetSession final : public DatasetSession {
public:
    [[nodiscard]] static std::shared_ptr<RemoteDatasetSession> open(
        std::shared_ptr<Connection> connection, const std::string& path,
        std::uint64_t cacheBudgetBytes, StopToken cancellation = {});

    ~RemoteDatasetSession() override;

    [[nodiscard]] DatasetId id() const noexcept override;
    [[nodiscard]] const DatasetMetadata& metadata() const noexcept override;
    [[nodiscard]] const MetadataReadMetrics& metadataReadMetrics()
        const noexcept override;
    [[nodiscard]] const std::string& fileVersion() const noexcept override;
    [[nodiscard]] const std::vector<ParticleSpeciesMetadata>& particleSpecies()
        const noexcept override;
    [[nodiscard]] std::optional<std::uint32_t> maximumResponseBytes()
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

    [[nodiscard]] std::shared_ptr<Connection> connection() const noexcept;
    [[nodiscard]] const std::string& remotePath() const noexcept;

private:
    RemoteDatasetSession(std::shared_ptr<Connection> connection,
        std::string path, OpenedDataset opened);
    void requireOpen() const;

    std::shared_ptr<Connection> m_connection;
    std::string m_path;
    DatasetId m_id;
    DatasetMetadata m_metadata;
    MetadataReadMetrics m_metadataMetrics;
    std::string m_fileVersion;
    std::vector<ParticleSpeciesMetadata> m_particleSpecies;
    std::vector<std::uint8_t> m_fileRangeAvailable;
    std::vector<std::uint8_t> m_levelRangeAvailable;
    mutable std::mutex m_mutex;
    bool m_open = true;
};

} // namespace amrvis::remote
