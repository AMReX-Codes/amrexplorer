#pragma once

// Shared per-point block-lookup helpers for the query layer. These were
// previously copied verbatim between SliceQuery.cpp and LineQuery.cpp, with a
// third, unchecked valueOffset variant in the GUI's DatasetExtract — this is
// the one overflow-checked definition.

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace amrvis::detail {

// One cached block pinned for a query: the grid's valid box plus the cache
// handle that keeps its values resident.
struct LoadedBlock {
    IntBox validBox;
    PlotfileDataset::BlockCache::Handle data;
};

[[nodiscard]] inline bool intersects(
    const IntBox& left, const IntBox& right, int dimension)
{
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        if (left.upper[i] < right.lower[i] || right.upper[i] < left.lower[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool contains(
    const IntBox& box, const Int3& point, int dimension)
{
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        if (point[i] < box.lower[i] || point[i] > box.upper[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline int physicalToIndex(double position,
    const DatasetMetadata& metadata, const LevelMetadata& level, int axis)
{
    (void) metadata;
    return sampleIndex(level, axis, position);
}

// Offset of `point` into a FAB's component-major values (first axis
// fastest), overflow-checked end to end. `point` must lie inside `box`
// (checked via the relative-coordinate guard).
[[nodiscard]] inline std::size_t valueOffset(
    const IntBox& box, const Int3& point, int dimension)
{
    const auto extent = [&box](std::size_t axis) {
        const auto value = static_cast<std::int64_t>(box.upper[axis])
            - box.lower[axis] + 1;
        if (value <= 0) {
            throw std::overflow_error("FAB extent is not positive");
        }
        return static_cast<std::uint64_t>(value);
    };
    const auto relative = [&box, &point](std::size_t axis) {
        const auto value = static_cast<std::int64_t>(point[axis]) - box.lower[axis];
        if (value < 0) {
            throw std::overflow_error("FAB point precedes its indexed box");
        }
        return static_cast<std::uint64_t>(value);
    };
    const auto nx = extent(0);
    const auto x = relative(0);
    if (dimension == 1) {
        return static_cast<std::size_t>(x);
    }
    const auto ny = extent(1);
    const auto y = relative(1);
    if (dimension == 2) {
        if (y > (std::numeric_limits<std::uint64_t>::max() - x) / nx) {
            throw std::overflow_error("2-D FAB offset overflows");
        }
        const auto offset = x + nx * y;
        if (offset > std::numeric_limits<std::size_t>::max()) {
            throw std::overflow_error("2-D FAB offset exceeds addressable memory");
        }
        return static_cast<std::size_t>(offset);
    }
    const auto z = relative(2);
    if (z > (std::numeric_limits<std::uint64_t>::max() - y) / ny) {
        throw std::overflow_error("3-D FAB row offset overflows");
    }
    const auto row = y + ny * z;
    if (row > (std::numeric_limits<std::uint64_t>::max() - x) / nx) {
        throw std::overflow_error("3-D FAB offset overflows");
    }
    const auto offset = x + nx * row;
    if (offset > std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("3-D FAB offset exceeds addressable memory");
    }
    return static_cast<std::size_t>(offset);
}

// A uniform bin grid over one level's loaded blocks, on two chosen axes, for
// O(1)-average point->block lookup. The composited-value lookup runs once per
// output pixel (five times per pixel for linear sampling) and once per line
// sample, and a linear scan of every intersecting block per point was
// O(points * blocks) -- seconds on a full-resolution view of a block-heavy fine
// level. Blocks within an AMReX level are non-overlapping, so a point lands in
// at most one; the grid narrows the scan to the one tile the point falls in.
//
// A block that spans several tiles is listed in each, so every block covering a
// point shares that point's tile and the bucket scan sees all candidates. Scans
// run in ascending block index (buckets are filled in block order), so a
// malformed overlapping catalog resolves to the smallest index -- identical to
// the first-match order of a plain linear scan.
class BlockGrid {
public:
    BlockGrid() = default;

    BlockGrid(const std::vector<LoadedBlock>& blocks,
        const std::array<int, 2>& axes)
        : m_axis0(axes[0])
        , m_axis1(axes[1])
    {
        if (blocks.empty()) {
            return;
        }
        const auto a0 = static_cast<std::size_t>(m_axis0);
        const auto a1 = static_cast<std::size_t>(m_axis1);
        m_lo0 = std::numeric_limits<std::int64_t>::max();
        m_lo1 = std::numeric_limits<std::int64_t>::max();
        m_hi0 = std::numeric_limits<std::int64_t>::min();
        m_hi1 = std::numeric_limits<std::int64_t>::min();
        for (const auto& block : blocks) {
            m_lo0 = std::min(m_lo0,
                static_cast<std::int64_t>(block.validBox.lower[a0]));
            m_lo1 = std::min(m_lo1,
                static_cast<std::int64_t>(block.validBox.lower[a1]));
            m_hi0 = std::max(m_hi0,
                static_cast<std::int64_t>(block.validBox.upper[a0]));
            m_hi1 = std::max(m_hi1,
                static_cast<std::int64_t>(block.validBox.upper[a1]));
        }
        const auto span0 = m_hi0 - m_lo0 + 1;
        const auto span1 = m_hi1 - m_lo1 + 1;
        // ~sqrt(blocks) tiles per axis, capped so the bucket array is bounded.
        // llround(sqrt(n)) >= 1 for n >= 1 (the empty case returned above), so
        // only the upper cap is needed.
        constexpr std::int64_t maxTilesPerAxis = 256;
        const auto target = std::min<std::int64_t>(maxTilesPerAxis,
            std::llround(std::sqrt(static_cast<double>(blocks.size()))));
        m_tile0 = std::max<std::int64_t>(1, (span0 + target - 1) / target);
        m_tile1 = std::max<std::int64_t>(1, (span1 + target - 1) / target);
        m_n0 = static_cast<int>((span0 + m_tile0 - 1) / m_tile0);
        m_n1 = static_cast<int>((span1 + m_tile1 - 1) / m_tile1);
        const auto tileCount
            = static_cast<std::size_t>(m_n0) * static_cast<std::size_t>(m_n1);
        // Non-overlapping blocks (the level invariant) put ~one block in each
        // tile, so the fill is O(blocks + tiles). validateMetadata does not
        // forbid overlap, though, and a degenerate catalog of many large
        // overlapping boxes would push billions of (block, tile) entries -- an
        // unbounded, GB-scale allocation off a small header. Cap the total and
        // fall back to a plain linear scan (bounded memory, identical result)
        // rather than build an enormous index.
        const auto maxEntries = 8 * (blocks.size() + tileCount);
        m_buckets.assign(tileCount, {});
        std::size_t entries = 0;
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const auto& box = blocks[index].validBox;
            // Box coordinates are within [m_lo, m_hi], so these tile indices
            // never overflow the narrowing cast in tile0()/tile1(); the loop
            // bounds are hoisted out of the inner condition.
            const auto t0Last = tile0(box.upper[a0]);
            const auto t1Last = tile1(box.upper[a1]);
            for (int t1 = tile1(box.lower[a1]); t1 <= t1Last; ++t1) {
                for (int t0 = tile0(box.lower[a0]); t0 <= t0Last; ++t0) {
                    if (++entries > maxEntries) {
                        m_buckets.clear();
                        m_buckets.shrink_to_fit();
                        m_linearScan = true;
                        return;
                    }
                    m_buckets[bucket(t0, t1)].push_back(static_cast<int>(index));
                }
            }
        }
    }

    // Index into `blocks` of the block containing `point`, or -1 if none does.
    // `blocks` must be the same vector the grid was built from.
    [[nodiscard]] int find(const std::vector<LoadedBlock>& blocks,
        const Int3& point, int dimension) const
    {
        if (m_linearScan) {
            for (std::size_t index = 0; index < blocks.size(); ++index) {
                if (contains(blocks[index].validBox, point, dimension)) {
                    return static_cast<int>(index);
                }
            }
            return -1;
        }
        const auto p0 = static_cast<std::int64_t>(
            point[static_cast<std::size_t>(m_axis0)]);
        const auto p1 = static_cast<std::int64_t>(
            point[static_cast<std::size_t>(m_axis1)]);
        // Reject out-of-bounds points in int64 *before* tiling. A point far
        // above the bounding box would otherwise overflow the narrowing cast
        // in tile0()/tile1() to a negative int and slip past a bare upper-tile
        // check, indexing m_buckets wildly out of range. The empty-level case
        // (inverted default bounds, m_hi < m_lo) is rejected here too.
        if (p0 < m_lo0 || p0 > m_hi0 || p1 < m_lo1 || p1 > m_hi1) {
            return -1;
        }
        for (const auto index : m_buckets[bucket(tile0(p0), tile1(p1))]) {
            if (contains(blocks[static_cast<std::size_t>(index)].validBox,
                    point, dimension)) {
                return index;
            }
        }
        return -1;
    }

private:
    // Precondition: coordinate is within [m_lo, m_hi] on its axis, so the
    // quotient is in [0, m_n - 1] and the narrowing cast cannot overflow.
    [[nodiscard]] int tile0(std::int64_t coordinate) const noexcept
    {
        return static_cast<int>((coordinate - m_lo0) / m_tile0);
    }
    [[nodiscard]] int tile1(std::int64_t coordinate) const noexcept
    {
        return static_cast<int>((coordinate - m_lo1) / m_tile1);
    }
    [[nodiscard]] std::size_t bucket(int t0, int t1) const noexcept
    {
        return static_cast<std::size_t>(t1) * static_cast<std::size_t>(m_n0)
            + static_cast<std::size_t>(t0);
    }

    int m_axis0 = 0;
    int m_axis1 = 1;
    // Inverted default range so an empty grid rejects every point in find().
    std::int64_t m_lo0 = 0;
    std::int64_t m_lo1 = 0;
    std::int64_t m_hi0 = -1;
    std::int64_t m_hi1 = -1;
    std::int64_t m_tile0 = 1;
    std::int64_t m_tile1 = 1;
    int m_n0 = 0;
    int m_n1 = 0;
    bool m_linearScan = false;
    std::vector<std::vector<int>> m_buckets;
};

// The value at `point` in the finest-covering block located by `grid`, or
// nullopt when no block covers it. Throws if the covering block's FAB index is
// out of range (a corrupt block whose loaded payload is smaller than its box).
// The shared composed-sample tail for the slice and line queries; callers keep
// the point and covering level.
[[nodiscard]] inline std::optional<double> lookupBlockValue(
    const BlockGrid& grid, const std::vector<LoadedBlock>& blocks,
    const Int3& point, int dimension)
{
    const auto blockIndex = grid.find(blocks, point, dimension);
    if (blockIndex < 0) {
        return std::nullopt;
    }
    const auto& block = blocks[static_cast<std::size_t>(blockIndex)];
    const auto offset = valueOffset(block.data->box, point, dimension);
    if (offset >= block.data->values.size()) {
        throw std::runtime_error("composed FAB index exceeds loaded block");
    }
    return block.data->values[offset];
}

} // namespace amrvis::detail
