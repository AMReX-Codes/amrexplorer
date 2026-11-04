#include <amrexplorer/analysis/LeafChunks.hpp>

#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/detail/FabHeaderParsing.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace amrvis {
namespace {

// What a box holds, zero for an empty box. Only for boxes bounded by a grid:
// a box over a deep level's domain can hold more cells than 64 bits count.
std::uint64_t cellCount(const IntBox& box, int dimension) noexcept
{
    std::uint64_t result = 1;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto length = static_cast<std::int64_t>(box.upper[i])
            - box.lower[i] + 1;
        if (length <= 0) {
            return 0;
        }
        result *= static_cast<std::uint64_t>(length);
    }
    return result;
}

std::optional<IntBox> intersect(
    const IntBox& left, const IntBox& right, int dimension) noexcept
{
    IntBox result = left;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        result.lower[i] = std::max(left.lower[i], right.lower[i]);
        result.upper[i] = std::min(left.upper[i], right.upper[i]);
        if (result.lower[i] > result.upper[i]) {
            return std::nullopt;
        }
    }
    return result;
}

int floorDivide(int value, int divisor) noexcept
{
    const auto quotient = value / divisor;
    return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}

IntBox coarsen(const IntBox& box, const Int3& ratio, int dimension) noexcept
{
    IntBox result = box;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        result.lower[i] = floorDivide(box.lower[i], ratio[i]);
        result.upper[i] = floorDivide(box.upper[i], ratio[i]);
    }
    return result;
}

// The refinement ratio from `coarse` to `fine`, from their domains. AMReX
// writes the ratios in the Header too, but the domains are what the boxes are
// indexed against, so they are the ratio the coverage test must use.
Int3 refinementRatio(
    const LevelMetadata& coarse, const LevelMetadata& fine, int dimension)
{
    Int3 ratio{{1, 1, 1}};
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto coarseCells = static_cast<std::int64_t>(coarse.domain.upper[i])
            - coarse.domain.lower[i] + 1;
        const auto fineCells = static_cast<std::int64_t>(fine.domain.upper[i])
            - fine.domain.lower[i] + 1;
        if (coarseCells <= 0 || fineCells < coarseCells
            || fineCells % coarseCells != 0) {
            std::ostringstream message;
            message << "level " << fine.level << " is not an integer "
                    << "refinement of level " << coarse.level << " on axis "
                    << axis;
            throw AnalysisError(message.str());
        }
        ratio[i] = static_cast<int>(fineCells / coarseCells);
    }
    return ratio;
}

double cellCenter(const Real3& origin, const Real3& cellSize, int axis,
    std::int64_t index) noexcept
{
    const auto i = static_cast<std::size_t>(axis);
    return origin[i] + (static_cast<double>(index) + 0.5) * cellSize[i];
}

// The cells of a level a region selects along one axis: lower <= center <
// upper, decided with the same center arithmetic the chunk's coordinate
// arrays use, so a cell is selected exactly when its reported center lies in
// the region. Returns an empty range as first > last.
std::pair<int, int> selectedRange(const LevelMetadata& level, int axis,
    double lower, double upper)
{
    const auto i = static_cast<std::size_t>(axis);
    const auto& origin = level.indexOrigin;
    const auto& dx = level.cellSize;
    const auto first = [&](double bound) {
        // An estimate, then corrected against the exact test both ways.
        const auto estimate = std::ceil((bound - origin[i]) / dx[i] - 0.5);
        const auto clamped = std::clamp(estimate,
            static_cast<double>(level.domain.lower[i]) - 1.0,
            static_cast<double>(level.domain.upper[i]) + 1.0);
        auto index = static_cast<std::int64_t>(clamped);
        while (cellCenter(origin, dx, axis, index) < bound
            && index <= level.domain.upper[i]) {
            ++index;
        }
        while (index > level.domain.lower[i]
            && cellCenter(origin, dx, axis, index - 1) >= bound) {
            --index;
        }
        return index;
    };
    const auto low = std::max<std::int64_t>(first(lower), level.domain.lower[i]);
    const auto high
        = std::min<std::int64_t>(first(upper) - 1, level.domain.upper[i]);
    return {static_cast<int>(low), static_cast<int>(high)};
}

