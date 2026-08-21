#include <amrexplorer/data/LocalDatasetSession.hpp>

#include <amrexplorer/core/Statistics.hpp>
#include <amrexplorer/data/SessionValidation.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/query/LineQuery.hpp>
#include <amrexplorer/query/SliceQuery.hpp>
#include <amrexplorer/query/VolumeQuery.hpp>
#include <amrexplorer/render3d/VolumeRaycaster.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace amrvis {
namespace {

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

// The range a volume's colours span when the request leaves it to the
// renderer (the "Visible" range): the sampled grid's finite extrema, padded
// if degenerate, logarithmic only when the data allows it, and a neutral
// range for a grid with no finite values -- resolveRange's rules for a
// plane, applied to a grid.
VolumeRange visibleVolumeRange(const VolumeGrid& grid, bool logarithmic)
{
    if (logarithmic) {
        if (const auto extrema = volumeGridRange(grid, true)) {
            const auto [minimum, maximum]
                = paddedIfDegenerate(extrema->first, extrema->second, true);
            if (minimum < maximum && minimum > 0.0) {
                return {minimum, maximum, true};
            }
        } else if (grid.coveredVoxels == 0) {
            return {1.0, 10.0, true};
        }
    }
    const auto extrema = volumeGridRange(grid, false);
    if (!extrema) {
        return {0.0, 1.0, false};
    }
    const auto [minimum, maximum]
        = paddedIfDegenerate(extrema->first, extrema->second, false);
    return {minimum, maximum, false};
}

} // namespace

std::size_t VolumeGridKeyHash::operator()(const VolumeGridKey& key) const noexcept
{
    std::size_t seed = std::hash<std::uint32_t>{}(key.field.value);
    const auto combine = [&seed](std::size_t value) {
        seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    };
    combine(std::hash<int>{}(key.component));
    combine(std::hash<int>{}(key.maximumLevel));
    combine(std::hash<int>{}(static_cast<int>(key.composition)));
    for (std::size_t axis = 0; axis < 3; ++axis) {
        combine(std::hash<double>{}(key.region.lower[axis]));
        combine(std::hash<double>{}(key.region.upper[axis]));
        combine(std::hash<int>{}(key.dims[axis]));
    }
    return seed;
}

LocalDatasetSession::LocalDatasetSession(
    std::shared_ptr<PlotfileDataset> dataset)
    : m_id(dataset ? dataset->id() : DatasetId{})
    , m_metadata(dataset ? dataset->metadata() : DatasetMetadata{})
    , m_metadataMetrics(
          dataset ? dataset->metadataReadMetrics() : MetadataReadMetrics{})
    , m_fileVersion(dataset ? dataset->fileVersion() : std::string{})
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
          path, id, cacheBudgetBytes, cancellation))
{
}

