#include "OpenPmdSource.hpp"

#include <amrexplorer/io/PlotfileBlockReader.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace amrvis {
namespace {

// Bounds on what a series may claim, in the spirit of the plotfile reader's:
// a crafted file must not be able to make the metadata read allocate without
// limit. Levels past this are read as a field of their own name.
constexpr int maximumLevels = 32;
constexpr std::size_t maximumFields = 10'000;
constexpr std::size_t maximumBoxes = std::size_t{1} << 20;
// No array extent may exceed this, so a cell index and one past it both fit
// in an int.
constexpr std::uint64_t maximumExtent
    = static_cast<std::uint64_t>(std::numeric_limits<int>::max() / 2);

// The largest box side a written chunk is cut into. A serial HDF5 writer
// stores a level as one chunk, which as a single block would exceed any cache
// budget; AMReX's own max_grid_size is of this order.
int maximumBoxSide(int dimension) noexcept
{
    switch (dimension) {
    case 1:
        return 4096;
    case 2:
        return 256;
    default:
        return 64;
    }
}

struct MeshName {
    std::string base;
    int level = 0;
};

// WarpX writes level N of mesh E as "E_lvlN" (level 0 has no suffix).
MeshName splitLevel(const std::string& name)
{
    const auto at = name.rfind("_lvl");
    if (at == std::string::npos || at == 0) {
        return {name, 0};
    }
    const std::string_view digits(name.data() + at + 4, name.size() - at - 4);
    if (digits.empty() || digits.size() > 2
        || !std::all_of(digits.begin(), digits.end(),
            [](char c) { return c >= '0' && c <= '9'; })) {
        return {name, 0};
    }
    int level = 0;
    for (const char c : digits) {
        level = level * 10 + (c - '0');
    }
    if (level >= maximumLevels) {
        return {name, 0};
    }
    return {name.substr(0, at), level};
}

// The spatial axis each array dimension holds. Labels drawn from x, y, z keep
// their x < y < z order as axes 0, 1, 2 of what is present, so WarpX's 2-D
// ("x", "z") becomes the plotfile's (x, z) whichever way round the array is
// stored. Anything else is read as C order with the first axis fastest,
// which is also what AMReX writes.
std::array<int, 3> axesOf(const std::vector<std::string>& labels, int rank)
{
    std::array<int, 3> axes{0, 1, 2};
    for (int d = 0; d < rank; ++d) {
        axes[static_cast<std::size_t>(d)] = rank - 1 - d;
    }
    if (labels.size() != static_cast<std::size_t>(rank)) {
        return axes;
    }
    std::array<int, 3> letter{0, 0, 0};
    for (int d = 0; d < rank; ++d) {
        const auto& label = labels[static_cast<std::size_t>(d)];
        if (label != "x" && label != "y" && label != "z") {
            return axes;
        }
        letter[static_cast<std::size_t>(d)] = label[0] - 'x';
        for (int e = 0; e < d; ++e) {
            if (letter[static_cast<std::size_t>(e)]
                == letter[static_cast<std::size_t>(d)]) {
                return axes;
            }
        }
    }
    std::array<int, 3> ranked{0, 0, 0};
    for (int d = 0; d < rank; ++d) {
        for (int e = 0; e < rank; ++e) {
            if (letter[static_cast<std::size_t>(e)]
                < letter[static_cast<std::size_t>(d)]) {
                ++ranked[static_cast<std::size_t>(d)];
            }
        }
    }
    return ranked;
}

// One field at one level, with the grid its mesh declares (in SI, by
// spatial axis).
struct Candidate {
    int rank = 0;
    OpenPmdSource::Component component;
    std::array<double, 3> spacing{1.0, 1.0, 1.0};
    std::array<double, 3> offset{0.0, 0.0, 0.0};
};

// Cells along an axis: a nodal component (samples on the lower faces)
// carries one more sample than there are cells.
std::array<int, 3> cellCounts(
    const OpenPmdSource::Component& component, int dimension)
{
    std::array<int, 3> cells{1, 1, 1};
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto samples = static_cast<int>(component.extent[i]);
        cells[i] = component.position[i] == 0.0 && samples > 1 ? samples - 1
                                                               : samples;
    }
    return cells;
}