void collectBlockFields(const PlotfileDataset& dataset, FieldId field,
    std::set<std::uint32_t>& fields)
{
    if (!fields.insert(field.value).second) {
        return;
    }
    if (const auto* program = dataset.derivedFieldProgram(field)) {
        for (const auto& input : program->inputs) {
            if (!input.isCoordinate()) {
                collectBlockFields(dataset, input.field, fields);
            }
        }
    }
}

void requireSupportedGeometry(const DatasetMetadata& metadata)
{
    if (metadata.isFab || !metadata.hasPhysicalGeometry) {
        throw AnalysisError("leaf-cell analysis needs a plotfile with "
                            "physical geometry, not a standalone FAB or "
                            "MultiFab");
    }
    if (metadata.dimension < 1 || metadata.dimension > 3) {
        throw AnalysisError("the plotfile's dimension is unsupported");
    }
    if (metadata.coordinateSystem
        != static_cast<int>(CoordinateSystem::Cartesian)) {
        throw AnalysisError("leaf-cell analysis supports Cartesian plotfiles "
                            "only; this one has coordinate system "
            + std::to_string(metadata.coordinateSystem));
    }
    if (metadata.hasMappedGrid) {
        throw AnalysisError("leaf-cell analysis does not support plotfiles "
                            "with a mapped grid");
    }
    if (metadata.levels.empty()) {
        throw AnalysisError("the plotfile has no levels");
    }
    for (const auto& level : metadata.levels) {
        for (int axis = 0; axis < metadata.dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            if (!(level.cellSize[i] > 0.0) || !std::isfinite(level.cellSize[i])
                || !std::isfinite(level.indexOrigin[i])) {
                throw AnalysisError("level " + std::to_string(level.level)
                    + " has an invalid cell size or origin");
            }
            if (isNodal(level.domain, axis)) {
                throw AnalysisError("level " + std::to_string(level.level)
                    + " is not cell-centered");
            }
        }
    }
}

std::string describeItem(const LeafChunkItem& item)
{
    std::ostringstream text;
    text << "plan item " << item.index << " (level " << item.level
         << ", grid " << item.grid << ")";
    return text.str();
}

template <typename Output>
std::vector<Output> gather(const FabBlock& block, const IntBox& validBox,
    int dimension, const std::vector<std::uint32_t>& offsets)
{
    // Strides of the block's own box (stored blocks carry ghost cells, derived
    // ones do not) and of the valid box the offsets index.
    std::array<std::size_t, 3> blockStride{1, 1, 1};
    std::array<std::size_t, 3> validExtent{1, 1, 1};
    std::size_t base = 0;
    std::size_t stride = 1;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        if (validBox.lower[i] < block.box.lower[i]
            || validBox.upper[i] > block.box.upper[i]) {
            throw BlockReadError("a block does not cover its grid's valid box");
        }
        blockStride[i] = stride;
        base += stride
            * static_cast<std::size_t>(validBox.lower[i] - block.box.lower[i]);
        stride *= static_cast<std::size_t>(
            static_cast<std::int64_t>(block.box.upper[i]) - block.box.lower[i]
            + 1);
        validExtent[i] = static_cast<std::size_t>(
            static_cast<std::int64_t>(validBox.upper[i]) - validBox.lower[i]
            + 1);
    }
    if (block.values.size() < stride) {
        throw BlockReadError("a block's payload is smaller than its box");
    }
    std::vector<Output> result(offsets.size());
    const auto copy = [&](auto values) {
        for (std::size_t cell = 0; cell < offsets.size(); ++cell) {
            auto offset = static_cast<std::size_t>(offsets[cell]);
            const auto i = offset % validExtent[0];
            offset /= validExtent[0];
            const auto j = offset % validExtent[1];
            const auto k = offset / validExtent[1];
            result[cell] = static_cast<Output>(values[base + i
                + blockStride[1] * j + blockStride[2] * k]);
        }
    };
    if (const auto single = block.values.singleValues(); !single.empty()) {
        copy(single);
    } else {
        copy(block.values.doubleValues());
    }
    return result;
}

} // namespace

