// The amrexplorer._amrexplorer extension: a thin, typed layer over the
// Qt-free analysis code. Everything a script sees is a native number -- no
// units are read, attached or converted here -- and every array handed to
// Python is a read-only NumPy view of memory a chunk owns, kept alive by the
// array itself, so it outlives the chunk object, the plan and the dataset.
// The ergonomic surface (open_plotfile, context management, region and dtype
// spellings) lives in the pure-Python package around this module.

#include <amrexplorer/analysis/Blocks.hpp>
#include <amrexplorer/analysis/Deposition.hpp>
#include <amrexplorer/analysis/LeafChunks.hpp>
#include <amrexplorer/analysis/Projection.hpp>
#include <amrexplorer/analysis/Surfaces.hpp>
#include <amrexplorer/analysis/Tiles.hpp>
#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/core/CacheConfig.hpp>
#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/ParticleReader.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/pipeline/VolumePipeline.hpp>
#include <amrexplorer/query/SliceQuery.hpp>
#include <amrexplorer/query/VolumeQuery.hpp>
#include <amrexplorer/render2d/Palette.hpp>
#include <amrexplorer/render2d/ScalarRenderer.hpp>
#include <amrexplorer/render3d/VolumeRaycaster.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/array.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace nb = nanobind;
using namespace nb::literals;

namespace amrvis::python {
namespace {

// Every dataset the module opens gets an id no other has had in this
// process, so an item planned on one can never be read through another --
// even one opened on the same path with the same definitions.
std::atomic<std::uint64_t> nextDatasetId{1};

// A cancellation flag a script can raise from another thread (or a signal
// handler) to stop a read, projection or slice in progress; the operation
// then raises ReadCancelled. One token may be shared by many operations, and
// once cancelled it stays cancelled.
struct CancelToken {
    StopSource source;
};

StopToken tokenOf(const CancelToken* cancel)
{
    return cancel ? cancel->source.get_token() : StopToken{};
}

// A built-in palette by its settings name ("viridis"), or a .pal file.
Palette paletteNamed(const std::string& name)
{
    for (int index = 0; index < static_cast<int>(BuiltinPalette::Count); ++index) {
        const auto palette = static_cast<BuiltinPalette>(index);
        if (builtinPaletteName(palette) == name) {
            return builtinPalette(palette);
        }
    }
    return Palette::load(name);
}

// 0xAARRGGBB pixels as an (height, width, 4) RGBA byte array, rows flipped
// when `flip` so row 0 is the top of the picture.
nb::ndarray<nb::numpy, std::uint8_t, nb::ndim<3>> rgbaArray(
    const std::vector<std::uint32_t>& pixels, int width, int height, bool flip)
{
    auto* bytes = new std::vector<std::uint8_t>(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int row = 0; row < height; ++row) {
        const auto sourceRow = flip ? height - 1 - row : row;
        for (int column = 0; column < width; ++column) {
            const auto argb = pixels[static_cast<std::size_t>(sourceRow) * static_cast<std::size_t>(width)
                + static_cast<std::size_t>(column)];
            auto* out = bytes->data()
                + (static_cast<std::size_t>(row) * static_cast<std::size_t>(width)
                      + static_cast<std::size_t>(column)) * 4;
            out[0] = static_cast<std::uint8_t>((argb >> 16U) & 0xFFU);
            out[1] = static_cast<std::uint8_t>((argb >> 8U) & 0xFFU);
            out[2] = static_cast<std::uint8_t>(argb & 0xFFU);
            out[3] = static_cast<std::uint8_t>((argb >> 24U) & 0xFFU);
        }
    }
    nb::capsule owner(bytes, [](void* pointer) noexcept {
        delete static_cast<std::vector<std::uint8_t>*>(pointer);
    });
    return nb::ndarray<nb::numpy, std::uint8_t, nb::ndim<3>>(bytes->data(),
        {static_cast<std::size_t>(height), static_cast<std::size_t>(width), 4},
        owner);
}

class DatasetClosed : public std::runtime_error {
public:
    DatasetClosed() : std::runtime_error("the dataset is closed") {}
};

template <typename Value>
nb::ndarray<nb::numpy, const Value, nb::ndim<1>> view(
    const std::shared_ptr<const void>& owner, const std::vector<Value>& values)
{
    // The capsule holds a reference to whatever owns `values`, so the array
    // keeps the memory alive however long NumPy keeps the array.
    auto* keep = new std::shared_ptr<const void>(owner);
    nb::capsule capsule(keep, [](void* pointer) noexcept {
        delete static_cast<std::shared_ptr<const void>*>(pointer);
    });
    return nb::ndarray<nb::numpy, const Value, nb::ndim<1>>(
        values.data(), {values.size()}, capsule);
}

nb::object fieldArray(
    const std::shared_ptr<const LeafChunk>& chunk, const AnalysisArray& values)
{
    return std::visit(
        [&](const auto& vector) -> nb::object {
            return nb::cast(view(chunk, vector));
        },
        values);
}

using Box = std::pair<std::vector<int>, std::vector<int>>;

Box boxTuple(const IntBox& box, int dimension)
{
    Box result;
    for (int axis = 0; axis < dimension; ++axis) {
        result.first.push_back(box.lower[static_cast<std::size_t>(axis)]);
        result.second.push_back(box.upper[static_cast<std::size_t>(axis)]);
    }
    return result;
}

std::vector<double> realVector(const Real3& value, int dimension)
{
    return {value.values.begin(), value.values.begin() + dimension};
}

std::string dtypeName(AnalysisValueType type)
{
    return type == AnalysisValueType::Float32 ? "float32" : "float64";
}

AnalysisValueType parseDtype(const std::string& name)
{
    if (name == "float32") {
        return AnalysisValueType::Float32;
    }
    if (name == "float64") {
        return AnalysisValueType::Float64;
    }
    throw AnalysisError("dtype must be float32 or float64, not " + name);
}

// The budget a dataset opens with when the script names none: the viewer's
// default, except that an unusable AMREXPLORER_CACHE_SIZE_MB is an error
// rather than a silent fall back.
std::uint64_t defaultCacheBudget()
{
    const auto configured = cacheSizeEnvironmentValue();
    if (!configured) {
        return defaultInitialCacheBudget;
    }
    if (const auto bytes = parseCacheSizeMegabytes(*configured)) {
        return *bytes;
    }
    throw AnalysisError(std::string(cacheSizeEnvironmentVariable) + "='"
        + std::string(*configured)
        + "' is not a positive whole number of megabytes");
}

// A particle plan: one species' grids that hold any particles, in level and
// then native grid order, and the attributes to read from each.
struct ParticlePlan {
    DatasetId dataset;
    std::shared_ptr<const ParticleSpeciesLayout> layout;
    std::vector<std::string> realNames;
    std::vector<int> realColumns;
    std::vector<std::string> intNames;
    std::vector<int> intComponents;
    // Indices into layout->grids.
    std::vector<std::size_t> grids;
    std::uint64_t totalParticles = 0;
};

struct ParticleItem {
    std::shared_ptr<const ParticlePlan> plan;
    std::size_t index = 0;

