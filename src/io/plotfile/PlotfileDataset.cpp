#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

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
    const std::filesystem::path& root, StopToken cancellation)
{
    if (std::filesystem::is_directory(root)
        && std::filesystem::is_regular_file(root / "Header")) {
        return discoverParticleSpecies(root, cancellation);
    }
    return {};
}

// Points evaluated per pass when a derived block is computed. The inputs are
// gathered into double columns a chunk at a time rather than a block at a time:
// a chunk of columns is a few kilobytes whatever the block's size, and the
// stored payload may be single precision, so it cannot be handed to the
// evaluator directly.
constexpr std::size_t derivedChunkPoints = 512;

} // namespace

PlotfileDataset::Fields PlotfileDataset::installFields(
    const PlotfileMetadataResult& source,
    std::span<const DerivedFieldDefinition> definitions)
{
    if (!source.metadata) {
        throw std::invalid_argument(
            "selected FAB dataset requires metadata and an id");
    }
    if (definitions.empty()) {
        return {source.metadata, {}, source.metadata->fields.size(), {}};
    }
    auto metadata = std::make_shared<DatasetMetadata>(*source.metadata);
    const auto storedCount = metadata->fields.size();
    auto installation = installDerivedFields(
        *metadata, definitions, DerivedFieldPolicy::Skip);
    return {std::move(metadata), std::move(installation.programs), storedCount,
        std::move(installation.skipped)};
}

PlotfileDataset::PlotfileDataset(
    std::filesystem::path plotfile, DatasetId id,
    std::uint64_t cacheBudgetBytes, StopToken cancellation,
    std::vector<DerivedFieldDefinition> derivedFields)
    : m_plotfile(sourceDataRoot(plotfile))
    , m_id(id)
    , m_metadataResult(readDatasetMetadata(plotfile, cancellation))
    , m_fields(installFields(m_metadataResult, derivedFields))
    , m_particleSpecies(std::filesystem::is_directory(plotfile)
            ? discoverParticleSpecies(m_plotfile, cancellation)
            : std::vector<ParticleSpeciesMetadata>{})
    , m_blockReader(m_plotfile, m_fields.metadata)
    , m_cache(cacheBudgetBytes)
{
    if (m_id.value == 0) {
        throw std::invalid_argument("PlotfileDataset id must be nonzero");
    }
}

PlotfileDataset::PlotfileDataset(std::filesystem::path root, DatasetId id,
    std::uint64_t cacheBudgetBytes, PlotfileMetadataResult metadata,
    StopToken cancellation, std::vector<DerivedFieldDefinition> derivedFields)
    : m_plotfile(std::move(root))
    , m_id(id)
    , m_metadataResult(std::move(metadata))
    // Rejects a missing metadata result, which used to be the constructor
    // body's job -- the field list has to be built before the block reader is
    // constructed, so the check has to happen there too.
    , m_fields(installFields(m_metadataResult, derivedFields))
    , m_particleSpecies(
          discoverParticlesForPlotfileRoot(m_plotfile, cancellation))
    , m_blockReader(m_plotfile, m_fields.metadata)
    , m_cache(cacheBudgetBytes)
{
    if (m_id.value == 0) {
        throw std::invalid_argument("selected FAB dataset requires metadata and an id");
    }
}

const DatasetMetadata& PlotfileDataset::metadata() const noexcept
{
    return *m_fields.metadata;
}

bool PlotfileDataset::isDerivedField(FieldId field) const noexcept
{
    return field.value >= m_fields.storedCount
        && static_cast<std::size_t>(field.value) < m_fields.metadata->fields.size();
}

std::size_t PlotfileDataset::storedFieldCount() const noexcept
{
    return m_fields.storedCount;
}

const std::vector<DerivedFieldSkip>& PlotfileDataset::skippedDerivedFields()
    const noexcept
{
    return m_fields.skipped;
}

const MetadataReadMetrics& PlotfileDataset::metadataReadMetrics() const noexcept
{
    return m_metadataResult.metrics;
}

