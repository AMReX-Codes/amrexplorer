#pragma once

#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/data/DatasetSession.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace amrvis {

class PlotfileDataset;

// The sampled volume grids a local session keeps between renders (rotating
// the camera re-casts a cached grid instead of re-reading the plotfile),
// keyed by everything the sample depends on.

struct VolumeGridKey {
    FieldId field;
    int component = 0;
    int maximumLevel = 0;
    CompositionPolicy composition = CompositionPolicy::FinestAvailable;
    RealBox region;
    std::array<int, 3> dims{0, 0, 0};
    friend bool operator==(const VolumeGridKey&, const VolumeGridKey&) = default;
};

struct VolumeGridKeyHash {
    [[nodiscard]] std::size_t operator()(const VolumeGridKey& key) const noexcept;
};

// The range a volume's colours span when a request leaves it to the renderer
// (the "Visible" range), resolved from the sampled grid: its finite extrema,
// padded if degenerate, logarithmic only when every finite value allows it,
// and a neutral range when the grid holds none. Declared here rather than
// kept file-local because the remote session has to answer it the same way,
// and because the rule -- which has to agree with the slice path's
// resolveRange -- is worth pinning on its own. Scans the grid, so it throws
// ReadCancelled when the token stops.
[[nodiscard]] VolumeRange visibleVolumeRange(
    const VolumeGrid& grid, bool logarithmic, StopToken cancellation = {});

class LocalDatasetSession final : public DatasetSession {
public:
    // The file version comes from the dataset's own metadata read; nothing
    // re-opens the Header for it.
    // `derivedFields` are passed straight to the dataset, which resolves them
    // against its stored field list and leaves out any that do not resolve,
    // reporting them through skippedDerivedFields() rather than failing the
    // open (see PlotfileDataset).
    explicit LocalDatasetSession(std::shared_ptr<PlotfileDataset> dataset);
    LocalDatasetSession(const std::filesystem::path& path, DatasetId id,
        std::uint64_t cacheBudgetBytes, StopToken cancellation = {},
        std::vector<DerivedFieldDefinition> derivedFields = {});
    LocalDatasetSession(std::filesystem::path dataRoot, DatasetId id,
        std::uint64_t cacheBudgetBytes, PlotfileMetadataResult metadata,
        StopToken cancellation = {},
        std::vector<DerivedFieldDefinition> derivedFields = {});

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
    [[nodiscard]] ParticleSample requestParticleSample(
        const std::string& species, double fraction, std::uint64_t seed,
        std::size_t maximumPoints, StopToken cancellation = {});

    // A 3-D plotfile with physical geometry can be volume-rendered.
    [[nodiscard]] bool supportsVolumeRendering() const noexcept override;
    // Whatever this can render, it can sample either way: the choice only
    // ever fails to reach a peer, and there is no peer here.
    [[nodiscard]] bool supportsVolumeSampling() const noexcept override
    {
        return supportsVolumeRendering();
    }
    [[nodiscard]] VolumeFrame renderVolume(const VolumeRenderRequest& request,
        StopToken cancellation = {}) override;
    // The same render with a bound on the threads it may use, or 0 to leave
    // the choice to the ray caster (which takes hardware concurrency). A
    // server already runs each request on a worker of its own pool, so
    // without a bound the two multiply. Passed per render rather than set on
    // the session: concurrent renders would clobber shared state, and each
    // one would then run on whichever count landed last.
    [[nodiscard]] VolumeFrame renderVolume(const VolumeRenderRequest& request,
        StopToken cancellation, unsigned renderThreads);
    // The sampled-grid cache's budget; grids larger than it are rendered
    // uncached. Returns whether the cache fits the new budget, which a zero
    // budget does (every unpinned grid is evicted and nothing is resident);
    // it is false only while a pinned grid still exceeds it.
    [[nodiscard]] bool setVolumeGridCacheBudget(std::uint64_t bytes);

    // Locally there is nothing between the definitions and the dataset that
    // opens with them.
    [[nodiscard]] bool supportsDerivedFields() const noexcept override
    {
        return true;
    }
    [[nodiscard]] std::size_t storedFieldCount() const noexcept override;
    [[nodiscard]] std::vector<DerivedFieldSkip> skippedDerivedFields()
        const override;
    [[nodiscard]] std::vector<DerivedFieldDefinition> derivedFieldDefinitions()
        const override;

    // The block pool alone. setCacheBudget deliberately moves both pools,
    // which is what a user setting one number wants -- but a server applying
    // a *client's* requested budget must not let it raise the sampled-grid
    // cache past the limit the operator set with --volume-cache-mib.
    [[nodiscard]] bool setBlockCacheBudget(std::uint64_t bytes);

    // The sampled-grid pool on its own. cacheMetrics() reports the block
    // pool, because CacheMetrics carries one budget and one hit rate and the
    // remote snapshot asserts that budget is the one setCacheBudget was
    // given; until it can carry both, this is how the grid pool is observed.
    [[nodiscard]] CacheMetrics volumeGridCacheMetrics() const;
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
    std::size_t m_storedFieldCount = 0;
    std::vector<DerivedFieldSkip> m_skippedDerivedFields;
    std::vector<DerivedFieldDefinition> m_derivedFieldDefinitions;
    mutable std::mutex m_mutex;
    std::shared_ptr<PlotfileDataset> m_dataset;
    // File-scope ranges for a standalone FAB, by field. These are scanned out
    // of the payload rather than read from a statistic, and they never change,
    // so the scan happens once per field. Cached even when the answer is "no
    // finite values", which costs the same scan to discover. Cancelled scans
    // record nothing.
    std::map<std::uint32_t, std::optional<ValueRange>> m_fabRanges;
    // Seeded by the constructor from the budget the dataset was opened with,
    // and kept in step by setCacheBudget. It starts empty rather than on a
    // default of its own: a constant here would be overwritten before the
    // cache was ever used, and would only mislead a reader about the budget
    // that actually applies.
    ByteLruCache<VolumeGridKey, VolumeGrid, VolumeGridKeyHash> m_volumeGrids{0};
    // The last Visible range and what it was resolved from. Resolving it
    // walks every voxel, so a camera rotation -- which reuses the cached grid
    // and asks the same question of it -- would otherwise rescan the whole
    // grid before each cast. One entry is enough: the interactive case is the
    // same key and mapping frame after frame.
    std::optional<std::pair<VolumeGridKey, bool>> m_visibleRangeFor;
    VolumeRange m_visibleRange;
};

} // namespace amrvis