    const ParticleGridLayout& grid() const
    {
        return plan->layout->grids[plan->grids[index]];
    }
};

struct ParticleChunk {
    std::shared_ptr<const ParticlePlan> plan;
    std::size_t index = 0;
    ParticleGridData data;
};

class Dataset {
public:
    Dataset(const std::string& path,
        const std::vector<std::pair<std::string, std::string>>& derived,
        std::optional<std::uint64_t> cacheBytes)
    {
        std::vector<DerivedFieldDefinition> definitions;
        definitions.reserve(derived.size());
        for (const auto& [name, expression] : derived) {
            definitions.push_back({name, expression});
        }
        if (cacheBytes && *cacheBytes == 0) {
            throw AnalysisError("cache_bytes must be positive");
        }
        const auto budget = cacheBytes ? *cacheBytes : defaultCacheBudget();
        const DatasetId id{nextDatasetId.fetch_add(1)};
        nb::gil_scoped_release release;
        m_dataset = std::make_shared<PlotfileDataset>(path, id, budget,
            StopToken{}, std::move(definitions), DerivedFieldPolicy::Strict);
    }

    PlotfileDataset& dataset() const
    {
        if (!m_dataset) {
            throw DatasetClosed();
        }
        return *m_dataset;
    }

    void close() { m_dataset.reset(); }
    bool closed() const { return !m_dataset; }

    std::uint64_t id() const { return dataset().id().value; }
    const DatasetMetadata& metadata() const { return dataset().metadata(); }

    std::vector<std::string> fields() const
    {
        std::vector<std::string> names;
        for (const auto& field : metadata().fields) {
            names.push_back(field.name);
        }
        return names;
    }

    std::vector<std::string> storedFields() const
    {
        auto names = fields();
        names.resize(dataset().storedFieldCount());
        return names;
    }

    std::vector<std::string> derivedFields() const
    {
        auto names = fields();
        names.erase(names.begin(),
            names.begin()
                + static_cast<std::ptrdiff_t>(dataset().storedFieldCount()));
        return names;
    }

    std::string fieldCentering(const std::string& name) const
    {
        for (const auto& field : metadata().fields) {
            if (field.name != name) {
                continue;
            }
            switch (field.centering) {
            case Centering::Cell: return "cell";
            case Centering::Node: return "node";
            case Centering::FaceX: return "face_x";
            case Centering::FaceY: return "face_y";
            case Centering::FaceZ: return "face_z";
            case Centering::EdgeX: return "edge_x";
            case Centering::EdgeY: return "edge_y";
            case Centering::EdgeZ: return "edge_z";
            case Centering::Mixed: return "mixed";
            }
        }
        throw AnalysisError("unknown field '" + name + "'");
    }

    nb::dict geometry() const
    {
        const auto& meta = metadata();
        nb::dict result;
        result["dimension"] = meta.dimension;
        result["coordinate_system"] = meta.coordinateSystem == 0
            ? std::string("cartesian")
            : meta.coordinateSystem == 1 ? std::string("cylindrical")
            : meta.coordinateSystem == 2 ? std::string("spherical")
                                         : std::string("unknown");
        result["coordinate_system_code"] = meta.coordinateSystem;
        result["domain_lower"]
            = realVector(meta.physicalDomain.lower, meta.dimension);
        result["domain_upper"]
            = realVector(meta.physicalDomain.upper, meta.dimension);
        result["mapped_grid"] = meta.hasMappedGrid;
        result["physical_geometry"] = meta.hasPhysicalGeometry;
        return result;
    }

    nb::list levels() const
    {
        const auto& meta = metadata();
        nb::list result;
        for (std::size_t index = 0; index < meta.levels.size(); ++index) {
            const auto& level = meta.levels[index];
            nb::dict entry;
            entry["level"] = level.level;
            entry["step"] = level.step;
            entry["domain"] = boxTuple(level.domain, meta.dimension);
            entry["cell_size"] = realVector(level.cellSize, meta.dimension);
            entry["index_origin"]
                = realVector(level.indexOrigin, meta.dimension);
            entry["ghost_cells"] = std::vector<int>(
                level.ghostWidth.values.begin(),
                level.ghostWidth.values.begin() + meta.dimension);
            entry["num_grids"] = level.blocks.size();
            entry["dtype"] = dtypeName(
                storedValueType(dataset(), static_cast<int>(index)));
            result.append(entry);
        }
        return result;
    }

    std::uint64_t cacheBytes() const
    {
        return dataset().cacheMetrics().budgetBytes;
    }

    void setCacheBytes(std::uint64_t bytes)
    {
        if (bytes == 0) {
            throw AnalysisError("cache_bytes must be positive");
        }
        auto& data = dataset();
        const auto previous = data.cacheMetrics().budgetBytes;
        if (data.setCacheBudget(bytes)) {
            return;
        }
        // setCacheBudget has already stored the new budget on both pools;
        // put the old one back before saying so, so a failed call changes
        // nothing a later read could trip over.
        static_cast<void>(data.setCacheBudget(previous));
        const auto pinned = data.cacheMetrics().pinnedBytes;
        throw CacheBudgetExceeded("cannot lower the cache budget to "
            + std::to_string(bytes) + " bytes while "
            + std::to_string(pinned)
            + " bytes are pinned; the budget is unchanged");
    }

    nb::dict cacheMetrics() const
    {
        const auto metrics = dataset().cacheMetrics();
        nb::dict result;
        result["budget_bytes"] = metrics.budgetBytes;
        result["resident_bytes"] = metrics.residentBytes;
        result["pinned_bytes"] = metrics.pinnedBytes;
        result["hits"] = metrics.hits;
        result["misses"] = metrics.misses;
        result["evictions"] = metrics.evictions;
        return result;
    }

    std::shared_ptr<const LeafChunkPlan> plan(
        const std::vector<std::string>& fields,
        std::optional<std::pair<std::vector<double>, std::vector<double>>>
            region,
        std::optional<std::string> dtype) const
    {
        const auto& data = dataset();
        LeafChunkRequest request;
        request.fields = fields;
        if (region) {
            const auto dimension
                = static_cast<std::size_t>(data.metadata().dimension);
            if (region->first.size() != dimension
                || region->second.size() != dimension) {
                throw AnalysisError("region bounds must have one value per "
                                    "axis of this "
                    + std::to_string(dimension) + "-D plotfile");
            }
            RealBox box;
            for (std::size_t axis = 0; axis < dimension; ++axis) {
                box.lower[axis] = region->first[axis];
                box.upper[axis] = region->second[axis];
            }
            request.region = box;
        }
        if (dtype) {
            request.dtype = parseDtype(*dtype);
        }
        return std::make_shared<const LeafChunkPlan>(
            planLeafChunks(data, request));
    }

    std::shared_ptr<const LeafChunk> read(
        const LeafChunkItem& item, const CancelToken* cancel)
    {
        // Held across the read so close() from another thread cannot free
        // the dataset under it.
        auto keep = m_dataset;
        if (!keep) {
            throw DatasetClosed();
        }
        const auto token = tokenOf(cancel);
        nb::gil_scoped_release release;
        return readLeafChunk(*keep, item, token);
    }