const std::string& PlotfileDataset::fileVersion() const noexcept
{
    return m_metadataResult.fileVersion;
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
    StopToken cancellation, std::size_t maximumPoints) const
{
    return readParticleSample(
        m_plotfile, species, fraction, seed, cancellation, maximumPoints);
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

    // isDerivedField bounds the field id at both ends, so a request past the
    // field list falls through to the reader and is refused there, as it was
    // before derived fields existed.
    if (isDerivedField(request.field)) {
        // No IO lock: this reads no file of its own, and the recursive
        // requests for its inputs take the lock individually for whichever of
        // them actually miss the cache. Holding it across an evaluation would
        // stall every unrelated read on this dataset behind it -- and the
        // recursion below would deadlock on it. Two threads racing on the same
        // derived block therefore both evaluate it, and the loser's insert
        // finds the winner's entry and hands that back (insertAndPin), so the
        // race costs one duplicate evaluation and counts one extra cache hit.
        auto read = readDerivedBlock(request,
            m_fields.programs[static_cast<std::size_t>(request.field.value)
                - m_fields.storedCount],
            cancellation);
        auto handle = m_cache.insertAndPin(
            key, read.block, residentBytes(*read.block));
        return {std::move(handle), false, read.metrics};
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

BlockReadResult PlotfileDataset::readDerivedBlock(const BlockRequest& request,
    const DerivedFieldProgram& program, StopToken cancellation)
{
    if (request.componentCount != 1 || request.firstComponent != 0) {
        throw BlockReadError("a derived field has one component");
    }
    const auto& metadata = *m_fields.metadata;
    if (request.level < 0
        || static_cast<std::size_t>(request.level) >= metadata.levels.size()) {
        throw BlockReadError("requested level is unavailable");
    }
    const auto& level = metadata.levels[static_cast<std::size_t>(request.level)];
    if (request.gridIndex < 0
        || static_cast<std::size_t>(request.gridIndex) >= level.blocks.size()) {
        throw BlockReadError("requested grid is unavailable");
    }

    auto derived = std::make_shared<FabBlock>();
    // The grid's valid box, which is the box every reader of a block requires
    // it to cover -- deliberately not whatever box the inputs came back with.
    // A stored block's box is the valid box grown by the level's ghost width,
    // and a block computed from coordinates alone has no input to inherit a
    // box from at all, so taking the inputs' box would make the two disagree
    // and turn an expression mixing them into an error. The valid box is the
    // one thing they all cover.
    derived->box = level.blocks[static_cast<std::size_t>(request.gridIndex)].box;
    derived->field = request.field;
    derived->component = 0;
    const auto points = fabPointCount(derived->box, metadata.dimension);
    if (points > std::numeric_limits<std::size_t>::max()) {
        throw BlockReadError("derived block exceeds addressable memory");
    }
    const auto count = static_cast<std::size_t>(points);

    // Every input block, pinned for as long as the evaluation reads it, with
    // the arithmetic that turns a sample of the derived box into an offset
    // into that block's own (possibly ghost-grown) box. The recursion is
    // bounded by the definition order installDerivedFields enforces: an
    // expression reads only fields resolved before it.
    struct Source {
        BlockCache::Handle block;
        // Offset of the derived box's first sample in this block.
        std::size_t origin = 0;
        // Samples between consecutive j and k of this block.
        std::size_t rowStride = 1;
        std::size_t planeStride = 1;
    };
    const auto& inputs = program.inputs;
    std::vector<Source> sources;
    sources.reserve(inputs.size());
    // Where in `sources` each input's block is; unused for a coordinate.
    std::vector<std::size_t> sourceOf(inputs.size(), 0);
    BlockReadMetrics metrics;
    for (std::size_t input = 0; input < inputs.size(); ++input) {
        if (inputs[input].isCoordinate()) {
            continue;
        }
        auto inputRequest = request;
        inputRequest.field = inputs[input].field;
        auto access = requestBlock(inputRequest, cancellation);
        metrics.filesRead += access.io.filesRead;
        metrics.bytesRead += access.io.bytesRead;
        metrics.valuesRead += access.io.valuesRead;

        const auto& box = access.handle->box;
        // First, before any offset into this block is computed from its box:
        // fabPointCount is overflow-checked and is what the reader sized the
        // payload with, so a box this payload cannot hold -- or one whose
        // extents do not multiply -- is refused before the strides below
        // multiply them.
        if (access.handle->values.size()
            < fabPointCount(box, metadata.dimension)) {
            throw BlockReadError(
                "a derived field's input payload is smaller than its box");
        }
        Source source{std::move(access.handle), 0, 1, 1};
        std::size_t stride = 1;
        for (int axis = 0; axis < metadata.dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            if (derived->box.lower[i] < box.lower[i]
                || derived->box.upper[i] > box.upper[i]) {
                throw BlockReadError(
                    "a derived field's input does not cover its box");
            }
            source.origin += stride
                * static_cast<std::size_t>(
                    derived->box.lower[i] - box.lower[i]);
            if (axis == 1) {
                source.rowStride = stride;
            } else if (axis == 2) {
                source.planeStride = stride;
            }
            stride *= static_cast<std::size_t>(box.upper[i] - box.lower[i] + 1);
        }
        sourceOf[input] = sources.size();
        sources.push_back(std::move(source));
    }

    std::vector<double> values(count);
    // One column per symbol, a chunk long, and the spans handed to the
    // evaluator each pass (shorter on the last chunk).
    std::vector<std::vector<double>> columns(
        inputs.size(), std::vector<double>(derivedChunkPoints));
    std::vector<std::span<const double>> reads(inputs.size());
    // The sample index within the box, first axis fastest, which is the order
    // a FAB's values are laid out in (see valueOffset).
    std::array<int, 3> offset{0, 0, 0};
    std::array<int, 3> extent{1, 1, 1};
    for (int axis = 0; axis < metadata.dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        extent[i] = derived->box.upper[i] - derived->box.lower[i] + 1;
    }
    auto evaluator = program.expression.makeEvaluator();
    for (std::size_t start = 0; start < count; start += derivedChunkPoints) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto chunk = std::min(derivedChunkPoints, count - start);
        for (std::size_t point = 0; point < chunk; ++point) {
            for (std::size_t input = 0; input < inputs.size(); ++input) {
                if (inputs[input].isCoordinate()) {
                    const auto axis =
                        static_cast<std::size_t>(inputs[input].axis);
                    columns[input][point] = samplePosition(level,
                        inputs[input].axis,
                        derived->box.lower[axis] + offset[axis]);
                } else {
                    const auto& source = sources[sourceOf[input]];
                    columns[input][point] = source.block->values[source.origin
                        + static_cast<std::size_t>(offset[0])
                        + source.rowStride
                            * static_cast<std::size_t>(offset[1])
                        + source.planeStride
                            * static_cast<std::size_t>(offset[2])];
                }
            }
            // Advance the sample index with the walk rather than dividing it
            // out of the linear one per point.
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (++offset[axis] < extent[axis]) {
                    break;
                }
                offset[axis] = 0;
            }
        }
        for (std::size_t input = 0; input < inputs.size(); ++input) {
            reads[input] = std::span<const double>(columns[input].data(), chunk);
        }
        evaluator.evaluate(reads, std::span<double>(values.data() + start, chunk));
    }
    derived->values = FabValues{std::move(values)};
    return {std::shared_ptr<const FabBlock>(std::move(derived)), metrics};
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
