#pragma once

#include <amrexplorer/cache/BlockKey.hpp>
#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/ParticleReader.hpp>

#include <cstdint>
#include <filesystem>
#include <mutex>

namespace amrvis {

class PlotfileDataset {
public:
    using BlockCache = ByteLruCache<BlockKey, FabBlock, BlockKeyHash>;

    struct BlockAccess {
        BlockCache::Handle handle;
        bool cacheHit = false;
        BlockReadMetrics io;
    };

    PlotfileDataset(
        std::filesystem::path plotfile, DatasetId id, std::uint64_t cacheBudgetBytes);
    PlotfileDataset(std::filesystem::path dataRoot, DatasetId id,
        std::uint64_t cacheBudgetBytes, PlotfileMetadataResult metadata);

    [[nodiscard]] const DatasetMetadata& metadata() const noexcept;
    [[nodiscard]] const MetadataReadMetrics& metadataReadMetrics() const noexcept;
    [[nodiscard]] DatasetId id() const noexcept;
    [[nodiscard]] const std::vector<ParticleSpeciesMetadata>& particleSpecies()
        const noexcept;
    [[nodiscard]] ParticleSample requestParticleSample(
        const std::string& species, double fraction, std::uint64_t seed = 0,
        StopToken cancellation = {}) const;
    [[nodiscard]] const std::filesystem::path& dataRoot() const noexcept;

    [[nodiscard]] BlockAccess requestBlock(
        const BlockRequest& request, StopToken cancellation = {});

    [[nodiscard]] CacheMetrics cacheMetrics() const;
    [[nodiscard]] bool setCacheBudget(std::uint64_t bytes);
    void clearUnpinnedCache();

private:
    std::filesystem::path m_plotfile;
    DatasetId m_id;
    PlotfileMetadataResult m_metadataResult;
    std::vector<ParticleSpeciesMetadata> m_particleSpecies;
    PlotfileBlockReader m_blockReader;
    BlockCache m_cache;
    // Serializes this dataset's block reads so concurrent misses of the same
    // block read it once (see the double-checked lookup in requestBlock).
    // Per-dataset, not global: unrelated datasets read in parallel. A timed
    // mutex lets a waiter poll its StopToken instead of blocking a whole block
    // read long. (This makes PlotfileDataset non-movable, which is fine — it
    // is only ever a stack local or held via shared_ptr.)
    std::timed_mutex m_ioMutex;
};

} // namespace amrvis