AnalysisValueType storedValueType(const PlotfileDataset& dataset, int levelIndex)
{
    const auto& metadata = dataset.metadata();
    const auto& level = metadata.levels.at(static_cast<std::size_t>(levelIndex));
    std::string descriptor = level.realDescriptor;
    if (descriptor.empty()) {
        // A version-1 VisMF header records no precision of its own: each FAB
        // carries its RealDescriptor in the one-line header before its
        // payload. AMReX writes one precision per MultiFab, so the first
        // grid's header speaks for the level -- one short read, not a payload.
        if (level.blocks.empty()) {
            return AnalysisValueType::Float32;
        }
        const auto& block = level.blocks.front();
        const auto path = dataset.dataRoot() / block.filePath;
        std::ifstream input(path, std::ios::binary);
        input.seekg(static_cast<std::streamoff>(block.fileOffset), std::ios::beg);
        std::string line;
        if (!input || !detail::readBoundedLine<AnalysisError>(input, line)
            || !line.starts_with("FAB ")) {
            throw AnalysisError("cannot read the FAB header of level "
                + std::to_string(levelIndex) + " grid 0 in '" + path.string()
                + "' to find the level's precision");
        }
        const auto start = line.find('(', 4);
        const auto end
            = detail::balancedExpressionEnd<AnalysisError>(line, start);
        descriptor = line.substr(start, end - start);
    }
    const auto parsed = detail::parseRealDescriptor<AnalysisError>(descriptor);
    return parsed.bytes == sizeof(float) ? AnalysisValueType::Float32
                                         : AnalysisValueType::Float64;
}

AnalysisValueType widestStoredValueType(const PlotfileDataset& dataset)
{
    auto widest = AnalysisValueType::Float32;
    const auto levels = static_cast<int>(dataset.metadata().levels.size());
    for (int level = 0; level < levels; ++level) {
        if (storedValueType(dataset, level) == AnalysisValueType::Float64) {
            widest = AnalysisValueType::Float64;
        }
    }
    return widest;
}

std::uint64_t estimateLeafChunkPinnedBytes(const PlotfileDataset& dataset,
    const std::vector<FieldId>& fields, int levelIndex, int grid)
{
    const auto& metadata = dataset.metadata();
    const auto& level = metadata.levels.at(static_cast<std::size_t>(levelIndex));
    auto box = level.blocks.at(static_cast<std::size_t>(grid)).box;
    for (int axis = 0; axis < metadata.dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        box.lower[i] -= level.ghostWidth[i];
        box.upper[i] += level.ghostWidth[i];
    }
    std::set<std::uint32_t> blocks;
    for (const auto field : fields) {
        collectBlockFields(dataset, field, blocks);
    }
    const auto cells = cellCount(box, metadata.dimension);
    const auto perBlockLimit = (std::numeric_limits<std::uint64_t>::max()
        - leafChunkBlockAllowanceBytes) / sizeof(double);
    if (cells > perBlockLimit) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const auto perBlock = cells * sizeof(double) + leafChunkBlockAllowanceBytes;
    if (!blocks.empty()
        && perBlock > std::numeric_limits<std::uint64_t>::max() / blocks.size()) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return perBlock * blocks.size();
}

