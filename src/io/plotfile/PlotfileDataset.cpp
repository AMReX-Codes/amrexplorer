#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace amrvis {
namespace {

std::mutex& globalAmrexIoMutex()
{
    static std::mutex mutex;
    return mutex;
}

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

} // namespace

PlotfileDataset::PlotfileDataset(
    std::filesystem::path plotfile, DatasetId id, std::uint64_t cacheBudgetBytes)
    : m_plotfile(sourceDataRoot(plotfile))
    , m_id(id)
    , m_metadataResult(readDatasetMetadata(plotfile))
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

    std::scoped_lock ioLock(globalAmrexIoMutex());
    if (cancellation.stop_requested()) {
        throw ReadCancelled();
    }
    if (auto cached = m_cache.findAndPin(key)) {
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
