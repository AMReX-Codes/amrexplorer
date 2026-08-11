#include <amrexplorer/data/LocalDatasetSession.hpp>

#include <amrexplorer/core/Statistics.hpp>
#include <amrexplorer/data/SessionValidation.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/detail/FabHeaderParsing.hpp>
#include <amrexplorer/query/LineQuery.hpp>
#include <amrexplorer/query/SliceQuery.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace amrvis {
namespace {

std::string readFileVersion(
    const std::filesystem::path& path, StopToken cancellation)
{
    if (cancellation.stop_requested()) {
        throw ReadCancelled();
    }
    std::ifstream header(path / "Header");
    std::string version;
    // Bounded, like the FAB header reads: this runs on every dataset open,
    // including inside the long-lived server, and a Header whose first line
    // never ends would otherwise be pulled into memory whole. A version string
    // is a few dozen bytes; anything past the ceiling is not one.
    //
    // Empty rather than a throw, unlike the FAB readers: this string is
    // informational, the caller already treats an unreadable Header as an
    // empty version, and one unparsable line should not fail the open.
    try {
        static_cast<void>(
            detail::readBoundedLine<MetadataReadError>(header, version));
    } catch (const MetadataReadError&) {
        return {};
    }
    while (!version.empty() && version.back() == '\r') {
        version.pop_back();
    }
    return version;
}

std::optional<ValueRange> compositeMetadataRange(const DatasetMetadata& metadata,
    const RangeRequest& request)
{
    if (request.scope == RangeScope::File) {
        return metadataValueRange(metadata, request.field, std::nullopt);
    }
    if (request.composition == CompositionPolicy::ExactLevel) {
        return metadataValueRange(
            metadata, request.field, request.maximumLevel);
    }
    auto minimum = std::numeric_limits<double>::infinity();
    auto maximum = -std::numeric_limits<double>::infinity();
    for (int level = 0; level <= request.maximumLevel; ++level) {
        const auto range = metadataValueRange(metadata, request.field, level);
        if (!range) {
            return std::nullopt;
        }
        minimum = std::min(minimum, range->minimum);
        maximum = std::max(maximum, range->maximum);
    }
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
        return std::nullopt;
    }
    return ValueRange{minimum, maximum};
}

} // namespace

LocalDatasetSession::LocalDatasetSession(
    std::shared_ptr<PlotfileDataset> dataset, std::string fileVersion)
    : m_id(dataset ? dataset->id() : DatasetId{})
    , m_metadata(dataset ? dataset->metadata() : DatasetMetadata{})
    , m_metadataMetrics(
          dataset ? dataset->metadataReadMetrics() : MetadataReadMetrics{})
    , m_fileVersion(std::move(fileVersion))
    , m_particleSpecies(
          dataset ? dataset->particleSpecies()
                  : std::vector<ParticleSpeciesMetadata>{})
    , m_dataset(std::move(dataset))
{
    if (!m_dataset) {
        throw std::invalid_argument("local dataset session requires a dataset");
    }
}

LocalDatasetSession::LocalDatasetSession(const std::filesystem::path& path,
    DatasetId id, std::uint64_t cacheBudgetBytes, StopToken cancellation)
    : LocalDatasetSession(std::make_shared<PlotfileDataset>(
          path, id, cacheBudgetBytes, cancellation),
          readFileVersion(path, cancellation))
{
}

LocalDatasetSession::LocalDatasetSession(std::filesystem::path dataRoot,
    DatasetId id, std::uint64_t cacheBudgetBytes,
    PlotfileMetadataResult metadata, StopToken cancellation)
    : LocalDatasetSession(std::make_shared<PlotfileDataset>(dataRoot, id,
          cacheBudgetBytes, metadata, cancellation),
          metadata.fileVersion)
{
}

DatasetId LocalDatasetSession::id() const noexcept
{
    return m_id;
}

const DatasetMetadata& LocalDatasetSession::metadata() const noexcept
{
    return m_metadata;
}

const MetadataReadMetrics&
LocalDatasetSession::metadataReadMetrics() const noexcept
{
    return m_metadataMetrics;
}

const std::string& LocalDatasetSession::fileVersion() const noexcept
{
    return m_fileVersion;
}

