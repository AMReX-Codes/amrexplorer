// The amrexplorer._amrexplorer extension: a thin, typed layer over the
// Qt-free analysis code. Everything a script sees is a native number -- no
// units are read, attached or converted here -- and every array handed to
// Python is a read-only NumPy view of memory a chunk owns, kept alive by the
// array itself, so it outlives the chunk object, the plan and the dataset.
// The ergonomic surface (open_plotfile, context management, region and dtype
// spellings) lives in the pure-Python package around this module.

#include <amrexplorer/analysis/LeafChunks.hpp>
#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/core/CacheConfig.hpp>
#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/array.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
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

// A cancellation flag a script can raise from another thread or a Python
// signal handler to stop an open, a plan or a read in progress; the
// operation then raises ReadCancelled. Handlers run while one is in progress
// because the main thread polls for signals (runInterruptibly). One token may
// be shared by many operations, and once cancelled it stays cancelled.
struct CancelToken {
    StopSource source;
};

StopToken tokenOf(const CancelToken* cancel)
{
    return cancel ? cancel->source.get_token() : StopToken{};
}

// How often a blocking call on the main thread hands control back to
// Python's signal handlers. A call that finishes sooner returns at once.
constexpr std::chrono::milliseconds signalPollInterval{20};

// Whether this is the interpreter's main thread, the only one Python runs
// signal handlers on. Asked each time, not remembered from import: a child
// forked from another thread has that thread as its main thread.
bool onMainThread()
{
    const auto ident = nb::module_::import_("threading")
                           .attr("main_thread")()
                           .attr("ident");
    return nb::cast<unsigned long>(ident) == PyThread_get_thread_ident();
}

// Runs work(token) with the GIL released, so other Python threads run
// meanwhile, and returns its result. Called with the GIL held.
//
// On the main thread the work runs on a thread of its own while this one
// takes the GIL back every poll interval to run Python's signal handlers:
// Python runs them only there, between bytecodes, so work that kept the main
// thread in C++ for its whole length could be stopped neither by Ctrl-C nor
// by a handler that calls CancelToken.cancel(). The caller's token is polled
// too and forwarded to the work's own. A thread per call, not a long-lived
// one: a forked child has none of its parent's threads, and a handler that
// itself reads must not queue behind the call it interrupted.
//
// Elsewhere no handler can run, so the work runs inline on the caller's
// token: no thread, and no waking for the GIL.
template <typename Work>
std::invoke_result_t<Work&, StopToken> runInterruptibly(
    Work&& work, const CancelToken* cancel)
{
    const auto requested = tokenOf(cancel);
    if (!onMainThread()) {
        nb::gil_scoped_release release;
        return work(requested);
    }
    StopSource stop;
    if (requested.stop_requested()) {
        static_cast<void>(stop.request_stop());
    }
    // By reference: this function returns only after the work has.
    auto future = std::async(std::launch::async,
        [&work, token = stop.get_token()] { return work(token); });
    for (;;) {
        {
            nb::gil_scoped_release release;
            if (future.wait_for(signalPollInterval)
                == std::future_status::ready) {
                break;
            }
        }
        if (requested.stop_requested()) {
            static_cast<void>(stop.request_stop());
        }
        if (PyErr_CheckSignals() != 0) {
            // A handler raised (KeyboardInterrupt by default): stop the
            // work, wait for it to let go of what it borrowed, and raise
            // what the handler raised.
            static_cast<void>(stop.request_stop());
            {
                nb::gil_scoped_release release;
                future.wait();
            }
            throw nb::python_error();
        }
    }
    return future.get();
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

class Dataset {
public:
    Dataset(const std::filesystem::path& path,
        const std::vector<std::pair<std::string, std::string>>& derived,
        std::optional<std::uint64_t> cacheBytes, const CancelToken* cancel)
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
        // Reading the metadata can take minutes on a large plotfile over a
        // slow filesystem, so it is interruptible like a read.
        m_dataset = runInterruptibly(
            [&](StopToken token) {
                return std::make_shared<PlotfileDataset>(path, id, budget,
                    token, std::move(definitions),
                    DerivedFieldPolicy::Strict);
            },
            cancel);
    }

    // The dataset, for a caller that holds the GIL throughout.
    PlotfileDataset& dataset() const { return *held(); }

    // The dataset, kept alive by the caller across a call that releases the
    // GIL, so close() from another thread cannot free it underneath.
    std::shared_ptr<PlotfileDataset> held() const
    {
        if (!m_dataset) {
            throw DatasetClosed();
        }
        return m_dataset;
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
        auto keep = held();
        const auto& meta = keep->metadata();
        std::vector<std::optional<AnalysisValueType>> types;
        {
            nb::gil_scoped_release release;
            types = levelTypes(*keep);
        }
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
            entry["dtype"] = types[index]
                ? std::optional<std::string>(dtypeName(*types[index]))
                : std::nullopt;
            result.append(entry);
        }
        return result;
    }

    // Each level's stored dtype, or nullopt when it cannot be determined
    // without a FAB that is missing or unreadable (a version-1 header records
    // no precision, so the first FAB's header is read, once per dataset).
    // Metadata stays inspectable on a partially copied plotfile; planning,
    // which needs the dtype, raises there instead.
    static std::vector<std::optional<AnalysisValueType>> levelTypes(
        const PlotfileDataset& data)
    {
        std::vector<std::optional<AnalysisValueType>> types(
            data.metadata().levels.size());
        for (std::size_t level = 0; level < types.size(); ++level) {
            try {
                types[level] = storedValueType(data, static_cast<int>(level));
            } catch (const AnalysisError&) {
            }
        }
        return types;
    }

    std::optional<std::string> widestDtype() const
    {
        auto keep = held();
        nb::gil_scoped_release release;
        try {
            return dtypeName(widestStoredValueType(*keep));
        } catch (const AnalysisError&) {
            return std::nullopt;
        }
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
        // trySetCacheBudget refuses without storing or evicting anything, so
        // a failed call neither drops the warm cache nor exposes the smaller
        // budget to a read on another thread.
        // The pinned bytes the refusal saw: read again, a concurrent read
        // could have released them, and the message would contradict itself.
        std::uint64_t pinned = 0;
        if (data.trySetCacheBudget(bytes, &pinned)) {
            return;
        }
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
        std::optional<std::string> dtype, const CancelToken* cancel) const
    {
        auto keep = held();
        const auto& data = *keep;
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
        // Planning walks every coarse/fine grid pair and may read FAB
        // headers; other Python threads (parallel workers replanning) run
        // meanwhile.
        return runInterruptibly(
            [&](StopToken token) {
                return std::make_shared<const LeafChunkPlan>(
                    planLeafChunks(data, request, token));
            },
            cancel);
    }

    std::shared_ptr<const LeafChunk> read(
        const LeafChunkItem& item, const CancelToken* cancel)
    {
        auto keep = held();
        return runInterruptibly(
            [&](StopToken token) { return readLeafChunk(*keep, item, token); },
            cancel);
    }

