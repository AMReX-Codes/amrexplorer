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
        std::filesystem::path plotfile, DatasetId id,
        std::uint64_t cacheBudgetBytes, StopToken cancellation = {});
    PlotfileDataset(std::filesystem::path dataRoot, DatasetId id,
        std::uint64_t cacheBudgetBytes, PlotfileMetadataResult metadata,
        StopToken cancellation = {});

    [[nodiscard]] const DatasetMetadata& metadata() const noexcept;
    [[nodiscard]] const MetadataReadMetrics& metadataReadMetrics() const noexcept;
    // The Header's version *token* -- its first whitespace-delimited word --
    // as the bounded metadata read already parsed it. Exposed so no caller has
    // to open and re-read the Header for it.
    //
    // Narrower than the whole first line, which is what the session used to
    // re-read with its own getline. AMReX writes the version alone on that
    // line ("HyperCLaw-V1.1"), so the two agree on every real plotfile; they
    // differ only where a Header carries something after it, and this is the
    // string the UI's Format field and the wire catalog show.
    [[nodiscard]] const std::string& fileVersion() const noexcept;
    [[nodiscard]] DatasetId id() const noexcept;
    [[nodiscard]] const std::vector<ParticleSpeciesMetadata>& particleSpecies()
        const noexcept;
    [[nodiscard]] ParticleSample requestParticleSample(
        const std::string& species, double fraction, std::uint64_t seed = 0,
        StopToken cancellation = {},
        std::size_t maximumPoints = std::numeric_limits<std::size_t>::max()) const;
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
    // Per-dataset, not global: unrelated datasets read in parallel. Acquired
    // with a try_lock/sleep poll loop so a waiter can observe its StopToken
    // instead of blocking a whole block read long. A plain mutex, not
    // std::timed_mutex: libstdc++ implements try_lock_for via
    // pthread_mutex_clocklock, which older ThreadSanitizer runtimes (e.g.
    // GCC 13 on ubuntu-24.04 CI) do not intercept — TSan then misses the
    // lock and reports the destructor's unlock as "unlock of an unlocked
    // mutex". try_lock is intercepted everywhere. (The mutex member makes
    // PlotfileDataset non-movable, which is fine — it is only ever a stack
    // local or held via shared_ptr.)
    std::mutex m_ioMutex;
};

} // namespace amrvis
