#pragma once

// Shared per-point block-lookup helpers for the query layer: the one
// overflow-checked valueOffset, and the point->block grid the slice and line
// queries both index their loaded blocks with.

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace amrvis::detail {

// One cached block pinned for a query: the grid's valid box plus the cache
// handle that keeps its values resident. BlockGrid reads only the box, so a
// grid can be built and searched over blocks with no payload; lookupBlockValue
// needs the handle and rejects a null one.
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

// A uniform bin grid over one level's loaded blocks, on one or two chosen
// axes, for O(1)-average point->block lookup. The composited-value lookup runs
// once per output pixel (five times per pixel for linear sampling) and once per
// line sample, and a linear scan of every intersecting block per point was
// O(points * blocks) -- seconds on a full-resolution view of a block-heavy fine
// level. Blocks within an AMReX level are non-overlapping, so a point lands in
// at most one; the grid narrows the scan to the one tile the point falls in.
//
// Tiles are sized from the blocks themselves: each axis's tile is the median
// block extent on that axis, so a block spans about one tile whatever its
// aspect ratio -- a 1x1000 pencil decomposition gets 1x1000 tiles, not the
// square tiles that would list every pencil in dozens of buckets. Only the
// tile count is capped (a few per block), so sparse levels coarsen their tiles
// instead of allocating a bucket per empty cell.
//
// That cap is also what bounds the work per lookup on any *non-overlapping*
// layout (the level invariant), clustered ones included: such blocks each
// take about one (block, tile) entry, and with at least a quarter as many
// tiles as blocks the candidates scanned *averaged over points spread across
// the bounding box* -- which is what a slice's pixels and a line's samples
// are, since a query indexes only the blocks meeting its visible region --
// come to blocks/tiles, at most a few. A level of two refined regions far
// apart does coarsen to dozens of blocks per occupied bucket, but only the
// corresponding fraction of points lands in them; an index that keeps
// block-sized tiles there (hashed sparse tiles behind a coarse occupancy map)
// measured within noise of this one end to end. Overlapping catalogs get no
// such bound -- identical boxes all share one tile -- only the memory cap.
//
// A block that spans several tiles is listed in each, so every block covering a
// point shares that point's tile and the bucket scan sees all candidates. Scans
// run in ascending block index (buckets are filled in block order), so a
// malformed overlapping catalog resolves to the smallest index -- identical to
// the first-match order of a plain linear scan.
//
// Buckets are one flat CSR array (per-tile offsets into one index array): two
// allocations per build, not one per tile, since a build runs on every query.
class BlockGrid {
public:
    BlockGrid() = default;

    // Bin on the one axis a query varies. The other coordinates of a query
    // point are then pinned, and binning one of them would only list every
    // block in every tile of that axis (they all straddle the pinned cell).
    BlockGrid(const std::vector<LoadedBlock>& blocks, int axis)
        : BlockGrid(blocks, std::array<int, 2>{axis, axis})
    {}

    // Bin on two axes (a slice's plane axes). Equal axes bin on that one axis.
    BlockGrid(const std::vector<LoadedBlock>& blocks,
        const std::array<int, 2>& axes)
        : m_axis0(axes[0])
        , m_axis1(axes[1])
    {
        m_blockCount = blocks.size();
        const bool singleAxis = m_axis0 == m_axis1;
        const auto a0 = static_cast<std::size_t>(m_axis0);
        const auto a1 = static_cast<std::size_t>(m_axis1);
        // A box inverted on a binned axis contains no point (contains() can
        // never match it), so it takes no part in the index: not in the
        // bounds, the tile sizing, or either CSR pass -- the one predicate
        // every pass shares, so the count and the fill cannot disagree. (The
        // readers reject inverted boxes long before this, but the class is a
        // public one, and a negative extent here would wrap the entry count
        // and under-allocate the fill.)
        const auto indexable = [a0, a1](const IntBox& box) {
            return box.lower[a0] <= box.upper[a0]
                && box.lower[a1] <= box.upper[a1];
        };
        m_lo0 = std::numeric_limits<std::int64_t>::max();
        m_lo1 = std::numeric_limits<std::int64_t>::max();
        m_hi0 = std::numeric_limits<std::int64_t>::min();
        m_hi1 = std::numeric_limits<std::int64_t>::min();
        std::vector<std::int64_t> extents0;
        std::vector<std::int64_t> extents1;
        extents0.reserve(blocks.size());
        extents1.reserve(blocks.size());
        for (const auto& block : blocks) {
            const auto& box = block.validBox;
            if (!indexable(box)) {
                continue;
            }
            m_lo0 = std::min(m_lo0, static_cast<std::int64_t>(box.lower[a0]));
            m_lo1 = std::min(m_lo1, static_cast<std::int64_t>(box.lower[a1]));
            m_hi0 = std::max(m_hi0, static_cast<std::int64_t>(box.upper[a0]));
            m_hi1 = std::max(m_hi1, static_cast<std::int64_t>(box.upper[a1]));
            extents0.push_back(
                static_cast<std::int64_t>(box.upper[a0]) - box.lower[a0] + 1);
            extents1.push_back(
                static_cast<std::int64_t>(box.upper[a1]) - box.lower[a1] + 1);
        }
        if (extents0.empty()) {
            return;  // nothing indexable: the inverted default bounds reject all
        }
        const auto span0 = m_hi0 - m_lo0 + 1;
        const auto span1 = m_hi1 - m_lo1 + 1;
        // Tiles of the median block extent per axis (at least 1): the size
        // that puts a typical block in about one tile.
        m_tile0 = medianOf(extents0);
        m_tile1 = singleAxis ? span1 : medianOf(extents1);
        // Cap the tile count at a few per block: a sparse level (few blocks in
        // a large span) would otherwise get a bucket per block-sized cell of
        // empty space. Coarsen by doubling the tiles of the axes that still
        // have more than one, so the aspect ratio holds and an axis already
        // down to one tile is left alone; the product is compared by division
        // because two 32-bit spans of 1-cell tiles would overflow it.
        const auto maxTiles
            = static_cast<std::int64_t>(4 * blocks.size() + 64);
        auto n0 = (span0 + m_tile0 - 1) / m_tile0;
        auto n1 = (span1 + m_tile1 - 1) / m_tile1;
        // Terminates when both tiles reach their spans (one tile each); that
        // takes at most ~33 doublings for int32 spans, so the bound below is
        // slack -- it turns a future edit that stops one axis from coarsening
        // into a merely coarse index instead of a hang on the query thread.
        for (int guard = 0; guard < 64 && n0 > maxTiles / n1; ++guard) {
            m_tile0 = std::min(span0, m_tile0 * 2);
            m_tile1 = std::min(span1, m_tile1 * 2);
            n0 = (span0 + m_tile0 - 1) / m_tile0;
            n1 = (span1 + m_tile1 - 1) / m_tile1;
        }
        if (n0 > maxTiles / n1) {
            // Only reachable if the loop above stops coarsening an axis; the
            // bucket array below relies on the cap, so scan instead.
            m_linearScan = true;
            return;
        }
        m_n0 = static_cast<int>(n0);
        m_n1 = static_cast<int>(n1);
        const auto tileCount
            = static_cast<std::size_t>(m_n0) * static_cast<std::size_t>(m_n1);
        // With tiles sized to the blocks, a non-overlapping level puts each
        // block in a handful of buckets and the fill is O(blocks + tiles).
        // validateMetadata does not forbid overlap, though, and a degenerate
        // catalog of many large overlapping boxes would push millions of
        // (block, tile) entries -- an unbounded allocation off a small header.
        // Cap the total and fall back to a plain linear scan (bounded memory,
        // identical result) rather than build an enormous index.
        const auto maxEntries = 8 * (blocks.size() + tileCount);
        // CSR build: count per tile, prefix-sum into offsets, then fill.
        // Indexable box coordinates are within [m_lo, m_hi], so the tile
        // indices never overflow the narrowing cast in tile0()/tile1().
        std::vector<std::uint32_t> counts(tileCount, 0);
        std::size_t entries = 0;
        for (const auto& block : blocks) {
            const auto& box = block.validBox;
            if (!indexable(box)) {
                continue;
            }
            const auto t0First = tile0(box.lower[a0]);
            const auto t0Last = tile0(box.upper[a0]);
            const auto t1First = tile1(box.lower[a1]);
            const auto t1Last = tile1(box.upper[a1]);
            entries += static_cast<std::size_t>(t0Last - t0First + 1)
                * static_cast<std::size_t>(t1Last - t1First + 1);
            if (entries > maxEntries) {
                m_linearScan = true;
                return;
            }
            for (int t1 = t1First; t1 <= t1Last; ++t1) {
                for (int t0 = t0First; t0 <= t0Last; ++t0) {
                    ++counts[bucket(t0, t1)];
                }
            }
        }
        m_offsets.assign(tileCount + 1, 0);
        for (std::size_t tile = 0; tile < tileCount; ++tile) {
            m_offsets[tile + 1] = m_offsets[tile] + counts[tile];
        }
        m_indices.resize(entries);
        // Reuse `counts` as the per-tile fill cursor; ascending block index
        // within a bucket falls out of visiting blocks in order.
        std::fill(counts.begin(), counts.end(), 0U);
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const auto& box = blocks[index].validBox;
            if (!indexable(box)) {
                continue;
            }
            const auto t0Last = tile0(box.upper[a0]);
            const auto t1Last = tile1(box.upper[a1]);
            for (int t1 = tile1(box.lower[a1]); t1 <= t1Last; ++t1) {
                for (int t0 = tile0(box.lower[a0]); t0 <= t0Last; ++t0) {
                    const auto tile = bucket(t0, t1);
                    m_indices[m_offsets[tile] + counts[tile]++]
                        = static_cast<int>(index);
                }
            }
        }
    }

    // Whether lookups go through the index (false after the overlap fallback,
    // where find() is the linear scan the index replaces). For tests and
    // diagnostics; the result is the same either way.
    [[nodiscard]] bool usesIndex() const noexcept { return !m_linearScan; }

    // Index into `blocks` of the block containing `point`, or -1 if none does.
    // `blocks` must be the same vector the grid was built from: the grid holds
    // indices into it, so a different vector would be read through stale
    // ones (IndexedBlocks below keeps the two together). The size check is the
    // cheap part of that contract that can be enforced.
    [[nodiscard]] int find(const std::vector<LoadedBlock>& blocks,
        const Int3& point, int dimension) const
    {
        if (blocks.size() != m_blockCount) {
            throw std::logic_error(
                "BlockGrid::find given a block set other than its own");
        }
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
        // check, indexing the buckets wildly out of range. The empty-level
        // case (inverted default bounds, m_hi < m_lo) is rejected here too.
        if (p0 < m_lo0 || p0 > m_hi0 || p1 < m_lo1 || p1 > m_hi1) {
            return -1;
        }
        const auto tile = bucket(tile0(p0), tile1(p1));
        for (auto entry = m_offsets[tile]; entry < m_offsets[tile + 1];
             ++entry) {
            const auto index = m_indices[entry];
            if (contains(blocks[static_cast<std::size_t>(index)].validBox,
                    point, dimension)) {
                return index;
            }
        }
        return -1;
    }

private:
    // The median of a non-empty list of positive extents (reordered in place).
    [[nodiscard]] static std::int64_t medianOf(std::vector<std::int64_t>& extents)
    {
        const auto middle = extents.begin()
            + static_cast<std::ptrdiff_t>(extents.size() / 2);
        std::nth_element(extents.begin(), middle, extents.end());
        return std::max<std::int64_t>(1, *middle);
    }

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
    std::size_t m_blockCount = 0;
    std::vector<std::size_t> m_offsets;  // per tile: first entry in m_indices
    std::vector<int> m_indices;          // block indices, bucket by bucket
};