LeafChunkPlan planLeafChunks(
    const PlotfileDataset& dataset, const LeafChunkRequest& request)
{
    const auto& metadata = dataset.metadata();
    requireSupportedGeometry(metadata);
    const auto dimension = metadata.dimension;

    auto selection = std::make_shared<LeafChunkSelection>();
    selection->dataset = dataset.id();
    selection->fieldNames = request.fields;
    for (const auto& name : request.fields) {
        const auto found = std::find_if(metadata.fields.begin(),
            metadata.fields.end(),
            [&](const FieldMetadata& field) { return field.name == name; });
        if (found == metadata.fields.end()) {
            std::string available;
            for (const auto& field : metadata.fields) {
                available += available.empty() ? "" : ", ";
                available += field.name;
            }
            throw AnalysisError("unknown field '" + name
                + "'; the dataset has: " + available);
        }
        if (found->centering != Centering::Cell) {
            throw AnalysisError("field '" + name
                + "' is not cell-centered; leaf-cell analysis supports "
                  "cell-centered fields only");
        }
        const FieldId id{static_cast<std::uint32_t>(
            std::distance(metadata.fields.begin(), found))};
        if (std::find(selection->fields.begin(), selection->fields.end(), id)
            != selection->fields.end()) {
            throw AnalysisError("field '" + name + "' is requested twice");
        }
        selection->fields.push_back(id);
    }
    if (request.region) {
        for (int axis = 0; axis < dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            const auto lower = request.region->lower[i];
            const auto upper = request.region->upper[i];
            if (!std::isfinite(lower) || !std::isfinite(upper)
                || !(lower < upper)) {
                throw AnalysisError("the region must have finite bounds "
                                    "with lower < upper on every axis");
            }
        }
    }
    selection->region = request.region;
    selection->dtype = request.dtype.value_or(widestStoredValueType(dataset));

    LeafChunkPlan plan;
    plan.selection = selection;
    const auto budget = dataset.cacheMetrics().budgetBytes;
    std::vector<std::uint8_t> mask;
    for (std::size_t levelIndex = 0; levelIndex < metadata.levels.size();
         ++levelIndex) {
        const auto& level = metadata.levels[levelIndex];
        // The region as an index box on this level, by cell center.
        IntBox regionBox = level.domain;
        if (request.region) {
            for (int axis = 0; axis < dimension; ++axis) {
                const auto i = static_cast<std::size_t>(axis);
                const auto [first, last] = selectedRange(level, axis,
                    request.region->lower[i], request.region->upper[i]);
                regionBox.lower[i] = first;
                regionBox.upper[i] = last;
            }
        }
        // Per axis, not by counting: a deep level's region box can hold
        // 2^64 cells or more, and a wrapped count reads as empty.
        bool emptyRegion = false;
        for (int axis = 0; axis < dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            emptyRegion = emptyRegion || regionBox.lower[i] > regionBox.upper[i];
        }
        if (emptyRegion) {
            continue;
        }
        // The next finer level's grids in this level's index space.
        std::vector<IntBox> finer;
        if (levelIndex + 1 < metadata.levels.size()) {
            const auto& next = metadata.levels[levelIndex + 1];
            const auto ratio = refinementRatio(level, next, dimension);
            finer.reserve(next.blocks.size());
            for (const auto& block : next.blocks) {
                finer.push_back(coarsen(block.box, ratio, dimension));
            }
        }
        for (std::size_t grid = 0; grid < level.blocks.size(); ++grid) {
            const auto& box = level.blocks[grid].box;
            const auto selected = intersect(box, regionBox, dimension);
            if (!selected) {
                continue;
            }
            LeafChunkItem item;
            item.level = static_cast<int>(levelIndex);
            item.grid = static_cast<int>(grid);
            item.box = box;
            item.selectedBox = *selected;
            for (const auto& cover : finer) {
                if (const auto clipped = intersect(cover, *selected, dimension)) {
                    item.covered.push_back(*clipped);
                }
            }
            const auto selectedCells = cellCount(*selected, dimension);
            if (selectedCells > std::numeric_limits<std::uint32_t>::max()) {
                throw AnalysisError("level " + std::to_string(levelIndex)
                    + " grid " + std::to_string(grid)
                    + " has more cells than a chunk can index");
            }
            std::uint64_t coveredCells = 0;
            if (item.covered.size() == 1) {
                coveredCells = cellCount(item.covered.front(), dimension);
            } else if (!item.covered.empty()) {
                // Coarsened fine grids may overlap one another when they are
                // not aligned to the ratio, so count their union by marking.
                mask.assign(static_cast<std::size_t>(selectedCells), 0);
                std::array<std::int64_t, 3> extent{1, 1, 1};
                for (int axis = 0; axis < dimension; ++axis) {
                    const auto i = static_cast<std::size_t>(axis);
                    extent[i] = static_cast<std::int64_t>(selected->upper[i])
                        - selected->lower[i] + 1;
                }
                for (const auto& cover : item.covered) {
                    const auto lo = [&](std::size_t i) {
                        return i < static_cast<std::size_t>(dimension)
                            ? cover.lower[i] - selected->lower[i] : 0;
                    };
                    const auto hi = [&](std::size_t i) {
                        return i < static_cast<std::size_t>(dimension)
                            ? cover.upper[i] - selected->lower[i] : 0;
                    };
                    for (std::int64_t k = lo(2); k <= hi(2); ++k) {
                        for (std::int64_t j = lo(1); j <= hi(1); ++j) {
                            const auto row = (k * extent[1] + j) * extent[0];
                            for (std::int64_t i = lo(0); i <= hi(0); ++i) {
                                auto& cell = mask[static_cast<std::size_t>(
                                    row + i)];
                                coveredCells += cell == 0 ? 1 : 0;
                                cell = 1;
                            }
                        }
                    }
                }
            }
            item.selectedCells = selectedCells - coveredCells;
            if (item.selectedCells == 0) {
                continue;
            }
            item.estimatedPinnedBytes = estimateLeafChunkPinnedBytes(
                dataset, selection->fields, item.level, item.grid);
            if (item.estimatedPinnedBytes > budget) {
                std::ostringstream message;
                message << "level " << item.level << " grid " << item.grid
                        << " needs up to " << item.estimatedPinnedBytes
                        << " bytes of block cache for the requested fields, "
                           "more than the "
                        << budget << "-byte budget; raise cache_bytes";
                throw AnalysisError(message.str());
            }
            item.index = plan.items.size();
            item.selection = selection;
            plan.totalCells += item.selectedCells;
            plan.items.push_back(std::move(item));
        }
    }
    return plan;
}

