#pragma once

#include <amrexplorer/data/DatasetSession.hpp>
#include <amrexplorer/remote/Connection.hpp>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace amrvis::remote {

class RemoteDatasetSession final : public DatasetSession {
public:
    // `derivedFields` are installed by the server on the session it opens
    // (protocol 1.4). A non-empty list against a peer that predates it is
    // refused here, before the connection is used, so the refusal cannot be
    // mistaken for a peer that stopped speaking the protocol.
    [[nodiscard]] static std::shared_ptr<RemoteDatasetSession> open(
        std::shared_ptr<Connection> connection, const std::string& path,
        std::uint64_t cacheBudgetBytes, StopToken cancellation = {},
        std::vector<DerivedFieldDefinition> derivedFields = {});

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
    // True when the server speaks protocol 1.2 and the dataset is a 3-D
    // plotfile; the frame comes back rendered, validated against the request.
    [[nodiscard]] bool supportsVolumeRendering() const noexcept override;
    [[nodiscard]] bool supportsVolumeSampling() const noexcept override;
    // True when the server speaks protocol 1.4 -- which is also the only way
    // this session could have installed the list it was opened with, which is
    // what the GUI's reload loop relies on (see DatasetSession).
    [[nodiscard]] bool supportsDerivedFields() const noexcept override;
    [[nodiscard]] std::size_t storedFieldCount() const noexcept override;
    [[nodiscard]] std::vector<DerivedFieldSkip> skippedDerivedFields()
        const override;
    [[nodiscard]] std::vector<DerivedFieldDefinition> derivedFieldDefinitions()
        const override;
    [[nodiscard]] VolumeFrame renderVolume(const VolumeRenderRequest& request,
        StopToken cancellation = {}) override;
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
        std::string path, OpenedDataset opened,
        std::vector<DerivedFieldDefinition> derivedFields);
    void requireOpen() const;

    // A range is immutable for a (dataset, field, level, composition, scope),
    // and the dataset is fixed for the life of this session, so a successful
    // answer never has to be asked for twice. The UI defaults to File range
    // mode and resolves it after every slice, so without this each pan, zoom,
    // sequence frame, and cosmetic re-render paid an extra serialized round
    // trip -- and a logarithmic display resolves twice per render.
    //
    // RangeRequest has no equality or hash, so the key is a tuple of exactly
    // the fields the server distinguishes.
    using RangeKey = std::tuple<std::uint32_t, int, std::uint8_t, std::uint8_t>;
    [[nodiscard]] static RangeKey rangeKey(const RangeRequest& request) noexcept;

    std::shared_ptr<Connection> m_connection;
    std::string m_path;
    DatasetId m_id;
    DatasetMetadata m_metadata;
    MetadataReadMetrics m_metadataMetrics;
    std::string m_fileVersion;
    std::vector<ParticleSpeciesMetadata> m_particleSpecies;
    std::vector<std::uint8_t> m_fileRangeAvailable;
    std::vector<std::uint8_t> m_levelRangeAvailable;
    // The list this session was opened with, kept as it was sent rather than
    // rebuilt from the reply: a caller comparing its own list against this one
    // is asking what this session was built from, and a reply-borne copy would
    // differ from it in ways nothing here could see (an absent wire string
    // decodes as an empty one).
    std::vector<DerivedFieldDefinition> m_derivedFieldDefinitions;
    std::size_t m_storedFieldCount = 0;
    std::vector<DerivedFieldSkip> m_derivedFieldSkips;
    mutable std::mutex m_mutex;
    bool m_open = true;

    // Successful answers only. An empty optional is one of them -- the wire
    // carries "has range" as a boolean, so "this field has no range" is an
    // answer rather than an absence. Errors, cancellations, and disconnects are
    // never recorded, so none of them can poison a later retry.
    //
    // m_rangeInFlight plus the condition variable coalesce concurrent identical
    // misses into one transaction: the first caller for a key does the work and
    // the rest wait for it. A waiter whose leader failed takes the work over
    // itself rather than inheriting the failure. Neither mutex is ever held
    // across network I/O.
    mutable std::mutex m_rangeMutex;
    mutable std::condition_variable m_rangeReady;
    std::map<RangeKey, std::optional<ValueRange>> m_ranges;
    std::set<RangeKey> m_rangeInFlight;

};

} // namespace amrvis::remote
