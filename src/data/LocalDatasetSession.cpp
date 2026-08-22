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

} // namespace

VolumeRange visibleVolumeRange(const VolumeGrid& grid, bool logarithmic,
    StopToken cancellation)
{
    // One scan, and over every finite value rather than the positive ones.
    // Asking volumeGridRange for the positive extrema first would answer
    // "logarithmic is fine" for any field that merely *contains* positive
    // values, and every negative voxel would then map to nothing and vanish.
    // The slice path decides it the other way round -- finiteRange keeps
    // every finite value and a non-positive minimum makes resolveRange fall
    // back to linear -- and the two views of one field have to agree.
    const auto extrema = volumeGridRange(grid, false, cancellation);
    if (!extrema) {
        // Nothing finite to show. A logarithmic request still wants a
        // logarithmic axis, so the neutral range keeps the mapping.
        return logarithmic ? VolumeRange{1.0, 10.0, true}
                           : VolumeRange{0.0, 1.0, false};
    }
    if (logarithmic && extrema->first > 0.0) {
        const auto [minimum, maximum]
            = paddedIfDegenerate(extrema->first, extrema->second, true);
        if (minimum > 0.0 && minimum < maximum) {
            return {minimum, maximum, true};
        }
    }
    const auto [minimum, maximum]
        = paddedIfDegenerate(extrema->first, extrema->second, false);
    return {minimum, maximum, false};
}

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
    // The grid cache starts on the same budget the dataset was opened with,
    // not on its own default. setCacheBudget keeps the two in step afterwards,
    // but the normal local and server open paths only construct a session --
    // they never call it -- so without this a session opened with a small
    // AMREXPLORER_CACHE_SIZE_MB still held up to the 256 MiB grid default.
    static_cast<void>(
        m_volumeGrids.setBudget(m_dataset->cacheMetrics().budgetBytes));
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
    return datasetSupportsVolumeRendering(m_metadata);
}

VolumeFrame LocalDatasetSession::renderVolume(
    const VolumeRenderRequest& request, StopToken cancellation)
{
    return renderVolume(request, cancellation, 0);
}

VolumeFrame LocalDatasetSession::renderVolume(const VolumeRenderRequest& request,
    StopToken cancellation, unsigned renderThreads)
{
    const auto dataset = requireDataset();
    // Before the request is measured against the catalog: a dataset without
    // physical geometry has index-space sample bounds, so a request checked
    // first would fail on some incidental mismatch instead of saying that
    // this dataset cannot be volume-rendered at all.
    if (!supportsVolumeRendering()) {
        throw std::invalid_argument(
            "volume rendering requires a 3-D plotfile with physical geometry");
    }
    validateSessionVolumeRequest(m_metadata, m_id, request);
    // No clamp against finestLevel: validateSessionVolumeRequest above
    // refuses a level past it, so a clamp here could only ever be a no-op --
    // and reading like one that might fire invites the assumption that the
    // key and the sampler can see a different level than the validator did.
    const auto maximumLevel = request.maximumLevel;
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
        // Through the shared helper, so the fields the validator checked are
        // the fields the sampler gets. Hand-copying them means a field added
        // to VolumeSampleRequest is validated and then silently dropped here.
        auto sampled = VolumeQuery(*dataset).execute(
            volumeSampleRequestOf(request), cancellation);
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
            // A racing thread may have inserted this key first, in which
            // case insertAndPin returns its grid and ours is discarded. The
            // grid rendered did come from the cache, so say so -- but the
            // sampling and the payload reads above genuinely happened and
            // cost what they cost, so those stay. Zeroing them would make the
            // diagnostics understate the work the process actually did, which
            // is the opposite error.
            if (grid != owned) {
                metrics.gridFromCache = true;
            }
        } catch (const CacheBudgetExceeded&) {
            // Too big for the grid cache: render it anyway, uncached. (The
            // block cache's own budget failures come out of the sample above
            // and propagate, so the caller can fall back to a coarser level.)
            grid = std::move(owned);
        }
    }

    RaycastSettings settings;
    settings.threadCount = renderThreads;
    settings.camera = request.camera;
    settings.domain = datasetSampleBounds(m_metadata);
    settings.outputSize = request.outputSize;
    if (request.range) {
        settings.range = *request.range;
    } else {
        // Memoized against the grid's own cache key: the answer depends only
        // on the grid and the requested mapping, both of which the key and
        // the flag already pin.
        //
        // The scan itself runs outside the lock. m_mutex is the session's
        // general one -- requireDataset takes it, so every other entry point
        // does -- and holding it across a walk of up to 134 million voxels
        // would stall a slice request, a cacheMetrics poll, or close() for
        // the whole scan. Two threads racing here both compute the same
        // answer from the same grid, so publishing last-writer-wins costs
        // nothing but a duplicated scan in a case that needs two misses on
        // one key at once.
        const auto want = std::pair{key, request.logarithmic};
        std::optional<VolumeRange> memo;
        {
            const std::scoped_lock lock(m_mutex);
            if (m_visibleRangeFor == want) {
                memo = m_visibleRange;
            }
        }
        if (!memo) {
            memo = visibleVolumeRange(*grid, request.logarithmic, cancellation);
            const std::scoped_lock lock(m_mutex);
            m_visibleRange = *memo;
            m_visibleRangeFor = want;
        }
        settings.range = *memo;
    }
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

bool LocalDatasetSession::setBlockCacheBudget(std::uint64_t bytes)
{
    return requireDataset()->setCacheBudget(bytes);
}

CacheMetrics LocalDatasetSession::volumeGridCacheMetrics() const
{
    return m_volumeGrids.metrics();
}

CacheMetrics LocalDatasetSession::cacheMetrics() const
{
    // The block cache only. The sampled-grid cache is a second pool with its
    // own hit rate and its own budget, and CacheMetrics has one of each --
    // summing them would report a budget nobody set (the remote snapshot
    // asserts the number it was given) and a hit rate over two caches that
    // measure different things. What setCacheBudget below does guarantee is
    // that neither pool exceeds the configured budget; reporting both needs
    // a metrics shape that can hold two, which is a change of its own.
    return requireDataset()->cacheMetrics();
}

bool LocalDatasetSession::setCacheBudget(std::uint64_t bytes)
{
    // Both caches, each bounded by `bytes`: they hold different things (read
    // blocks and sampled grids) and neither should starve for the other, so
    // this is one budget applied twice rather than one budget split. Without
    // it AMREXPLORER_CACHE_SIZE_MB bounded only the block cache and a session
    // could hold the whole grid budget on top of it, unaccounted. It also
    // overrides an earlier setVolumeGridCacheBudget, which is the point: the
    // global setting is the one a user reaches for.
    const auto blocks = requireDataset()->setCacheBudget(bytes);
    const auto grids = m_volumeGrids.setBudget(bytes);
    return blocks && grids;
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
    // clearUnpinned collects the doomed entries in a vector, so it allocates
    // -- and the memory pressure that would make that throw is exactly when
    // a cache gets cleared. Letting it escape a noexcept close would
    // terminate the process during shutdown; dropping the grids is
    // best-effort, and the cache dies with the session either way.
    try {
        m_volumeGrids.clearUnpinned();
    } catch (...) {
    }
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