bool sameGrid(const Candidate& a, const Candidate& b, int dimension)
{
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto tolerance = 1e-9 * std::abs(a.spacing[i]);
        if (std::abs(a.spacing[i] - b.spacing[i]) > tolerance
            || std::abs(a.offset[i] - b.offset[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

openPMD::MeshRecordComponent& recordComponent(openPMD::Series& series,
    std::uint64_t iteration, const OpenPmdSource::Component& component)
{
    return series.iterations[iteration]
        .meshes[component.mesh][component.key];
}

// The chunks written for this component, as cell boxes clipped to the level
// and cut to at most maximumBoxSide per axis. A backend that cannot list
// chunks (or lists none) is read as one chunk over the whole array.
std::vector<IntBox> levelBoxes(openPMD::MeshRecordComponent& rc,
    const OpenPmdSource::Component& component, int dimension,
    const std::array<int, 3>& cells, std::size_t& boxBudget,
    StopToken cancellation)
{
    openPMD::ChunkTable chunks;
    try {
        chunks = rc.availableChunks();
    } catch (const std::exception&) {
        chunks.clear();
    }
    if (chunks.empty()) {
        chunks.emplace_back(openPMD::Offset(static_cast<std::size_t>(dimension), 0),
            rc.getExtent());
    }
    const auto side = maximumBoxSide(dimension);
    std::vector<IntBox> boxes;
    for (const auto& chunk : chunks) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        if (chunk.offset.size() != static_cast<std::size_t>(dimension)
            || chunk.extent.size() != static_cast<std::size_t>(dimension)) {
            continue;
        }
        IntBox clipped;
        bool empty = false;
        for (int d = 0; d < dimension; ++d) {
            const auto dim = static_cast<std::size_t>(d);
            const auto axis = static_cast<std::size_t>(
                component.axisOfDimension[dim]);
            if (chunk.extent[dim] == 0 || chunk.offset[dim] >= maximumExtent
                || chunk.extent[dim] > maximumExtent) {
                empty = true;
                break;
            }
            const auto lower = static_cast<int>(chunk.offset[dim]);
            const auto upper = std::min(
                lower + static_cast<int>(chunk.extent[dim]) - 1,
                cells[axis] - 1);
            if (lower > upper) {
                empty = true;
                break;
            }
            clipped.lower[axis] = lower;
            clipped.upper[axis] = upper;
        }
        if (empty) {
            continue;
        }
        // Cut into tiles of at most `side` cells per axis.
        std::array<int, 3> tiles{1, 1, 1};
        for (int axis = 0; axis < dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            tiles[i] = (clipped.upper[i] - clipped.lower[i]) / side + 1;
        }
        const auto count = static_cast<std::size_t>(tiles[0])
            * static_cast<std::size_t>(tiles[1])
            * static_cast<std::size_t>(tiles[2]);
        if (count > boxBudget) {
            throw MetadataReadError(
                "openPMD series has more boxes than this reader accepts");
        }
        boxBudget -= count;
        for (int k = 0; k < tiles[2]; ++k) {
            for (int j = 0; j < tiles[1]; ++j) {
                for (int i = 0; i < tiles[0]; ++i) {
                    IntBox box = clipped;
                    const std::array<int, 3> tile{i, j, k};
                    for (int axis = 0; axis < dimension; ++axis) {
                        const auto a = static_cast<std::size_t>(axis);
                        box.lower[a] = clipped.lower[a] + tile[a] * side;
                        box.upper[a] = std::min(
                            box.lower[a] + side - 1, clipped.upper[a]);
                    }
                    boxes.push_back(box);
                }
            }
        }
    }
    return boxes;
}

} // namespace

OpenPmdSource::OpenPmdSource(
    const std::filesystem::path& path, StopToken cancellation)
// A staggered vector's components have different extents (E_x has one more
// sample along x than E_y), which openPMD-api refuses to read by default.
try : m_series(path.string(), openPMD::Access::READ_ONLY,
          R"({"verify_homogeneous_extents": false})")
    , m_path(path)
{
    if (cancellation.stop_requested()) {
        throw ReadCancelled();
    }
    if (m_series.iterations.empty()) {
        throw MetadataReadError("the openPMD series holds no iterations");
    }
    auto entry = m_series.iterations.begin();
    m_iteration = entry->first;
    auto& iteration = entry->second;

    // Every field this reader can present, by name (so the field list comes
    // out sorted), then by level.
    std::map<std::string, std::map<int, Candidate>> byField;
    std::size_t candidates = 0;
    for (auto& [name, mesh] : iteration.meshes) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        // Cartesian, C-ordered meshes only; the rest are left out rather than
        // failing the series.
        if (mesh.geometry() != openPMD::Mesh::Geometry::cartesian
            || mesh.dataOrder() != openPMD::Mesh::DataOrder::C) {
            continue;
        }
        const auto split = splitLevel(name);
        const auto labels = mesh.axisLabels();
        const auto spacing = mesh.gridSpacing<double>();
        const auto offset = mesh.gridGlobalOffset();
        const auto unit = mesh.gridUnitSI();
        for (auto& [key, rc] : mesh) {
            const auto extent = rc.getExtent();
            const auto rank = static_cast<int>(extent.size());
            if (rank < 1 || rank > 3
                || spacing.size() != extent.size()
                || offset.size() != extent.size()) {
                continue;
            }
            const auto position = rc.position<double>();
            Candidate candidate;
            candidate.rank = rank;
            auto& component = candidate.component;
            component.mesh = name;
            component.key = key;
            component.axisOfDimension = axesOf(labels, rank);
            component.unitSI = rc.unitSI();
            bool usable = std::isfinite(component.unitSI);
            for (int d = 0; d < rank && usable; ++d) {
                const auto dim = static_cast<std::size_t>(d);
                const auto axis = static_cast<std::size_t>(
                    component.axisOfDimension[dim]);
                component.extent[axis] = extent[dim];
                component.position[axis]
                    = position.size() == extent.size() ? position[dim] : 0.5;
                candidate.spacing[axis] = spacing[dim] * unit;
                candidate.offset[axis] = offset[dim] * unit;
                usable = extent[dim] > 0 && extent[dim] <= maximumExtent
                    && component.position[axis] >= 0.0
                    && component.position[axis] <= 1.0
                    && candidate.spacing[axis] > 0.0
                    && std::isfinite(candidate.spacing[axis])
                    && std::isfinite(candidate.offset[axis]);
            }
            if (!usable) {
                continue;
            }
            if (++candidates > maximumFields) {
                throw MetadataReadError(
                    "openPMD series has more mesh components than this "
                    "reader accepts");
            }
            const auto field = key == openPMD::RecordComponent::SCALAR
                ? split.base
                : split.base + "_" + key;
            byField[field][split.level] = std::move(candidate);
        }
    }

    // The hierarchy is the reference field's: preferably one sampled at cell
    // centres, whose extents are the cell counts as they stand.
    const auto centred = [](const Candidate& candidate) {
        const auto& position = candidate.component.position;
        return std::all_of(position.begin(), position.begin() + candidate.rank,
            [](double p) { return p == 0.5; });
    };
    const std::map<int, Candidate>* referenceLevels = nullptr;
    for (const auto& [field, levels] : byField) {
        if (!levels.contains(0)) {
            continue;
        }
        if (referenceLevels == nullptr
            || (centred(levels.at(0)) && !centred(referenceLevels->at(0)))) {
            referenceLevels = &levels;
        }
    }
    if (referenceLevels == nullptr) {
        throw MetadataReadError(
            "the openPMD series has no cartesian mesh this reader can show");
    }
    const auto dimension = referenceLevels->at(0).rank;

    // Fields on the reference grid at level 0; each then limits the finest
    // level to the last of an unbroken run of levels it shares with the
    // reference, so every level holds every field.
    std::vector<const std::map<int, Candidate>*> kept;
    std::vector<std::string> names;
    int finest = 0;
    while (referenceLevels->contains(finest + 1)) {
        ++finest;
    }
    for (const auto& [field, levels] : byField) {
        const auto base = levels.find(0);
        if (base == levels.end() || base->second.rank != dimension
            || !sameGrid(base->second, referenceLevels->at(0), dimension)) {
            continue;
        }
        int run = 0;
        while (run < finest && levels.contains(run + 1)
            && levels.at(run + 1).rank == dimension
            && sameGrid(levels.at(run + 1), referenceLevels->at(run + 1),
                dimension)) {
            ++run;
        }
        finest = run;
        kept.push_back(&levels);
        names.push_back(field);
    }

    auto metadata = std::make_shared<DatasetMetadata>();
    metadata->dimension = dimension;
    metadata->finestLevel = finest;
    try {
        metadata->time = iteration.time<double>() * iteration.timeUnitSI();
    } catch (const std::exception&) {
        metadata->time = 0.0;
    }
    if (!std::isfinite(metadata->time)) {
        metadata->time = 0.0;
    }
    for (const auto& name : names) {
        metadata->fields.push_back({name, Centering::Cell, {name}});
    }
    auto filePath = path.filename().string();
    if (filePath.empty()) {
        filePath = path.parent_path().filename().string();
    }
    if (filePath.empty()) {
        filePath = "openPMD";
    }

    std::size_t boxBudget = maximumBoxes;
    m_components.resize(static_cast<std::size_t>(finest) + 1);
    for (int levelIndex = 0; levelIndex <= finest; ++levelIndex) {
        const auto& levelReference = referenceLevels->at(levelIndex);
        const auto cells = cellCounts(levelReference.component, dimension);
        LevelMetadata level;
        level.level = levelIndex;
        level.step = static_cast<int>(std::min<std::uint64_t>(
            m_iteration, static_cast<std::uint64_t>(
                std::numeric_limits<int>::max())));
        for (int axis = 0; axis < dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            level.domain.upper[i] = cells[i] - 1;
            level.cellSize[i] = levelReference.spacing[i];
            level.indexOrigin[i] = levelReference.offset[i];
        }
        level.storedComponents = static_cast<int>(names.size());
        level.boxes = levelBoxes(
            recordComponent(m_series, m_iteration, levelReference.component),
            levelReference.component, dimension, cells, boxBudget,
            cancellation);
        level.blocks.reserve(level.boxes.size());
        for (const auto& box : level.boxes) {
            level.blocks.push_back({box, filePath, 0, std::nullopt});
        }
        auto& components = m_components[static_cast<std::size_t>(levelIndex)];
        for (const auto* levels : kept) {
            components.push_back(levels->at(levelIndex).component);
        }
        metadata->levels.push_back(std::move(level));
    }
    const auto& coarse = metadata->levels.front();
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        metadata->physicalDomain.lower[i] = coarse.indexOrigin[i];
        metadata->physicalDomain.upper[i] = coarse.indexOrigin[i]
            + static_cast<double>(coarse.domain.upper[i] + 1)
                * coarse.cellSize[i];
    }

    if (const auto issues = validateMetadata(*metadata); !issues.empty()) {
        throw MetadataReadError("openPMD series metadata is invalid: "
            + issues.front().path + " " + issues.front().message);
    }
    m_metadata = std::move(metadata);
    m_fileVersion = "openPMD " + m_series.openPMD() + " ("
        + m_series.backend() + ")";
    m_metrics.filesRead = 1;
} catch (const MetadataReadError&) {
    throw;
} catch (const ReadCancelled&) {
    throw;
} catch (const std::exception& error) {
    throw MetadataReadError("cannot read openPMD series '" + path.string()
        + "': " + error.what());
}