    nb::list particleSpecies() const
    {
        nb::list result;
        for (const auto& species : dataset().particleSpecies()) {
            const auto layout
                = readParticleSpeciesLayout(dataset().dataRoot(), species.name);
            nb::dict entry;
            entry["name"] = species.name;
            entry["num_particles"] = species.particleCount;
            entry["dimension"] = species.dimension;
            entry["dtype"] = species.precision == ParticleRealPrecision::Single
                ? std::string("float32") : std::string("float64");
            entry["real_components"] = layout.realComponentNames;
            entry["int_components"] = layout.intComponentNames;
            result.append(entry);
        }
        return result;
    }

    std::shared_ptr<const ParticlePlan> planParticles(
        const std::string& species,
        std::optional<std::vector<std::string>> fields) const
    {
        const auto& data = dataset();
        const auto& known = data.particleSpecies();
        if (std::none_of(known.begin(), known.end(),
                [&](const ParticleSpeciesMetadata& entry) {
                    return entry.name == species;
                })) {
            std::string names;
            for (const auto& entry : known) {
                names += names.empty() ? "" : ", ";
                names += entry.name;
            }
            throw AnalysisError("unknown particle species '" + species
                + "'; the plotfile has: " + (names.empty() ? "none" : names));
        }
        auto plan = std::make_shared<ParticlePlan>();
        plan->dataset = data.id();
        plan->layout = std::make_shared<const ParticleSpeciesLayout>(
            readParticleSpeciesLayout(data.dataRoot(), species));
        const auto& layout = *plan->layout;
        const auto dimension = layout.metadata.dimension;
        // Positions are always read; they are what places a particle.
        static const char* const axes[] = {"x", "y", "z"};
        for (int axis = 0; axis < dimension; ++axis) {
            plan->realColumns.push_back(axis);
            plan->realNames.emplace_back(axes[axis]);
        }
        std::vector<std::string> wanted;
        if (fields) {
            wanted = *fields;
        } else {
            wanted = layout.realComponentNames;
            wanted.insert(wanted.end(), layout.intComponentNames.begin(),
                layout.intComponentNames.end());
        }
        for (const auto& name : wanted) {
            const auto& reals = layout.realComponentNames;
            const auto& ints = layout.intComponentNames;
            const auto real = std::find(reals.begin(), reals.end(), name);
            const auto integer = std::find(ints.begin(), ints.end(), name);
            const auto seen = [&](const std::vector<std::string>& list) {
                return std::find(list.begin(), list.end(), name) != list.end();
            };
            if (seen(plan->realNames) || seen(plan->intNames)) {
                if (name.size() == 1 && name[0] >= 'x' && name[0] <= 'z') {
                    continue;
                }
                throw AnalysisError("particle field '" + name
                    + "' is requested twice");
            }
            if (real != reals.end()) {
                plan->realColumns.push_back(dimension
                    + static_cast<int>(std::distance(reals.begin(), real)));
                plan->realNames.push_back(name);
            } else if (integer != ints.end()) {
                plan->intComponents.push_back(
                    static_cast<int>(std::distance(ints.begin(), integer)));
                plan->intNames.push_back(name);
            } else {
                std::string names;
                for (const auto& list : {reals, ints}) {
                    for (const auto& entry : list) {
                        names += names.empty() ? "" : ", ";
                        names += entry;
                    }
                }
                throw AnalysisError("unknown particle field '" + name
                    + "' for species '" + species + "'; it has: "
                    + (names.empty() ? "positions only" : names));
            }
        }
        for (std::size_t grid = 0; grid < layout.grids.size(); ++grid) {
            if (layout.grids[grid].count != 0) {
                plan->grids.push_back(grid);
                plan->totalParticles += layout.grids[grid].count;
            }
        }
        return plan;
    }

    std::shared_ptr<const ParticleChunk> readParticles(
        const ParticleItem& item, const CancelToken* cancel)
    {
        auto keep = m_dataset;
        if (!keep) {
            throw DatasetClosed();
        }
        if (item.plan->dataset != keep->id()) {
            throw AnalysisError("particle plan item "
                + std::to_string(item.index)
                + " was planned on a different dataset; plan again on this "
                  "one");
        }
        const auto token = tokenOf(cancel);
        nb::gil_scoped_release release;
        auto chunk = std::make_shared<ParticleChunk>();
        chunk->plan = item.plan;
        chunk->index = item.index;
        chunk->data = readParticleGrid(keep->dataRoot(), *item.plan->layout,
            item.plan->grids[item.index], item.plan->realColumns,
            item.plan->intComponents, token);
        return chunk;
    }

    std::shared_ptr<const ProjectionResult> project(const std::string& field,
        std::optional<std::string> weight, int axis,
        std::array<double, 4> rect, std::array<double, 2> axisBounds,
        std::array<int, 2> resolution, const CancelToken* cancel)
    {
        auto keep = m_dataset;
        if (!keep) {
            throw DatasetClosed();
        }
        ProjectionRequest request;
        request.field = field;
        request.weight = std::move(weight);
        request.axis = axis;
        request.rect = rect;
        request.axisBounds = axisBounds;
        request.resolution = resolution;
        const auto token = tokenOf(cancel);
        nb::gil_scoped_release release;
        return std::make_shared<const ProjectionResult>(
            projectLeafCells(*keep, request, token));
    }

    // A sampled slice through the viewer's query layer: each pixel center
    // reads the finest covering cell (nearest / piecewise constant) or
    // interpolates between cell centers (linear).
    std::shared_ptr<const ScalarPlane> slice(const std::string& field,
        int axis, double coordinate, std::array<double, 4> rect,
        std::array<int, 2> resolution, const std::string& sampling,
        std::optional<int> maximumLevel, const std::string& composition,
        const CancelToken* cancel)
    {
        auto keep = m_dataset;
        if (!keep) {
            throw DatasetClosed();
        }
        const auto& meta = keep->metadata();
        SliceRequest request;
        request.dataset = keep->id();
        const auto found = std::find_if(meta.fields.begin(), meta.fields.end(),
            [&](const FieldMetadata& entry) { return entry.name == field; });
        if (found == meta.fields.end()) {
            throw AnalysisError("unknown field '" + field + "'");
        }
        request.field = FieldId{static_cast<std::uint32_t>(
            std::distance(meta.fields.begin(), found))};
        if (meta.dimension == 3) {
            if (axis < 0 || axis > 2) {
                throw AnalysisError("slice axis must be 0, 1 or 2");
            }
            request.normalDirection = axis;
        } else if (meta.dimension == 2) {
            // A 2-D plotfile is its own plane; the normal names no axis it has.
            request.normalDirection = 0;
        } else {
            throw AnalysisError("slices need a 2-D or 3-D plotfile");
        }
        request.physicalPosition = coordinate;
        const auto axes = planeAxes(meta.dimension, request.normalDirection);
        request.visibleRegion = meta.physicalDomain;
        request.visibleRegion.lower[static_cast<std::size_t>(axes[0])] = rect[0];
        request.visibleRegion.lower[static_cast<std::size_t>(axes[1])] = rect[1];
        request.visibleRegion.upper[static_cast<std::size_t>(axes[0])] = rect[2];
        request.visibleRegion.upper[static_cast<std::size_t>(axes[1])] = rect[3];
        request.outputSize = resolution;
        request.maximumLevel = maximumLevel.value_or(meta.finestLevel);
        if (sampling == "nearest") {
            request.sampling = SamplingPolicy::Nearest;
        } else if (sampling == "piecewise") {
            request.sampling = SamplingPolicy::PiecewiseConstant;
        } else if (sampling == "linear") {
            request.sampling = SamplingPolicy::Linear;
        } else {
            throw AnalysisError("sampling must be nearest, piecewise or linear");
        }
        if (composition == "finest") {
            request.composition = CompositionPolicy::FinestAvailable;
        } else if (composition == "exact") {
            request.composition = CompositionPolicy::ExactLevel;
        } else {
            throw AnalysisError("composition must be finest or exact");
        }
        const auto errors = validateSliceRequest(request, meta.dimension);
        if (!errors.empty()) {
            throw AnalysisError("invalid slice: " + errors.front());
        }
        const auto token = tokenOf(cancel);
        nb::gil_scoped_release release;
        SliceQuery query(*keep);
        auto result = query.execute(request, token);
        return std::make_shared<const ScalarPlane>(std::move(result.plane));
    }