// One level's loaded blocks together with the point->block grid over them.
// The grid stores indices into `blocks`, so the two travel as one value:
// build it from the loaded vector and query it through lookupBlockValue.
struct IndexedBlocks {
    IndexedBlocks() = default;

    IndexedBlocks(std::vector<LoadedBlock> loaded, int axis)
        : blocks(std::move(loaded))
        , grid(blocks, axis)
    {}

    IndexedBlocks(
        std::vector<LoadedBlock> loaded, const std::array<int, 2>& axes)
        : blocks(std::move(loaded))
        , grid(blocks, axes)
    {}

    std::vector<LoadedBlock> blocks;
    BlockGrid grid;
};

// The value at `point` in the block of one level's `indexed` blocks that
// covers it, or nullopt when none does. Throws if the covering block's FAB
// index is out of range (a corrupt block whose loaded payload is smaller than
// its box). The shared composed-sample tail for the slice and line queries;
// the caller walks the levels finest first and keeps the point and covering
// level.
[[nodiscard]] inline std::optional<double> lookupBlockValue(
    const IndexedBlocks& indexed, const Int3& point, int dimension)
{
    const auto blockIndex = indexed.grid.find(indexed.blocks, point, dimension);
    if (blockIndex < 0) {
        return std::nullopt;
    }
    const auto& block = indexed.blocks[static_cast<std::size_t>(blockIndex)];
    if (!block.data) {
        throw std::logic_error("covering block has no loaded payload");
    }
    // The point was found in the catalog's valid box; the offset is computed
    // in the FAB's own header box. Nothing upstream cross-checks the two, and
    // a disagreement aliases into the wrong cell without leaving the payload
    // (which is sized to the FAB box exactly), so containment -- not payload
    // size -- is the guard.
    if (!contains(block.data->box, point, dimension)) {
        throw std::runtime_error("composed FAB does not cover its catalog box");
    }
    return block.data->values[valueOffset(block.data->box, point, dimension)];
}

} // namespace amrvis::detail