private:
    std::shared_ptr<PlotfileDataset> m_dataset;
};

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
    nb::exception<DatasetClosed>(m, "DatasetClosed", PyExc_RuntimeError);
    nb::exception<ReadCancelled>(m, "ReadCancelled", PyExc_RuntimeError);

    m.attr("DEFAULT_CACHE_BYTES") = defaultInitialCacheBudget;
    m.attr("CACHE_SIZE_ENVIRONMENT_VARIABLE")
        = std::string(cacheSizeEnvironmentVariable);
    m.attr("BLOCK_ALLOWANCE_BYTES") = leafChunkBlockAllowanceBytes;
    m.def("default_cache_bytes", &defaultCacheBudget,
        "The budget open_plotfile uses when cache_bytes is None.");

    nb::class_<CancelToken>(m, "CancelToken",
        "A cancellation flag for opens, plans and reads: call "
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
        .def(nb::init<const std::filesystem::path&,
                 const std::vector<std::pair<std::string, std::string>>&,
                 std::optional<std::uint64_t>, const CancelToken*>(),
            "path"_a, "derived"_a, "cache_bytes"_a.none(),
            "cancel"_a.none() = nb::none())
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
                return self.dataset().dataRoot();
            })
        .def_prop_ro("geometry", &Dataset::geometry)
        .def_prop_ro("levels", &Dataset::levels)
        .def_prop_ro("widest_dtype", &Dataset::widestDtype)
        .def_prop_ro("cache_bytes", &Dataset::cacheBytes)
        .def("set_cache_bytes", &Dataset::setCacheBytes, "bytes"_a)
        .def("cache_metrics", &Dataset::cacheMetrics)
        .def("plan_leaf_chunks", &Dataset::plan, "fields"_a,
            "region"_a.none(), "dtype"_a.none(),
            "cancel"_a.none() = nb::none())
        .def("read_leaf_chunk",
            [](Dataset& self, const PlanItem& item, const CancelToken* cancel) {
                return self.read(item.item(), cancel);
            },
            "item"_a, "cancel"_a.none() = nb::none());

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
            })
        // Identifies this plan among live ones: a plan item keeps its plan
        // alive, so no other plan can be at the same address meanwhile.
        .def_prop_ro("_key", [](const LeafChunkPlan& self) {
            return reinterpret_cast<std::uintptr_t>(&self);
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
        .def_prop_ro("_plan_key",
            [](const PlanItem& self) {
                return reinterpret_cast<std::uintptr_t>(self.plan.get());
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