    std::shared_ptr<const CellAverageSliceResult> sliceCellAverage(
        const std::string& field, int axis, double coordinate,
        std::array<double, 4> rect, std::array<int, 2> resolution,
        const CancelToken* cancel)
    {
        auto keep = m_dataset;
        if (!keep) {
            throw DatasetClosed();
        }
        CellAverageSliceRequest request{field, axis, coordinate, rect, resolution};
        const auto token = tokenOf(cancel);
        nb::gil_scoped_release release;
        return std::make_shared<const CellAverageSliceResult>(
            amrvis::sliceCellAverage(*keep, request, token));
    }

    nb::list blocks(std::optional<int> level) const
    {
        const auto& data = dataset();
        const auto dimension = data.metadata().dimension;
        nb::list result;
        for (const auto& block : listBlocks(data, level)) {
            nb::dict entry;
            entry["level"] = block.level;
            entry["grid"] = block.grid;
            entry["box"] = boxTuple(block.box, dimension);
            entry["lower"] = realVector(block.bounds.lower, dimension);
            entry["upper"] = realVector(block.bounds.upper, dimension);
            entry["cells"] = block.cells;
            entry["covered_cells"] = block.coveredCells;
            entry["file"] = block.file;
            entry["offset"] = block.offset;
            result.append(entry);
        }
        return result;
    }

    nb::list blockStatistics(const std::vector<std::string>& fields,
        std::optional<int> level, std::optional<int> grid,
        const CancelToken* cancel)
    {
        auto keep = m_dataset;
        if (!keep) {
            throw DatasetClosed();
        }
        const auto dimension = keep->metadata().dimension;
        std::vector<BlockExtrema> extrema;
        {
            const auto token = tokenOf(cancel);
            nb::gil_scoped_release release;
            extrema = amrvis::blockStatistics(*keep, fields, level, grid, token);
        }
        nb::list result;
        for (const auto& block : extrema) {
            nb::dict entry;
            entry["level"] = block.level;
            entry["grid"] = block.grid;
            nb::dict minimum;
            nb::dict maximum;
            nb::dict boxes;
            nb::dict ghost;
            for (std::size_t field = 0; field < fields.size(); ++field) {
                const auto name = nb::str(fields[field].c_str());
                minimum[name] = block.minimum[field];
                maximum[name] = block.maximum[field];
                boxes[name] = boxTuple(block.boxes[field], dimension);
                ghost[name] = static_cast<bool>(block.includesGhostCells[field]);
            }
            entry["minimum"] = minimum;
            entry["maximum"] = maximum;
            entry["box"] = boxes;
            entry["includes_ghost_cells"] = ghost;
            result.append(entry);
        }
        return result;
    }

    std::shared_ptr<const TilePlan> planTiles(
        const std::vector<std::string>& fields,
        std::optional<std::pair<std::vector<double>, std::vector<double>>>
            region,
        std::optional<std::string> dtype, int ghost,
        std::array<bool, 3> periodic, const std::string& interpolation) const
    {
        const auto& data = dataset();
        TileRequest request;
        request.cells.fields = fields;
        if (region) {
            RealBox box;
            for (std::size_t axis = 0; axis < region->first.size() && axis < 3;
                 ++axis) {
                box.lower[axis] = region->first[axis];
                box.upper[axis] = region->second[axis];
            }
            request.cells.region = box;
        }
        if (dtype) {
            request.cells.dtype = parseDtype(*dtype);
        }
        request.ghost = ghost;
        request.periodic = periodic;
        if (interpolation == "linear") {
            request.interpolation = TileInterpolation::Linear;
        } else if (interpolation == "limited") {
            request.interpolation = TileInterpolation::Limited;
        } else if (interpolation == "constant") {
            request.interpolation = TileInterpolation::Constant;
        } else {
            throw AnalysisError(
                "interpolation must be linear, limited or constant");
        }
        return std::make_shared<const TilePlan>(amrvis::planTiles(data, request));
    }

    std::shared_ptr<const Tile> readTile(
        const TileItem& item, const CancelToken* cancel)
    {
        auto keep = m_dataset;
        if (!keep) {
            throw DatasetClosed();
        }
        const auto token = tokenOf(cancel);
        nb::gil_scoped_release release;
        return amrvis::readTile(*keep, item, token);
    }

    // The viewer's slice colouring of a sampled plane: palette slots over
    // [minimum, maximum], linear or logarithmic; invalid pixels grey.
    nb::object renderPlane(const ScalarPlane& plane, const std::string& palette,
        double minimum, double maximum, bool logarithmic) const
    {
        const auto colours = paletteNamed(palette);
        ScalarRenderSettings settings;
        settings.minimum = minimum;
        settings.maximum = maximum;
        settings.logarithmic = logarithmic;
        settings.palette = &colours;
        ImageBuffer image;
        try {
            image = renderScalarPlane(plane, settings);
        } catch (const std::invalid_argument& error) {
            throw AnalysisError(error.what());
        }
        // The plane's row 0 is the image's lower edge; flip for a picture.
        return nb::cast(rgbaArray(image.rgba, image.width, image.height, true));
    }