std::shared_ptr<const LeafChunk> readLeafChunk(
    PlotfileDataset& dataset, const LeafChunkItem& item, StopToken cancellation)
{
    if (!item.selection || item.selection->dataset != dataset.id()) {
        throw AnalysisError(describeItem(item)
            + " was planned on a different dataset; plan again on this one");
    }
    const auto& metadata = dataset.metadata();
    const auto dimension = metadata.dimension;
    if (item.level < 0
        || static_cast<std::size_t>(item.level) >= metadata.levels.size()) {
        throw AnalysisError(describeItem(item) + " names a missing level");
    }
    const auto& level = metadata.levels[static_cast<std::size_t>(item.level)];
    if (item.grid < 0
        || static_cast<std::size_t>(item.grid) >= level.blocks.size()) {
        throw AnalysisError(describeItem(item) + " names a missing grid");
    }

    // The selected leaf cells as offsets into the valid box, x fastest.
    std::array<std::int64_t, 3> extent{1, 1, 1};
    std::array<std::int64_t, 3> lower{0, 0, 0};
    std::array<std::int64_t, 3> upper{0, 0, 0};
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        extent[i] = static_cast<std::int64_t>(item.box.upper[i])
            - item.box.lower[i] + 1;
        lower[i] = item.selectedBox.lower[i] - item.box.lower[i];
        upper[i] = item.selectedBox.upper[i] - item.box.lower[i];
    }
    const auto boxCells = cellCount(item.box, dimension);
    if (boxCells > std::numeric_limits<std::uint32_t>::max()) {
        throw AnalysisError(describeItem(item)
            + " has more cells than a chunk can index");
    }
    std::vector<std::uint8_t> covered;
    if (!item.covered.empty()) {
        covered.assign(static_cast<std::size_t>(boxCells), 0);
        for (const auto& cover : item.covered) {
            std::array<std::int64_t, 3> lo{0, 0, 0};
            std::array<std::int64_t, 3> hi{0, 0, 0};
            for (int axis = 0; axis < dimension; ++axis) {
                const auto i = static_cast<std::size_t>(axis);
                lo[i] = cover.lower[i] - item.box.lower[i];
                hi[i] = cover.upper[i] - item.box.lower[i];
            }
            for (auto k = lo[2]; k <= hi[2]; ++k) {
                for (auto j = lo[1]; j <= hi[1]; ++j) {
                    const auto row = (k * extent[1] + j) * extent[0];
                    std::fill(covered.begin() + row + lo[0],
                        covered.begin() + row + hi[0] + 1, std::uint8_t{1});
                }
            }
        }
    }
    std::vector<std::uint32_t> offsets;
    offsets.reserve(static_cast<std::size_t>(item.selectedCells));
    for (auto k = lower[2]; k <= upper[2]; ++k) {
        for (auto j = lower[1]; j <= upper[1]; ++j) {
            const auto row = (k * extent[1] + j) * extent[0];
            for (auto i = lower[0]; i <= upper[0]; ++i) {
                const auto offset = row + i;
                if (covered.empty()
                    || covered[static_cast<std::size_t>(offset)] == 0) {
                    offsets.push_back(static_cast<std::uint32_t>(offset));
                }
            }
        }
    }

    const auto& selection = *item.selection;
    std::vector<AnalysisArray> fields;
    fields.reserve(selection.fields.size());
    for (const auto field : selection.fields) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        BlockRequest request;
        request.dataset = dataset.id();
        request.level = item.level;
        request.gridIndex = item.grid;
        request.field = field;
        // One field's blocks at a time: the handle releases its pin at the end
        // of this iteration, before the next field is requested.
        try {
            const auto access = dataset.requestBlock(request, cancellation);
            if (selection.dtype == AnalysisValueType::Float32) {
                fields.emplace_back(
                    gather<float>(*access.handle, item.box, dimension, offsets));
            } else {
                fields.emplace_back(gather<double>(
                    *access.handle, item.box, dimension, offsets));
            }
        } catch (const CacheBudgetExceeded& error) {
            throw CacheBudgetExceeded(describeItem(item) + ": " + error.what()
                + "; raise cache_bytes or read fewer items at once");
        }
    }
    return std::make_shared<const LeafChunk>(item, dimension, level.cellSize,
        level.indexOrigin, std::move(offsets), std::move(fields));
}

