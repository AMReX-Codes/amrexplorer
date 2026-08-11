#include <amrexplorer/remote/RemoteDatasetSession.hpp>

#include <amrexplorer/data/SessionValidation.hpp>

#include <stdexcept>
#include <utility>
#include <variant>

namespace amrvis::remote {
namespace {

// One policy over everything a response can fail at: the transport and the
// decode inside the connection, and the contextual validation after it. A peer
// that cannot answer in the protocol we negotiated is not one whose next answer
// should be trusted, so the connection goes rather than just the request -- and
// that has to include a failure raised while decoding, where there is no result
// to validate yet and where the server may be holding an opened dataset for a
// session we are about to abandon.
//
// Two exceptions pass through untouched: RemoteError is the server answering
// properly, with an error, and ReadCancelled is our own stop.
template <typename Operation>
decltype(auto) refusingInvalidResponses(
    Connection& connection, Operation&& operation)
{
    try {
        return operation();
    } catch (const RemoteError&) {
        throw;
    } catch (const ReadCancelled&) {
        throw;
    } catch (...) {
        connection.close();
        throw;
    }
}

} // namespace

std::shared_ptr<RemoteDatasetSession> RemoteDatasetSession::open(
    std::shared_ptr<Connection> connection, const std::string& path,
    std::uint64_t cacheBudgetBytes, StopToken cancellation)
{
    if (!connection) {
        throw std::invalid_argument(
            "remote dataset requires a connection");
    }
    // The catalog is validated while it is decoded, so an impossible one throws
    // in here rather than at a later call.
    auto opened = refusingInvalidResponses(*connection, [&] {
        return connection->openDataset(path, cacheBudgetBytes, cancellation);
    });
    return std::shared_ptr<RemoteDatasetSession>(
        new RemoteDatasetSession(
            std::move(connection), path, std::move(opened)));
}

RemoteDatasetSession::RemoteDatasetSession(
    std::shared_ptr<Connection> connection, std::string path,
    OpenedDataset opened)
    : m_connection(std::move(connection))
    , m_path(std::move(path))
    , m_id(opened.id)
    , m_metadata(std::move(opened.catalog))
    , m_metadataMetrics(opened.metadataMetrics)
    , m_fileVersion(std::move(opened.fileVersion))
    , m_particleSpecies(std::move(opened.particleSpecies))
    , m_fileRangeAvailable(std::move(opened.fileRangeAvailable))
    , m_levelRangeAvailable(std::move(opened.levelRangeAvailable))
{
}

RemoteDatasetSession::~RemoteDatasetSession()
{
    close();
}

DatasetId RemoteDatasetSession::id() const noexcept
{
    return m_id;
}

const DatasetMetadata& RemoteDatasetSession::metadata() const noexcept
{
    return m_metadata;
}

const MetadataReadMetrics&
RemoteDatasetSession::metadataReadMetrics() const noexcept
{
    return m_metadataMetrics;
}

const std::string& RemoteDatasetSession::fileVersion() const noexcept
{
    return m_fileVersion;
}

const std::vector<ParticleSpeciesMetadata>&
RemoteDatasetSession::particleSpecies() const noexcept
{
    return m_particleSpecies;
}

std::optional<std::uint32_t>
RemoteDatasetSession::maximumResponseBytes() const noexcept
{
    return m_connection->serverInfo().maximumFrameBytes;
}

ViewDataResult RemoteDatasetSession::requestView(
    const ViewDataRequest& request, StopToken cancellation)
{
    requireOpen();
    validateSessionViewRequest(m_metadata, m_id, request);
    return refusingInvalidResponses(*m_connection, [&] {
        auto result = m_connection->requestView(request, cancellation);
        validateSessionViewResult(m_metadata, request, result);
        return result;
    });
}

DatasetPage RemoteDatasetSession::requestDatasetPage(
    const DatasetPageRequest& request, StopToken cancellation)
{
    requireOpen();
    validateSessionDatasetPageRequest(m_metadata, m_id, request);
    return refusingInvalidResponses(*m_connection, [&] {
        auto page = m_connection->requestDatasetPage(request, cancellation);
        validateSessionDatasetPageResult(m_metadata, request, page);
        return page;
    });
}

std::optional<ValueRange> RemoteDatasetSession::requestRange(
    const RangeRequest& request, StopToken cancellation)
{
    requireOpen();
    validateSessionRangeRequest(m_metadata, request);
    return refusingInvalidResponses(*m_connection, [&] {
        return m_connection->requestRange(m_id, request, cancellation);
    });
}

bool RemoteDatasetSession::rangeAvailable(
    const RangeRequest& request) const noexcept
{
    if (request.field.value >= m_metadata.fields.size()
        || request.maximumLevel < 0
        || request.maximumLevel > m_metadata.finestLevel) {
        return false;
    }
    const auto field = static_cast<std::size_t>(request.field.value);
    if (request.scope == RangeScope::File) {
        return field < m_fileRangeAvailable.size()
            && m_fileRangeAvailable[field] != 0;
    }
    const auto levelCount = m_metadata.levels.size();
    const auto availableAt = [&](int level) {
        const auto index = field * levelCount
            + static_cast<std::size_t>(level);
        return index < m_levelRangeAvailable.size()
            && m_levelRangeAvailable[index] != 0;
    };
    if (request.composition == CompositionPolicy::ExactLevel) {
        return availableAt(request.maximumLevel);
    }
    for (int level = 0; level <= request.maximumLevel; ++level) {
        if (!availableAt(level)) {
            return false;
        }
    }
    return true;
}

ParticleSample RemoteDatasetSession::requestParticleSample(
    const std::string& species, double fraction, std::uint64_t seed,
    StopToken cancellation)
{
    requireOpen();
    validateSessionParticleRequest(
        m_metadata, m_particleSpecies, species, fraction);
    return refusingInvalidResponses(*m_connection, [&] {
        auto sample = m_connection->requestParticleSample(
            m_id, species, fraction, seed, cancellation);
        validateSessionParticleSampleResult(
            m_particleSpecies, species, sample);
        return sample;
    });
}

CacheMetrics RemoteDatasetSession::cacheMetrics() const
{
    requireOpen();
    return m_connection->latestCache(m_id);
}

bool RemoteDatasetSession::setCacheBudget(std::uint64_t bytes)
{
    requireOpen();
    return refusingInvalidResponses(*m_connection, [&] {
        return m_connection->setCacheBudget(m_id, bytes).withinBudget();
    });
}

void RemoteDatasetSession::clearUnpinnedCache()
{
    requireOpen();
    refusingInvalidResponses(*m_connection, [&] {
        static_cast<void>(m_connection->clearCache(m_id));
    });
}

void RemoteDatasetSession::close() noexcept
{
    {
        std::scoped_lock lock(m_mutex);
        if (!m_open) {
            return;
        }
        m_open = false;
    }
    if (m_connection->connected()) {
        m_connection->closeDatasetBestEffort(m_id);
    }
}

std::shared_ptr<Connection> RemoteDatasetSession::connection() const noexcept
{
    return m_connection;
}

const std::string& RemoteDatasetSession::remotePath() const noexcept
{
    return m_path;
}

void RemoteDatasetSession::requireOpen() const
{
    std::scoped_lock lock(m_mutex);
    if (!m_open) {
        throw std::runtime_error("remote dataset session is closed");
    }
}

} // namespace amrvis::remote