    nb::object renderVolume(const std::string& field,
        std::array<double, 3> lower, std::array<double, 3> upper,
        std::optional<int> maximumLevel, std::uint64_t maximumVoxels,
        double azimuth, double elevation, double zoom, std::array<int, 2> size,
        const std::string& palette, std::optional<std::pair<double, double>> range,
        bool logarithmic, double lowThreshold, double highThreshold,
        double maximumOpacity, int samplesPerVoxel, const CancelToken* cancel)
    {
        auto keep = m_dataset;
        if (!keep) {
            throw DatasetClosed();
        }
        const auto& meta = keep->metadata();
        if (!datasetSupportsVolumeRendering(meta)) {
            throw AnalysisError("volume rendering needs a 3-D plotfile with physical geometry");
        }
        const auto found = std::find_if(meta.fields.begin(), meta.fields.end(),
            [&](const FieldMetadata& entry) { return entry.name == field; });
        if (found == meta.fields.end()) {
            throw AnalysisError("unknown field '" + field + "'");
        }
        VolumeSampleRequest request;
        request.dataset = keep->id();
        request.field = FieldId{static_cast<std::uint32_t>(
            std::distance(meta.fields.begin(), found))};
        request.maximumLevel = maximumLevel.value_or(meta.finestLevel);
        request.region = RealBox{Real3{lower}, Real3{upper}};
        request.maximumVoxels = std::min<std::uint64_t>(
            std::max<std::uint64_t>(maximumVoxels, 1), maxVolumeVoxelBudget);
        const auto colours = paletteNamed(palette);
        const auto token = tokenOf(cancel);
        VolumeFrame frame;
        {
            nb::gil_scoped_release release;
            VolumeQuery query(*keep);
            const auto sampled = query.execute(request, token);
            RaycastSettings settings;
            settings.camera = orthoCameraFromAngles(azimuth, elevation, zoom);
            settings.domain = datasetSampleBounds(meta);
            settings.outputSize = size;
            if (range) {
                settings.range = {range->first, range->second, logarithmic};
            } else {
                const auto extrema = volumeGridRange(sampled.grid, logarithmic, token);
                if (!extrema) {
                    throw AnalysisError("the sampled volume has no finite values to colour");
                }
                settings.range = {extrema->first, extrema->second, logarithmic};
            }
            OpacityRamp ramp;
            ramp.lowThreshold = lowThreshold;
            ramp.highThreshold = highThreshold;
            ramp.maximumOpacity = maximumOpacity;
            settings.transfer = makeVolumeTransferFunction(colours, ramp);
            settings.samplesPerVoxel = samplesPerVoxel;
            try {
                frame = raycastVolume(sampled.grid, settings, token);
            } catch (const std::invalid_argument& error) {
                throw AnalysisError(error.what());
            }
        }
        return nb::cast(rgbaArray(frame.pixels, frame.width, frame.height, false));
    }

private:
    std::shared_ptr<PlotfileDataset> m_dataset;
};

struct TileItemRef {
    std::shared_ptr<const TilePlan> plan;
    std::size_t index = 0;

    const TileItem& item() const { return plan->items[index]; }
};

// A tile array as an (nx[, ny[, nz]]) Fortran-ordered read-only view.
template <typename Value>
nb::ndarray<nb::numpy, const Value> tileView(
    const std::shared_ptr<const Tile>& tile, const std::vector<Value>& values)
{
    auto* keep = new std::shared_ptr<const void>(tile);
    nb::capsule capsule(keep, [](void* pointer) noexcept {
        delete static_cast<std::shared_ptr<const void>*>(pointer);
    });
    std::array<std::size_t, 3> shape{};
    std::array<std::int64_t, 3> strides{};
    std::int64_t stride = 1;
    const auto dimension = static_cast<std::size_t>(tile->dimension);
    for (std::size_t axis = 0; axis < dimension; ++axis) {
        shape[axis] = tile->extent[axis];
        strides[axis] = stride;
        stride *= static_cast<std::int64_t>(tile->extent[axis]);
    }
    return nb::ndarray<nb::numpy, const Value>(
        values.data(), dimension, shape.data(), capsule, strides.data());
}

template <typename Value, typename Owner>
nb::ndarray<nb::numpy, const Value, nb::ndim<2>> planeView(
    const std::shared_ptr<const Owner>& owner, const std::vector<Value>& values,
    int width, int height)
{
    auto* keep = new std::shared_ptr<const void>(owner);
    nb::capsule capsule(keep, [](void* pointer) noexcept {
        delete static_cast<std::shared_ptr<const void>*>(pointer);
    });
    return nb::ndarray<nb::numpy, const Value, nb::ndim<2>>(values.data(),
        {static_cast<std::size_t>(height), static_cast<std::size_t>(width)},
        capsule);
}

// A (height, width) read-only view of one of a projection's images.
nb::ndarray<nb::numpy, const double, nb::ndim<2>> imageView(
    const std::shared_ptr<const ProjectionResult>& owner,
    const std::vector<double>& values)
{
    auto* keep = new std::shared_ptr<const void>(owner);
    nb::capsule capsule(keep, [](void* pointer) noexcept {
        delete static_cast<std::shared_ptr<const void>*>(pointer);
    });
    return nb::ndarray<nb::numpy, const double, nb::ndim<2>>(values.data(),
        {static_cast<std::size_t>(owner->height),
            static_cast<std::size_t>(owner->width)},
        capsule);
}

// A plan item, kept with the plan it came from so its selection outlives the
// Python plan object.
struct PlanItem {
    std::shared_ptr<const LeafChunkPlan> plan;
    std::size_t index = 0;

    const LeafChunkItem& item() const { return plan->items[index]; }
};

} // namespace
} // namespace amrvis::python