const std::vector<ParticleSpeciesMetadata>&
LocalDatasetSession::particleSpecies() const noexcept
{
    return m_particleSpecies;
}

ViewDataResult LocalDatasetSession::requestView(
    const ViewDataRequest& request, StopToken cancellation)
{
    const auto dataset = requireDataset();
    validateSessionViewRequest(m_metadata, m_id, request);
    return std::visit(
        [&](const auto& typedRequest) -> ViewDataResult {
            using Request = std::decay_t<decltype(typedRequest)>;
            if constexpr (std::is_same_v<Request, SliceRequest>) {
                return SliceQuery(*dataset).execute(
                    typedRequest, cancellation);
            } else {
                auto result = LineQuery(*dataset).execute(
                    typedRequest.query, cancellation);
                return boundLineToViewport(
                    std::move(result), typedRequest.outputWidth);
            }
        },
        request);
}

DatasetPage LocalDatasetSession::requestDatasetPage(
    const DatasetPageRequest& request, StopToken cancellation)
{
    const auto dataset = requireDataset();
    validateSessionDatasetPageRequest(m_metadata, m_id, request);
    return extractDatasetPage(*dataset, request, cancellation);
}

std::optional<ValueRange> LocalDatasetSession::requestRange(
    const RangeRequest& request, StopToken cancellation)
{
    const auto dataset = requireDataset();
    validateSessionRangeRequest(m_metadata, request);
    if (m_metadata.isFab && request.scope == RangeScope::File) {
        BlockRequest block;
        block.dataset = m_id;
        block.field = request.field;
        const auto access = dataset->requestBlock(block, cancellation);
        auto minimum = std::numeric_limits<double>::infinity();
        auto maximum = -std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index < access.handle->values.size();
             ++index) {
            const auto value = access.handle->values[index];
            if (std::isfinite(value)) {
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        }
        if (std::isfinite(minimum) && std::isfinite(maximum)) {
            return ValueRange{minimum, maximum};
        }
        return std::nullopt;
    }
    return compositeMetadataRange(m_metadata, request);
}

bool LocalDatasetSession::rangeAvailable(
    const RangeRequest& request) const noexcept
{
    if (request.field.value >= m_metadata.fields.size()
        || request.maximumLevel < 0
        || request.maximumLevel > m_metadata.finestLevel) {
        return false;
    }
    return (m_metadata.isFab && request.scope == RangeScope::File)
        || compositeMetadataRange(m_metadata, request).has_value();
}

ParticleSample LocalDatasetSession::requestParticleSample(
    const std::string& species, double fraction, std::uint64_t seed,
    StopToken cancellation)
{
    validateSessionParticleRequest(
        m_metadata, m_particleSpecies, species, fraction);
    return requireDataset()->requestParticleSample(
        species, fraction, seed, cancellation);
}

ParticleSample LocalDatasetSession::requestParticleSample(
    const std::string& species, double fraction, std::uint64_t seed,
    std::size_t maximumPoints, StopToken cancellation)
{
    // The bounded overload serves the remote path, whose species and fraction
    // arrive off the wire -- it needs this validation at least as much as the
    // local one above, which is the caller that already had it.
    validateSessionParticleRequest(
        m_metadata, m_particleSpecies, species, fraction);
    return requireDataset()->requestParticleSample(
        species, fraction, seed, cancellation, maximumPoints);
}

CacheMetrics LocalDatasetSession::cacheMetrics() const
{
    return requireDataset()->cacheMetrics();
}

bool LocalDatasetSession::setCacheBudget(std::uint64_t bytes)
{
    return requireDataset()->setCacheBudget(bytes);
}

void LocalDatasetSession::clearUnpinnedCache()
{
    requireDataset()->clearUnpinnedCache();
}

void LocalDatasetSession::close() noexcept
{
    std::scoped_lock lock(m_mutex);
    m_dataset.reset();
}

std::shared_ptr<PlotfileDataset> LocalDatasetSession::requireDataset() const
{
    std::scoped_lock lock(m_mutex);
    if (!m_dataset) {
        throw std::runtime_error("dataset session is closed");
    }
    return m_dataset;
}

} // namespace amrvis