LocalDatasetSession::LocalDatasetSession(std::filesystem::path dataRoot,
    DatasetId id, std::uint64_t cacheBudgetBytes,
    PlotfileMetadataResult metadata, StopToken cancellation)
    : LocalDatasetSession(std::make_shared<PlotfileDataset>(dataRoot, id,
          cacheBudgetBytes, std::move(metadata), cancellation))
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
        // A standalone FAB has no metadata statistic, so the File range has to
        // be scanned out of the payload. The result is immutable for this
        // (dataset, field), and the resolver asks for it on every range
        // resolve, so scan once.
        //
        // The check precedes the memo for the same reason it precedes the scan:
        // the block read this used to always perform answered an already-
        // cancelled token with ReadCancelled, and a memo hit must not turn
        // abandoned work into a successful resolve.
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        {
            std::scoped_lock lock(m_mutex);
            if (const auto found = m_fabRanges.find(request.field.value);
                found != m_fabRanges.end()) {
                return found->second;
            }
        }
        const auto access = [&] {
            BlockRequest block;
            block.dataset = m_id;
            block.field = request.field;
            return dataset->requestBlock(block, cancellation);
        }();
        auto minimum = std::numeric_limits<double>::infinity();
        auto maximum = -std::numeric_limits<double>::infinity();
        const auto& values = access.handle->values;
        for (std::size_t index = 0; index < values.size(); ++index) {
            // The read itself polls cancellation; this pass did not, so a
            // cancelled request still walked every value of a large FAB before
            // returning. One check per chunk keeps that responsive without
            // putting a branch on every value.
            if ((index & 0xFFFFU) == 0 && cancellation.stop_requested()) {
                throw ReadCancelled();
            }
            const auto value = values[index];
            if (std::isfinite(value)) {
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        }
        const auto result = std::isfinite(minimum) && std::isfinite(maximum)
            ? std::optional<ValueRange>{ValueRange{minimum, maximum}}
            : std::nullopt;
        // Only a completed scan is recorded; a cancelled one threw above.
        std::scoped_lock lock(m_mutex);
        m_fabRanges.insert_or_assign(request.field.value, result);
        return result;
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

bool LocalDatasetSession::supportsVolumeRendering() const noexcept
{
    return m_metadata.dimension == 3 && !m_metadata.isFab
        && m_metadata.hasPhysicalGeometry;
}

VolumeFrame LocalDatasetSession::renderVolume(
    const VolumeRenderRequest& request, StopToken cancellation)
{
    const auto dataset = requireDataset();
    validateSessionVolumeRequest(m_metadata, m_id, request);
    if (!supportsVolumeRendering()) {
        throw std::invalid_argument(
            "volume rendering requires a 3-D plotfile with physical geometry");
    }
    const auto maximumLevel = std::min(request.maximumLevel, m_metadata.finestLevel);
    const VolumeGridKey key{request.field, request.component, maximumLevel,
        request.composition, request.region,
        volumeGridDims(m_metadata, request.region, maximumLevel,
            request.maximumVoxels)};

    // The grid: from the cache when the same sample was rendered before
    // (a camera change re-casts it), else sampled now and cached -- unless
    // it is larger than the whole grid budget, in which case it is rendered
    // from the local copy and forgotten.
    VolumeRenderMetrics metrics;
    auto handle = m_volumeGrids.findAndPin(key);
    std::shared_ptr<const VolumeGrid> grid;
    if (handle) {
        metrics.gridFromCache = true;
        grid = handle.value();
    } else {
        const auto started = std::chrono::steady_clock::now();
        VolumeSampleRequest sample;
        sample.dataset = request.dataset;
        sample.field = request.field;
        sample.component = request.component;
        sample.maximumLevel = maximumLevel;
        sample.composition = request.composition;
        sample.region = request.region;
        sample.maximumVoxels = request.maximumVoxels;
        auto sampled = VolumeQuery(*dataset).execute(sample, cancellation);
        metrics.sampleMicroseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count());
        metrics.candidateBlocks = sampled.metrics.candidateBlocks;
        metrics.blocksRead = sampled.metrics.blocksRead;
        metrics.cacheHits = sampled.metrics.cacheHits;
        metrics.payloadBytesRead = sampled.metrics.payloadBytesRead;
        const auto bytes = static_cast<std::uint64_t>(sampled.grid.values.size())
            * sizeof(float);
        auto owned = std::make_shared<const VolumeGrid>(std::move(sampled.grid));
        try {
            handle = m_volumeGrids.insertAndPin(key, owned, bytes);
            grid = handle.value();
        } catch (const CacheBudgetExceeded&) {
            // Too big for the grid cache: render it anyway, uncached. (The
            // block cache's own budget failures come out of the sample above
            // and propagate, so the caller can fall back to a coarser level.)
            grid = std::move(owned);
        }
    }

    RaycastSettings settings;
    settings.camera = request.camera;
    settings.domain = datasetSampleBounds(m_metadata);
    settings.outputSize = request.outputSize;
    settings.range = request.range
        ? *request.range : visibleVolumeRange(*grid, request.logarithmic);
    settings.transfer = request.transfer;
    settings.samplesPerVoxel = request.samplesPerVoxel;
    auto frame = raycastVolume(*grid, settings, cancellation);
    const auto renderMicroseconds = frame.metrics.renderMicroseconds;
    frame.metrics = metrics;
    frame.metrics.renderMicroseconds = renderMicroseconds;
    frame.metrics.gridDims = grid->dims;
    frame.metrics.coveredVoxels = grid->coveredVoxels;
    frame.metrics.sampledMaximumLevel = grid->maximumLevel;
    return frame;
}

bool LocalDatasetSession::setVolumeGridCacheBudget(std::uint64_t bytes)
{
    return m_volumeGrids.setBudget(bytes);
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
    m_volumeGrids.clearUnpinned();
}

void LocalDatasetSession::close() noexcept
{
    std::scoped_lock lock(m_mutex);
    m_dataset.reset();
    m_volumeGrids.clearUnpinned();
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