std::shared_ptr<const DatasetMetadata> OpenPmdSource::metadata() const noexcept
{
    return m_metadata;
}

const MetadataReadMetrics& OpenPmdSource::metadataReadMetrics() const noexcept
{
    return m_metrics;
}

const std::string& OpenPmdSource::fileVersion() const noexcept
{
    return m_fileVersion;
}

const std::vector<ParticleSpeciesMetadata>& OpenPmdSource::particleSpecies()
    const noexcept
{
    return m_particleSpecies;
}

ParticleSample OpenPmdSource::readParticleSample(const std::string& species,
    double, std::uint64_t, StopToken, std::size_t,
    std::optional<ParticleAttribute>) const
{
    throw ParticleReadError(
        "particle species '" + species + "' is not in this openPMD series");
}

BlockReadResult OpenPmdSource::readBlock(
    const BlockRequest& request, StopToken cancellation) const
{
    if (cancellation.stop_requested()) {
        throw ReadCancelled();
    }
    const auto& metadata = *m_metadata;
    if (request.level < 0
        || static_cast<std::size_t>(request.level) >= metadata.levels.size()) {
        throw BlockReadError("requested level is unavailable");
    }
    const auto& level = metadata.levels[static_cast<std::size_t>(request.level)];
    if (request.gridIndex < 0
        || static_cast<std::size_t>(request.gridIndex) >= level.blocks.size()) {
        throw BlockReadError("requested grid is unavailable");
    }
    if (request.componentCount != 1 || request.firstComponent != 0) {
        throw BlockReadError("an openPMD field has one component");
    }
    const auto& components = m_components[static_cast<std::size_t>(request.level)];
    if (request.field.value >= components.size()) {
        throw BlockReadError("requested field component is unavailable");
    }
    const auto& component = components[request.field.value];
    const auto& box = level.blocks[static_cast<std::size_t>(request.gridIndex)].box;
    const auto dimension = metadata.dimension;

    // A cell's value is interpolated linearly, axis by axis, between the two
    // samples bracketing its centre. The sample below cell i is
    // i + floor(0.5 - position), and the weight of the one above it is the
    // same on every cell: 0 for a cell-centred component (which is then read
    // as it stands), one half for a nodal or face-centred one. Samples past
    // the array's ends are clamped to it.
    struct AxisPlan {
        std::int64_t first = 0;
        std::int64_t count = 1;
        double weight = 0.0;
        std::vector<std::int64_t> below;
        std::vector<std::int64_t> above;
    };
    std::array<AxisPlan, 3> plans;
    for (int axis = 0; axis < 3; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        auto& plan = plans[i];
        if (axis >= dimension) {
            plan.below = {0};
            plan.above = {0};
            continue;
        }
        const auto shift = 0.5 - component.position[i];
        const auto step = static_cast<std::int64_t>(std::floor(shift));
        plan.weight = shift - std::floor(shift);
        const auto last = static_cast<std::int64_t>(component.extent[i]) - 1;
        const auto clamp = [last](std::int64_t sample) {
            return std::clamp<std::int64_t>(sample, 0, last);
        };
        plan.first = clamp(box.lower[i] + step);
        const auto end = clamp(box.upper[i] + step + (plan.weight > 0.0 ? 1 : 0));
        plan.count = end - plan.first + 1;
        for (std::int64_t cell = box.lower[i]; cell <= box.upper[i]; ++cell) {
            plan.below.push_back(clamp(cell + step) - plan.first);
            plan.above.push_back(clamp(cell + step + 1) - plan.first);
        }
    }

    // The array region those samples span, by array dimension, and the
    // stride of each spatial axis within it (C order: last dimension fastest).
    const auto rank = static_cast<std::size_t>(dimension);
    openPMD::Offset offset(rank, 0);
    openPMD::Extent extent(rank, 1);
    std::array<std::int64_t, 3> stride{0, 0, 0};
    std::int64_t loaded = 1;
    for (std::size_t d = rank; d-- > 0;) {
        const auto axis = static_cast<std::size_t>(component.axisOfDimension[d]);
        offset[d] = static_cast<std::uint64_t>(plans[axis].first);
        extent[d] = static_cast<std::uint64_t>(plans[axis].count);
        stride[axis] = loaded;
        loaded *= plans[axis].count;
    }

    openPMD::RecordComponent::shared_ptr_dataset_types data;
    openPMD::Datatype datatype{};
    try {
        std::scoped_lock lock(m_mutex);
        auto& rc = recordComponent(m_series, m_iteration, component);
        datatype = rc.getDatatype();
        data = rc.loadChunkVariant(offset, extent);
        m_series.flush();
    } catch (const std::exception& error) {
        throw BlockReadError("cannot read openPMD mesh '" + component.mesh
            + "': " + error.what());
    }
    if (cancellation.stop_requested()) {
        throw ReadCancelled();
    }

    auto block = std::make_shared<FabBlock>();
    block->box = box;
    block->field = request.field;
    block->component = 0;
    const auto points = fabPointCount(box, dimension);
    std::visit(
        [&](const auto& samples) {
            using Sample = typename std::decay_t<decltype(samples)>::element_type;
            if constexpr (!std::is_arithmetic_v<Sample>
                || std::is_same_v<Sample, bool>) {
                throw BlockReadError("openPMD mesh '" + component.mesh
                    + "' has a datatype this reader cannot show");
            } else {
                // Single precision stays single precision, as a plotfile's
                // does; every other type is read as double.
                using Value = std::conditional_t<std::is_same_v<Sample, float>,
                    float, double>;
                std::vector<Value> values;
                values.reserve(static_cast<std::size_t>(points));
                const auto* source = samples.get();
                const auto& x = plans[0];
                const auto& y = plans[1];
                const auto& z = plans[2];
                for (std::size_t k = 0; k < z.below.size(); ++k) {
                    for (std::size_t j = 0; j < y.below.size(); ++j) {
                        for (std::size_t i = 0; i < x.below.size(); ++i) {
                            double value = 0.0;
                            // The up-to-eight samples around the centre; a
                            // corner of zero weight is never read.
                            for (int corner = 0; corner < 8; ++corner) {
                                double weight = 1.0;
                                std::int64_t at = 0;
                                const std::array<std::size_t, 3> cell{i, j, k};
                                for (std::size_t a = 0; a < 3; ++a) {
                                    const bool upper = ((corner >> a) & 1) != 0;
                                    const auto& plan = plans[a];
                                    weight *= upper ? plan.weight
                                                    : 1.0 - plan.weight;
                                    at += stride[a]
                                        * (upper ? plan.above[cell[a]]
                                                 : plan.below[cell[a]]);
                                }
                                if (weight != 0.0) {
                                    value += weight
                                        * static_cast<double>(
                                            source[static_cast<std::size_t>(at)]);
                                }
                            }
                            values.push_back(
                                static_cast<Value>(value * component.unitSI));
                        }
                    }
                }
                block->values = FabValues{std::move(values)};
            }
        },
        data);

    BlockReadMetrics metrics;
    metrics.filesRead = 1;
    metrics.valuesRead = static_cast<std::uint64_t>(loaded);
    metrics.bytesRead = static_cast<std::uint64_t>(loaded)
        * static_cast<std::uint64_t>(openPMD::toBytes(datatype));
    return {std::shared_ptr<const FabBlock>(std::move(block)), metrics};
}

} // namespace amrvis