NB_MODULE(_amrexplorer, m)
{
    using namespace amrvis;
    using namespace amrvis::python;

    m.doc() = "Native core of the amrexplorer analysis package.";

    auto analysisError = nb::exception<AnalysisError>(
        m, "AnalysisError", PyExc_ValueError);
    nb::exception<DerivedFieldError>(m, "DerivedFieldError", analysisError);
    nb::exception<CacheBudgetExceeded>(
        m, "CacheBudgetExceeded", PyExc_MemoryError);
    auto readError = nb::exception<MetadataReadError>(
        m, "PlotfileReadError", PyExc_OSError);
    nb::exception<BlockReadError>(m, "BlockReadError", readError);
    nb::exception<ParticleReadError>(m, "ParticleReadError", readError);
    nb::exception<DatasetClosed>(m, "DatasetClosed", PyExc_RuntimeError);
    nb::exception<ReadCancelled>(m, "ReadCancelled", PyExc_RuntimeError);

    m.attr("DEFAULT_CACHE_BYTES") = defaultInitialCacheBudget;
    m.attr("CACHE_SIZE_ENVIRONMENT_VARIABLE")
        = std::string(cacheSizeEnvironmentVariable);
    m.attr("BLOCK_ALLOWANCE_BYTES") = leafChunkBlockAllowanceBytes;
    m.def("default_cache_bytes", &defaultCacheBudget,
        "The budget open_plotfile uses when cache_bytes is None.");

    nb::class_<CancelToken>(m, "CancelToken",
        "A cancellation flag for reads, projections and slices: call "
        "cancel() from another thread or a signal handler, and the "
        "operation in progress raises ReadCancelled.")
        .def(nb::init<>())
        .def("cancel",
            [](CancelToken& self) {
                static_cast<void>(self.source.request_stop());
            })
        .def_prop_ro("cancelled", [](const CancelToken& self) {
            return self.source.stop_requested();
        });

    nb::class_<Dataset>(m, "_Dataset")
        .def(nb::init<const std::string&,
                 const std::vector<std::pair<std::string, std::string>>&,
                 std::optional<std::uint64_t>>(),
            "path"_a, "derived"_a, "cache_bytes"_a.none())
        .def("close", &Dataset::close)
        .def_prop_ro("closed", &Dataset::closed)
        .def_prop_ro("id", &Dataset::id)
        .def_prop_ro("fields", &Dataset::fields)
        .def_prop_ro("stored_fields", &Dataset::storedFields)
        .def_prop_ro("derived_fields", &Dataset::derivedFields)
        .def("field_centering", &Dataset::fieldCentering)
        .def_prop_ro("time",
            [](const Dataset& self) { return self.metadata().time; })
        .def_prop_ro("dimension",
            [](const Dataset& self) { return self.metadata().dimension; })
        .def_prop_ro("finest_level",
            [](const Dataset& self) { return self.metadata().finestLevel; })
        .def_prop_ro("path",
            [](const Dataset& self) {
                return self.dataset().dataRoot().string();
            })
        .def_prop_ro("geometry", &Dataset::geometry)
        .def_prop_ro("levels", &Dataset::levels)
        .def_prop_ro("widest_dtype",
            [](const Dataset& self) {
                return dtypeName(widestStoredValueType(self.dataset()));
            })
        .def_prop_ro("cache_bytes", &Dataset::cacheBytes)
        .def("set_cache_bytes", &Dataset::setCacheBytes, "bytes"_a)
        .def("cache_metrics", &Dataset::cacheMetrics)
        .def("plan_leaf_chunks", &Dataset::plan, "fields"_a,
            "region"_a.none(), "dtype"_a.none())
        .def("read_leaf_chunk",
            [](Dataset& self, const PlanItem& item, const CancelToken* cancel) {
                return self.read(item.item(), cancel);
            },
            "item"_a, "cancel"_a.none() = nb::none())
        .def("slice", &Dataset::slice, "field"_a, "axis"_a, "coordinate"_a,
            "rect"_a, "resolution"_a, "sampling"_a, "max_level"_a.none(),
            "composition"_a, "cancel"_a.none() = nb::none())
        .def("render_plane",
            [](const Dataset& self, const ScalarPlane& plane, const std::string& palette,
                double minimum, double maximum, bool logarithmic) {
                return self.renderPlane(plane, palette, minimum, maximum, logarithmic);
            })
        .def("render_volume", &Dataset::renderVolume, "field"_a, "lower"_a,
            "upper"_a, "max_level"_a.none(), "max_voxels"_a, "azimuth"_a,
            "elevation"_a, "zoom"_a, "size"_a, "palette"_a, "range"_a.none(),
            "log"_a, "low_threshold"_a, "high_threshold"_a, "max_opacity"_a,
            "samples_per_voxel"_a, "cancel"_a.none() = nb::none())
        .def("slice_cell_average", &Dataset::sliceCellAverage, "field"_a,
            "axis"_a, "coordinate"_a, "rect"_a, "resolution"_a,
            "cancel"_a.none() = nb::none())
        .def("project", &Dataset::project, "field"_a, "weight"_a.none(),
            "axis"_a, "rect"_a, "axis_bounds"_a, "resolution"_a,
            "cancel"_a.none() = nb::none())
        .def("blocks", &Dataset::blocks, "level"_a.none())
        .def("block_statistics", &Dataset::blockStatistics, "fields"_a,
            "level"_a.none(), "grid"_a.none(), "cancel"_a.none() = nb::none())
        .def("plan_tiles", &Dataset::planTiles, "fields"_a, "region"_a.none(),
            "dtype"_a.none(), "ghost"_a, "periodic"_a, "interpolation"_a)
        .def("read_tile",
            [](Dataset& self, const TileItemRef& item, const CancelToken* cancel) {
                return self.readTile(item.item(), cancel);
            },
            "item"_a, "cancel"_a.none() = nb::none())
        .def("particle_species", &Dataset::particleSpecies)
        .def("plan_particle_chunks", &Dataset::planParticles, "species"_a,
            "fields"_a.none())
        .def("read_particle_chunk", &Dataset::readParticles, "item"_a,
            "cancel"_a.none() = nb::none());

    nb::class_<ProjectionResult>(m, "_Projection")
        .def_prop_ro("pixel_area",
            [](const ProjectionResult& self) { return self.pixelArea; })
        .def_prop_ro("path_length",
            [](const ProjectionResult& self) { return self.pathLength; })
        .def_prop_ro("cells",
            [](const ProjectionResult& self) { return self.cells; })
        .def_prop_ro("direct_integral",
            [](const ProjectionResult& self) { return self.directIntegral; })
        .def_prop_ro("weighted",
            [](const ProjectionResult& self) {
                return !self.weightImage.empty();
            })
        .def("_image",
            [](std::shared_ptr<const ProjectionResult> self) {
                return imageView(self, self->image);
            })
        .def("_weight_image",
            [](std::shared_ptr<const ProjectionResult> self) {
                return imageView(self, self->weightImage);
            })
        .def("_coverage", [](std::shared_ptr<const ProjectionResult> self) {
            return imageView(self, self->coverage);
        });

    nb::class_<ScalarPlane>(m, "_SampledSlice")
        .def("_values",
            [](std::shared_ptr<const ScalarPlane> self) {
                return planeView(self, self->values, self->width, self->height);
            })
        .def("_valid",
            [](std::shared_ptr<const ScalarPlane> self) {
                return planeView(self, self->valid, self->width, self->height);
            })
        .def("_source_level", [](std::shared_ptr<const ScalarPlane> self) {
            return planeView(
                self, self->sourceLevel, self->width, self->height);
        });

    nb::class_<CellAverageSliceResult>(m, "_CellAverageSlice")
        .def_prop_ro("pixel_area",
            [](const CellAverageSliceResult& self) { return self.pixelArea; })
        .def("_image",
            [](std::shared_ptr<const CellAverageSliceResult> self) {
                return planeView(self, self->image, self->width, self->height);
            })
        .def("_coverage",
            [](std::shared_ptr<const CellAverageSliceResult> self) {
                return planeView(
                    self, self->coverage, self->width, self->height);
            })
        .def("_finest_level",
            [](std::shared_ptr<const CellAverageSliceResult> self) {
                return planeView(
                    self, self->finestLevel, self->width, self->height);
            });

    m.def("_sphere_area_in_cells",
        [](const LeafChunk& chunk, std::array<double, 3> center, double radius) {
            auto section = std::make_shared<const SphereSection>(
                sphereAreaInCells(chunk, Real3{center}, radius));
            nb::dict result;
            result["area"] = view(section, section->area);
            result["normal_x"] = view(section, section->normalX);
            result["normal_y"] = view(section, section->normalY);
            result["normal_z"] = view(section, section->normalZ);
            return result;
        },
        "chunk"_a, "center"_a, "radius"_a);
    m.def("_cylinder_area_in_cells",
        [](const LeafChunk& chunk, std::array<double, 3> center, double radius,
            double halfHeight) {
            auto section = std::make_shared<const CylinderSection>(
                cylinderAreaInCells(chunk, Real3{center}, radius, halfHeight));
            nb::dict result;
            result["wall_area"] = view(section, section->wallArea);
            result["wall_normal_x"] = view(section, section->wallNormalX);
            result["wall_normal_y"] = view(section, section->wallNormalY);
            result["upper_cap_area"] = view(section, section->upperCapArea);
            result["lower_cap_area"] = view(section, section->lowerCapArea);
            result["cap_moment_x"] = view(section, section->capMomentX);
            result["cap_moment_y"] = view(section, section->capMomentY);
            return result;
        },
        "chunk"_a, "center"_a, "radius"_a, "half_height"_a);
    m.def("_disk_rectangle_moments",
        [](double radius, double x0, double x1, double y0, double y1) {
            const auto moments = diskRectangleMoments(radius, x0, x1, y0, y1);
            return std::make_tuple(moments.area, moments.x, moments.y);
        });
    m.def("_surface_region_padding", [](std::array<double, 3> cellSize) {
        return surfaceRegionPadding(Real3{cellSize});
    });

    nb::class_<TilePlan>(m, "_TilePlan")
        .def("__len__", [](const TilePlan& self) { return self.items.size(); })
        .def_prop_ro("fields",
            [](const TilePlan& self) { return self.cells->fieldNames; })
        .def_prop_ro("dtype",
            [](const TilePlan& self) { return dtypeName(self.cells->dtype); })
        .def_prop_ro("ghost",
            [](const TilePlan& self) { return self.selection->ghost; })
        .def_prop_ro("dataset_id",
            [](const TilePlan& self) { return self.cells->dataset.value; });

    m.def("_tile_item",
        [](std::shared_ptr<const TilePlan> plan, std::int64_t index) {
            const auto size = static_cast<std::int64_t>(plan->items.size());
            if (index < 0) {
                index += size;
            }
            if (index < 0 || index >= size) {
                throw nb::index_error("plan item index out of range");
            }
            return TileItemRef{std::move(plan), static_cast<std::size_t>(index)};
        });

    nb::class_<TileItemRef>(m, "_TileItem")
        .def_prop_ro("index",
            [](const TileItemRef& self) { return self.item().cells.index; })
        .def_prop_ro("level",
            [](const TileItemRef& self) { return self.item().cells.level; })
        .def_prop_ro("grid",
            [](const TileItemRef& self) { return self.item().cells.grid; })
        .def_prop_ro("num_cells",
            [](const TileItemRef& self) {
                return self.item().cells.selectedCells;
            })
        .def_prop_ro("estimated_bytes",
            [](const TileItemRef& self) {
                return self.item().estimatedPinnedBytes;
            })
        .def_prop_ro("touched",
            [](const TileItemRef& self) {
                std::vector<std::pair<int, int>> result;
                for (const auto& entry : self.item().touched) {
                    result.emplace_back(entry[0], entry[1]);
                }
                return result;
            })
        .def("_grown_box", [](const TileItemRef& self, int dimension) {
            return boxTuple(self.item().grownBox, dimension);
        });

    nb::class_<Tile>(m, "_Tile")
        .def_prop_ro("index",
            [](const Tile& self) { return self.item.cells.index; })
        .def_prop_ro("level",
            [](const Tile& self) { return self.item.cells.level; })
        .def_prop_ro("grid",
            [](const Tile& self) { return self.item.cells.grid; })
        .def_prop_ro("dimension", [](const Tile& self) { return self.dimension; })
        .def_prop_ro("ghost",
            [](const Tile& self) { return self.item.selection->ghost; })
        .def_prop_ro("fields",
            [](const Tile& self) { return self.item.cells.selection->fieldNames; })
        .def_prop_ro("cell_size",
            [](const Tile& self) {
                return realVector(self.cellSize, self.dimension);
            })
        .def_prop_ro("index_origin",
            [](const Tile& self) {
                return realVector(self.indexOrigin, self.dimension);
            })
        .def_prop_ro("box",
            [](const Tile& self) {
                return boxTuple(self.item.cells.box, self.dimension);
            })
        .def_prop_ro("grown_box",
            [](const Tile& self) {
                return boxTuple(self.item.grownBox, self.dimension);
            })
        .def("_field",
            [](std::shared_ptr<const Tile> self, const std::string& name) -> nb::object {
                const auto& names = self->item.cells.selection->fieldNames;
                const auto found = std::find(names.begin(), names.end(), name);
                if (found == names.end()) {
                    throw nb::key_error(name.c_str());
                }
                return std::visit(
                    [&](const auto& values) -> nb::object {
                        return nb::cast(tileView(self, values));
                    },
                    self->fields[static_cast<std::size_t>(
                        std::distance(names.begin(), found))]);
            })
        .def("_valid",
            [](std::shared_ptr<const Tile> self) {
                return tileView(self, self->valid);
            })
        .def("_source_level",
            [](std::shared_ptr<const Tile> self) {
                return tileView(self, self->sourceLevel);
            })
        .def("_leaf_mask", [](std::shared_ptr<const Tile> self) {
            return tileView(self, self->leafMask);
        });

    nb::class_<ParticleDeposit>(m, "_Deposit")
        .def("__init__",
            [](ParticleDeposit* self, const Dataset& dataset, int axis,
                std::array<double, 4> rect, std::array<double, 2> bounds,
                std::array<int, 2> resolution, std::array<bool, 3> periodic) {
                new (self) ParticleDeposit(dataset.metadata(),
                    DepositionRequest{axis, rect, bounds, resolution, periodic});
            })
        .def("add",
            [](ParticleDeposit& self, const ParticleChunk& chunk,
                std::optional<std::string> weight,
                std::optional<nb::ndarray<const bool, nb::ndim<1>, nb::c_contig>>
                    mask) {
                const auto& data = chunk.data;
                const auto count = data.ids.size();
                const auto dimension = chunk.plan->layout->metadata.dimension;
                if (dimension != 3) {
                    throw AnalysisError("particle deposition needs 3-D particles");
                }
                const auto column = [&](std::size_t index) {
                    std::vector<double> values(count);
                    if (data.precision == ParticleRealPrecision::Single) {
                        const auto& source = data.singleReals.at(index);
                        std::copy(source.begin(), source.end(), values.begin());
                    } else {
                        values = data.doubleReals.at(index);
                    }
                    return values;
                };
                const auto x = column(0);
                const auto y = column(1);
                const auto z = column(2);
                std::vector<double> weights;
                if (weight) {
                    const auto& reals = chunk.plan->realNames;
                    const auto found = std::find(reals.begin(), reals.end(), *weight);
                    if (found == reals.end()
                        || found - reals.begin() < dimension) {
                        throw AnalysisError("weight '" + *weight
                            + "' is not a real attribute the plan reads");
                    }
                    weights = column(static_cast<std::size_t>(found - reals.begin()));
                }
                std::vector<std::uint8_t> selected;
                if (mask) {
                    if (mask->shape(0) != count) {
                        throw AnalysisError("mask length " + std::to_string(mask->shape(0))
                            + " does not match the chunk's "
                            + std::to_string(count) + " particles");
                    }
                    selected.resize(count);
                    for (std::size_t p = 0; p < count; ++p) {
                        selected[p] = mask->data()[p] ? 1 : 0;
                    }
                }
                nb::gil_scoped_release release;
                self.add(data.level, x, y, z, weight ? weights.data() : nullptr,
                    mask ? selected.data() : nullptr);
            },
            "chunk"_a, "weight"_a.none(), "mask"_a.none())
        .def_prop_ro("outside_weight", &ParticleDeposit::outsideWeight)
        .def_prop_ro("selected_weight", &ParticleDeposit::selectedWeight)
        .def_prop_ro("selected_particles", &ParticleDeposit::selectedParticles)
        .def_prop_ro("pixel_area", &ParticleDeposit::pixelArea)
        .def("_image", [](const ParticleDeposit& self) {
            const auto& image = self.image();
            nb::ndarray<nb::numpy, const double, nb::ndim<2>> view(image.data(),
                {static_cast<std::size_t>(self.height()),
                    static_cast<std::size_t>(self.width())});
            return nb::cast(view).attr("copy")();
        });

    nb::class_<ParticlePlan>(m, "_ParticlePlan")
        .def("__len__",
            [](const ParticlePlan& self) { return self.grids.size(); })
        .def_prop_ro("species",
            [](const ParticlePlan& self) { return self.layout->metadata.name; })
        .def_prop_ro("total_particles",
            [](const ParticlePlan& self) { return self.totalParticles; })
        .def_prop_ro("real_fields",
            [](const ParticlePlan& self) { return self.realNames; })
        .def_prop_ro("int_fields",
            [](const ParticlePlan& self) { return self.intNames; })
        .def_prop_ro("dimension",
            [](const ParticlePlan& self) {
                return self.layout->metadata.dimension;
            })
        .def_prop_ro("dataset_id",
            [](const ParticlePlan& self) { return self.dataset.value; });

    m.def("_particle_item",
        [](std::shared_ptr<const ParticlePlan> plan, std::int64_t index) {
            const auto size = static_cast<std::int64_t>(plan->grids.size());
            if (index < 0) {
                index += size;
            }
            if (index < 0 || index >= size) {
                throw nb::index_error("plan item index out of range");
            }
            return ParticleItem{
                std::move(plan), static_cast<std::size_t>(index)};
        });

    nb::class_<ParticleItem>(m, "_ParticleItem")
        .def_prop_ro("index",
            [](const ParticleItem& self) { return self.index; })
        .def_prop_ro("level",
            [](const ParticleItem& self) { return self.grid().level; })
        .def_prop_ro("grid",
            [](const ParticleItem& self) { return self.grid().grid; })
        .def_prop_ro("num_particles",
            [](const ParticleItem& self) { return self.grid().count; })
        .def_prop_ro("dataset_id", [](const ParticleItem& self) {
            return self.plan->dataset.value;
        });

    nb::class_<ParticleChunk>(m, "_ParticleChunk")
        .def("__len__",
            [](const ParticleChunk& self) { return self.data.ids.size(); })
        .def_prop_ro("index",
            [](const ParticleChunk& self) { return self.index; })
        .def_prop_ro("level",
            [](const ParticleChunk& self) { return self.data.level; })
        .def_prop_ro("grid",
            [](const ParticleChunk& self) { return self.data.grid; })
        .def_prop_ro("species",
            [](const ParticleChunk& self) {
                return self.plan->layout->metadata.name;
            })
        .def_prop_ro("dimension",
            [](const ParticleChunk& self) {
                return self.plan->layout->metadata.dimension;
            })
        .def_prop_ro("real_fields",
            [](const ParticleChunk& self) { return self.plan->realNames; })
        .def_prop_ro("int_fields",
            [](const ParticleChunk& self) { return self.plan->intNames; })
        .def("_ids",
            [](std::shared_ptr<const ParticleChunk> self) {
                return view(self, self->data.ids);
            })
        .def("_cpus",
            [](std::shared_ptr<const ParticleChunk> self) {
                return view(self, self->data.cpus);
            })
        .def("_real",
            [](std::shared_ptr<const ParticleChunk> self,
                std::size_t column) -> nb::object {
                if (self->data.precision == ParticleRealPrecision::Single) {
                    return nb::cast(
                        view(self, self->data.singleReals.at(column)));
                }
                return nb::cast(view(self, self->data.doubleReals.at(column)));
            })
        .def("_int", [](std::shared_ptr<const ParticleChunk> self,
                         std::size_t column) {
            return view(self, self->data.ints.at(column));
        });

    nb::class_<LeafChunkPlan>(m, "_LeafChunkPlan")
        .def("__len__",
            [](const LeafChunkPlan& self) { return self.items.size(); })
        .def_prop_ro("total_cells",
            [](const LeafChunkPlan& self) { return self.totalCells; })
        .def_prop_ro("fields",
            [](const LeafChunkPlan& self) {
                return self.selection->fieldNames;
            })
        .def_prop_ro("dtype",
            [](const LeafChunkPlan& self) {
                return dtypeName(self.selection->dtype);
            })
        .def_prop_ro("dataset_id",
            [](const LeafChunkPlan& self) {
                return self.selection->dataset.value;
            });

    m.def("_plan_item",
        [](std::shared_ptr<const LeafChunkPlan> plan, std::int64_t index) {
            const auto size = static_cast<std::int64_t>(plan->items.size());
            if (index < 0) {
                index += size;
            }
            if (index < 0 || index >= size) {
                throw nb::index_error("plan item index out of range");
            }
            return PlanItem{std::move(plan), static_cast<std::size_t>(index)};
        });

    nb::class_<PlanItem>(m, "_PlanItem")
        .def_prop_ro("index",
            [](const PlanItem& self) { return self.item().index; })
        .def_prop_ro("level",
            [](const PlanItem& self) { return self.item().level; })
        .def_prop_ro("grid",
            [](const PlanItem& self) { return self.item().grid; })
        .def_prop_ro("num_cells",
            [](const PlanItem& self) { return self.item().selectedCells; })
        .def_prop_ro("estimated_bytes",
            [](const PlanItem& self) {
                return self.item().estimatedPinnedBytes;
            })
        .def_prop_ro("dataset_id",
            [](const PlanItem& self) {
                return self.item().selection->dataset.value;
            })
        .def("_box",
            [](const PlanItem& self, int dimension) {
                return boxTuple(self.item().box, dimension);
            })
        .def("_selected_box", [](const PlanItem& self, int dimension) {
            return boxTuple(self.item().selectedBox, dimension);
        });

    nb::class_<LeafChunk>(m, "_LeafChunk")
        .def("__len__", &LeafChunk::size)
        .def_prop_ro("index",
            [](const LeafChunk& self) { return self.item().index; })
        .def_prop_ro("level",
            [](const LeafChunk& self) { return self.item().level; })
        .def_prop_ro("grid",
            [](const LeafChunk& self) { return self.item().grid; })
        .def_prop_ro("dimension", &LeafChunk::dimension)
        .def_prop_ro("fields", &LeafChunk::fieldNames)
        .def_prop_ro("dtype",
            [](const LeafChunk& self) { return dtypeName(self.dtype()); })
        .def_prop_ro("cell_size",
            [](const LeafChunk& self) {
                return realVector(self.cellSize(), self.dimension());
            })
        .def_prop_ro("box",
            [](const LeafChunk& self) {
                return boxTuple(self.item().box, self.dimension());
            })
        .def("_field",
            [](std::shared_ptr<const LeafChunk> self, const std::string& name) {
                return fieldArray(self, self->field(name));
            })
        .def("_index",
            [](std::shared_ptr<const LeafChunk> self, int axis) {
                return view(self, self->index(axis));
            })
        .def("_center",
            [](std::shared_ptr<const LeafChunk> self, int axis) {
                return view(self, self->center(axis));
            })
        .def("_width",
            [](std::shared_ptr<const LeafChunk> self, int axis) {
                return view(self, self->width(axis));
            })
        .def("_cell_volume", [](std::shared_ptr<const LeafChunk> self) {
            return view(self, self->cellVolume());
        });

}