LeafChunk::LeafChunk(LeafChunkItem item, int dimension, Real3 cellSize,
    Real3 indexOrigin, std::vector<std::uint32_t> offsets,
    std::vector<AnalysisArray> fields)
    : m_item(std::move(item))
    , m_dimension(dimension)
    , m_cellSize(cellSize)
    , m_indexOrigin(indexOrigin)
    , m_offsets(std::move(offsets))
    , m_fields(std::move(fields))
{
}

AnalysisValueType LeafChunk::dtype() const noexcept
{
    return m_item.selection ? m_item.selection->dtype
                            : AnalysisValueType::Float64;
}

const std::vector<std::string>& LeafChunk::fieldNames() const noexcept
{
    static const std::vector<std::string> none;
    return m_item.selection ? m_item.selection->fieldNames : none;
}

const AnalysisArray& LeafChunk::field(std::size_t index) const
{
    if (index >= m_fields.size()) {
        throw AnalysisError("field index is out of range");
    }
    return m_fields[index];
}

const AnalysisArray& LeafChunk::field(const std::string& name) const
{
    const auto& names = fieldNames();
    const auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) {
        throw AnalysisError("field '" + name
            + "' was not requested by this chunk's plan");
    }
    return m_fields[static_cast<std::size_t>(
        std::distance(names.begin(), found))];
}

void LeafChunk::checkAxis(int axis) const
{
    if (axis < 0 || axis >= m_dimension) {
        throw AnalysisError("axis " + std::to_string(axis)
            + " is not one of this " + std::to_string(m_dimension)
            + "-D plotfile's axes");
    }
}

const std::vector<std::int32_t>& LeafChunk::index(int axis) const
{
    checkAxis(axis);
    const auto a = static_cast<std::size_t>(axis);
    std::scoped_lock lock(m_geometryMutex);
    if (!m_index[a]) {
        std::array<std::uint32_t, 3> extent{1, 1, 1};
        for (int d = 0; d < m_dimension; ++d) {
            const auto i = static_cast<std::size_t>(d);
            extent[i] = static_cast<std::uint32_t>(
                m_item.box.upper[i] - m_item.box.lower[i] + 1);
        }
        std::uint32_t divisor = 1;
        for (std::size_t d = 0; d < a; ++d) {
            divisor *= extent[d];
        }
        std::vector<std::int32_t> values(m_offsets.size());
        for (std::size_t cell = 0; cell < m_offsets.size(); ++cell) {
            values[cell] = m_item.box.lower[a]
                + static_cast<std::int32_t>(
                    (m_offsets[cell] / divisor) % extent[a]);
        }
        m_index[a] = std::move(values);
    }
    return *m_index[a];
}

const std::vector<double>& LeafChunk::center(int axis) const
{
    const auto& indices = index(axis);
    const auto a = static_cast<std::size_t>(axis);
    std::scoped_lock lock(m_geometryMutex);
    if (!m_center[a]) {
        std::vector<double> values(indices.size());
        for (std::size_t cell = 0; cell < indices.size(); ++cell) {
            values[cell] = cellCenter(m_indexOrigin, m_cellSize, axis,
                indices[cell]);
        }
        m_center[a] = std::move(values);
    }
    return *m_center[a];
}

const std::vector<double>& LeafChunk::width(int axis) const
{
    checkAxis(axis);
    const auto a = static_cast<std::size_t>(axis);
    std::scoped_lock lock(m_geometryMutex);
    if (!m_width[a]) {
        m_width[a] = std::vector<double>(m_offsets.size(), m_cellSize[a]);
    }
    return *m_width[a];
}

const std::vector<double>& LeafChunk::cellVolume() const
{
    std::scoped_lock lock(m_geometryMutex);
    if (!m_volume) {
        double volume = 1.0;
        for (int d = 0; d < m_dimension; ++d) {
            volume *= m_cellSize[static_cast<std::size_t>(d)];
        }
        m_volume = std::vector<double>(m_offsets.size(), volume);
    }
    return *m_volume;
}

} // namespace amrvis
