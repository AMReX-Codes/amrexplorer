#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace amrvis {
namespace {

// How often a thread waiting for the per-dataset IO lock wakes to re-check its
// cancellation token, so cancellation latency is bounded by this interval
// rather than by the in-progress block read (potentially seconds for ~1 GB).
constexpr auto ioLockPollInterval = std::chrono::milliseconds(25);

std::uint64_t residentBytes(const FabBlock& block)
{
    return static_cast<std::uint64_t>(sizeof(FabBlock))
        + block.values.residentBytes();
}

std::filesystem::path sourceDataRoot(const std::filesystem::path& path)
{
    if (std::filesystem::is_directory(path)
        && std::filesystem::is_regular_file(path / "Header")) {
        return path;
    }
    const auto parent = path.parent_path();
    return parent.empty() ? std::filesystem::path(".") : parent;
}

std::vector<ParticleSpeciesMetadata> discoverParticlesForPlotfileRoot(
    const std::filesystem::path& root)
{
    if (std::filesystem::is_directory(root)
        && std::filesystem::is_regular_file(root / "Header")) {
        return discoverParticleSpecies(root);
    }
    return {};
}

} // namespace

PlotfileDataset::PlotfileDataset(
    std::filesystem::path plotfile, DatasetId id, std::uint64_t cacheBudgetBytes)
    : m_plotfile(sourceDataRoot(plotfile))
    , m_id(id)
    , m_metadataResult(readDatasetMetadata(plotfile))
    , m_particleSpecies(std::filesystem::is_directory(plotfile)
            ? discoverParticleSpecies(m_plotfile)
            : std::vector<ParticleSpeciesMetadata>{})
    , m_blockReader(m_plotfile, m_metadataResult.metadata)
    , m_cache(cacheBudgetBytes)
{
    if (m_id.value == 0) {
        throw std::invalid_argument("PlotfileDataset id must be nonzero");
    }
}

PlotfileDataset::PlotfileDataset(std::filesystem::path root, DatasetId id,
    std::uint64_t cacheBudgetBytes, PlotfileMetadataResult metadata)
    : m_plotfile(std::move(root))
    , m_id(id)
    , m_metadataResult(std::move(metadata))
    , m_particleSpecies(discoverParticlesForPlotfileRoot(m_plotfile))
    , m_blockReader(m_plotfile, m_metadataResult.metadata)
    , m_cache(cacheBudgetBytes)
{
    if (m_id.value == 0 || !m_metadataResult.metadata) {
        throw std::invalid_argument("selected FAB dataset requires metadata and an id");
    }
}

const DatasetMetadata& PlotfileDataset::metadata() const noexcept
{
    return *m_metadataResult.metadata;
}

const MetadataReadMetrics& PlotfileDataset::metadataReadMetrics() const noexcept
{
    return m_metadataResult.metrics;
}

DatasetId PlotfileDataset::id() const noexcept
{
    return m_id;
}

const std::vector<ParticleSpeciesMetadata>& PlotfileDataset::particleSpecies()
    const noexcept
{
    return m_particleSpecies;
}

ParticleSample PlotfileDataset::requestParticleSample(
    const std::string& species, double fraction, std::uint64_t seed,
    StopToken cancellation) const
{
    return readParticleSample(
        m_plotfile, species, fraction, seed, cancellation);
}

const std::filesystem::path& PlotfileDataset::dataRoot() const noexcept
{
    return m_plotfile;
}

PlotfileDataset::BlockAccess PlotfileDataset::requestBlock(
    const BlockRequest& request, StopToken cancellation)
{
    if (request.dataset != m_id) {
        throw BlockReadError("block request targets a different dataset");
    }
    const auto key = makeBlockKey(request);
    if (auto cached = m_cache.findAndPin(key)) {
        return {std::move(cached), true, {}};
    }

    // Acquire the per-dataset IO lock, polling the cancellation token while we
    // wait so a request can bail promptly instead of being stuck behind a
    // long in-progress read on this dataset. try_lock + sleep rather than a
    // timed lock: see the m_ioMutex comment (uninstrumented
    // pthread_mutex_clocklock under older TSan runtimes). The uncontended
    // path acquires on the first try_lock with no sleep; under contention
    // acquisition lands within one poll interval of the holder finishing.
    std::unique_lock<std::mutex> ioLock(m_ioMutex, std::defer_lock);
    while (!ioLock.try_lock()) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        std::this_thread::sleep_for(ioLockPollInterval);
    }
    if (cancellation.stop_requested()) {
        throw ReadCancelled();
    }
    // The logical lookup's miss was already recorded by the findAndPin above;
    // this second, in-lock check only catches a block a racing thread inserted
    // while we waited for the IO lock, so it must not count again.
    if (auto cached = m_cache.peekAndPin(key)) {
        return {std::move(cached), true, {}};
    }

    auto read = m_blockReader.readBlock(request, cancellation);
    auto handle = m_cache.insertAndPin(
        key, read.block, residentBytes(*read.block));
    return {std::move(handle), false, read.metrics};
}

CacheMetrics PlotfileDataset::cacheMetrics() const
{
    return m_cache.metrics();
}

bool PlotfileDataset::setCacheBudget(std::uint64_t bytes)
{
    return m_cache.setBudget(bytes);
}

void PlotfileDataset::clearUnpinnedCache()
{
    m_cache.clearUnpinned();
}

} // namespace amrvis
