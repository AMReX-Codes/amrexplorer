#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/StopToken.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// Exact traversal of a plotfile's leaf cells, for analysis rather than
// display. A plan partitions the leaf cells -- those no finer level covers --
// of a region into work items, one per grid (FAB) that has any; reading an
// item copies its selected cells' native values into arrays the chunk owns.
// Nothing here resamples: a value in a chunk is a value the plotfile stores,
// or a derived field evaluated on those values, widened or narrowed only to
// the plan's dtype.

namespace amrvis {

class PlotfileDataset;

// An analysis request the dataset cannot honour: an unknown field, an
// unsupported geometry or centering, an invalid region, an item for another
// dataset, or one whose blocks could not fit in the cache budget.
class AnalysisError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

enum class AnalysisValueType : std::uint8_t {
    Float32,
    Float64
};

// What a plan selects. Fields are the plotfile's exact names (or a derived
// field's), each a single component.
struct LeafChunkRequest {
    std::vector<std::string> fields;
    // A physical region, selecting the leaf cells whose centers lie in it:
    // lower <= center < upper on every axis the plotfile has, so adjacent
    // regions partition the cells between them. Nullopt: the whole domain.
    std::optional<RealBox> region;
    // Nullopt: the widest precision any level stores.
    std::optional<AnalysisValueType> dtype;
};

// What a plan holds for every item: the fields, their dtype, and the region.
struct LeafChunkSelection {
    DatasetId dataset;
    std::vector<std::string> fieldNames;
    std::vector<FieldId> fields;
    std::optional<RealBox> region;
    AnalysisValueType dtype = AnalysisValueType::Float64;
};

// One unit of work: exactly one grid's selected leaf cells. Immutable, and
// self-contained -- reading it needs the dataset it was planned on and
// nothing about the plan's other items or the order they are read in.
struct LeafChunkItem {
    // Position in the plan, which is the canonical leaf-cell order: levels
    // coarsest first, then grids in the order the level header lists them.
    std::size_t index = 0;
    int level = 0;
    int grid = 0;
    // The grid's valid box, without stored ghost cells.
    IntBox box;
    // The part of `box` the region selects, by cell center; equal to `box`
    // for a full-domain plan.
    IntBox selectedBox;
    // The next finer level's grids, coarsened to this level and clipped to
    // selectedBox: the cells under them are covered, not leaves. Proper
    // nesting means no other level can cover a cell this list does not.
    std::vector<IntBox> covered;
    std::uint64_t selectedCells = 0;
    // The planning-time upper bound on what reading this item pins in the
    // block cache (see estimateLeafChunkPinnedBytes).
    std::uint64_t estimatedPinnedBytes = 0;
    std::shared_ptr<const LeafChunkSelection> selection;
};

struct LeafChunkPlan {
    std::shared_ptr<const LeafChunkSelection> selection;
    std::vector<LeafChunkItem> items;
    std::uint64_t totalCells = 0;
};

using AnalysisArray = std::variant<std::vector<float>, std::vector<double>>;

// One item's cells, copied out of the block cache: a chunk holds no cache
// pins and outlives the dataset it was read from. Every array is a 1-D vector
// over the selected leaf cells in canonical order (AMReX FAB order, x
// fastest). Geometry arrays are computed on first use and kept.
class LeafChunk {
public:
    LeafChunk(LeafChunkItem item, int dimension, Real3 cellSize,
        Real3 indexOrigin, std::vector<std::uint32_t> offsets,
        std::vector<AnalysisArray> fields);

    [[nodiscard]] const LeafChunkItem& item() const noexcept { return m_item; }
    [[nodiscard]] int dimension() const noexcept { return m_dimension; }
    [[nodiscard]] std::size_t size() const noexcept { return m_offsets.size(); }
    [[nodiscard]] const Real3& cellSize() const noexcept { return m_cellSize; }
    // The physical position of index zero's lower face: cell i spans
    // [indexOrigin + i dx, indexOrigin + (i + 1) dx).
    [[nodiscard]] const Real3& indexOrigin() const noexcept
    {
        return m_indexOrigin;
    }
    [[nodiscard]] AnalysisValueType dtype() const noexcept;
    [[nodiscard]] const std::vector<std::string>& fieldNames() const noexcept;
    // The selected cells' offsets into the item's valid box, x fastest.
    [[nodiscard]] const std::vector<std::uint32_t>& offsets() const noexcept
    {
        return m_offsets;
    }
    // In the order the plan listed its fields.
    [[nodiscard]] const AnalysisArray& field(std::size_t index) const;
    // Throws AnalysisError for a name the plan did not request.
    [[nodiscard]] const AnalysisArray& field(const std::string& name) const;

    // Per-axis arrays; `axis` must be one the plotfile has.
    [[nodiscard]] const std::vector<std::int32_t>& index(int axis) const;
    [[nodiscard]] const std::vector<double>& center(int axis) const;
    [[nodiscard]] const std::vector<double>& width(int axis) const;
    // The product of the cell widths over the plotfile's dimensions.
    [[nodiscard]] const std::vector<double>& cellVolume() const;

private:
    void checkAxis(int axis) const;

    LeafChunkItem m_item;
    int m_dimension = 0;
    Real3 m_cellSize;
    Real3 m_indexOrigin;
    std::vector<std::uint32_t> m_offsets;
    std::vector<AnalysisArray> m_fields;

    // Lazily built; the mutex makes first use safe from any thread.
    mutable std::mutex m_geometryMutex;
    mutable std::array<std::optional<std::vector<std::int32_t>>, 3> m_index;
    mutable std::array<std::optional<std::vector<double>>, 3> m_center;
    mutable std::array<std::optional<std::vector<double>>, 3> m_width;
    mutable std::optional<std::vector<double>> m_volume;
};

// Per-block allowance for cache bookkeeping in the planning estimate.
inline constexpr std::uint64_t leafChunkBlockAllowanceBytes = 4096;

// The precision one level stores: its VisMF RealDescriptor, or, for a
// version-1 header that has none, the first grid's FAB header.
[[nodiscard]] AnalysisValueType storedValueType(
    const PlotfileDataset& dataset, int level);

// The widest precision any level of the dataset stores.
[[nodiscard]] AnalysisValueType widestStoredValueType(
    const PlotfileDataset& dataset);

// Throws AnalysisError when the dataset's geometry or the request is not one
// leaf traversal supports, or when an item's estimate exceeds the cache
// budget. Reads metadata only.
[[nodiscard]] LeafChunkPlan planLeafChunks(
    const PlotfileDataset& dataset, const LeafChunkRequest& request);

// The conservative pinned-bytes bound the planner checks: every block the
// item could touch -- each distinct stored component and each derived field,
// including derived inputs of derived fields -- at 8 bytes per cell of the
// grown on-disk box, plus leafChunkBlockAllowanceBytes each.
[[nodiscard]] std::uint64_t estimateLeafChunkPinnedBytes(
    const PlotfileDataset& dataset, const std::vector<FieldId>& fields,
    int level, int grid);

// Throws AnalysisError when the item was planned on another dataset, and
// CacheBudgetExceeded (naming the item) when its blocks do not fit.
[[nodiscard]] std::shared_ptr<const LeafChunk> readLeafChunk(
    PlotfileDataset& dataset, const LeafChunkItem& item,
    StopToken cancellation = {});

} // namespace amrvis
