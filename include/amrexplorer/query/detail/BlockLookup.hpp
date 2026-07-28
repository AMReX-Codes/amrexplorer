#pragma once

// Shared per-point block-lookup helpers for the query layer. These were
// previously copied verbatim between SliceQuery.cpp and LineQuery.cpp, with a
// third, unchecked valueOffset variant in the GUI's DatasetExtract — this is
// the one overflow-checked definition.

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

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

} // namespace amrvis::detail
