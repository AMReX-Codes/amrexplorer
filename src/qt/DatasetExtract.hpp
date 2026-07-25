#pragma once

// Raw per-level cell-value extraction behind the dataset (spreadsheet)
// window. Kept free of Qt types so it can be exercised without the GUI.

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace amrvis::qt {

// Hard cap on the extracted in-plane extent per axis; the dataset window
// truncates larger regions instead of building million-cell tables.
constexpr int datasetExtractMaxExtent = 512;

// Raw values of one field at one AMR level over a region. values/covered
// run over the in-plane index box with the first in-plane axis fastest; a
// cell no grid covers at this level keeps covered == 0 and value 0.
struct DatasetLevelExtract {
    std::array<int, 2> lower{0, 0};    // in-plane index box, level index space
    std::array<int, 2> upper{-1, -1};
    int nx = 0;                        // extent along in-plane axis 0
    int ny = 0;                        // extent along in-plane axis 1
    int sliceIndex = 0;                // normal-axis cell index (3-D)
    std::vector<float> values;
    std::vector<std::uint8_t> covered;
    double minimum = 0.0;              // over finite covered cells only
    double maximum = 0.0;
    bool hasFiniteValues = false;       // minimum/maximum are meaningful
    bool truncatedX = false;           // region exceeded maxExtent, axis 0
    bool truncatedY = false;           // region exceeded maxExtent, axis 1
};

namespace dataset_extract_detail {

// The two in-plane axes for a view normal, in ascending order (2-D: x, y).
inline std::array<int, 2> inPlaneAxes(int dimension, int normalAxis) noexcept
{
    if (dimension != 3) {
        return {0, 1};
    }
    std::array<int, 2> axes{0, 1};
    std::size_t next = 0;
    for (int axis = 0; axis < 3; ++axis) {
        if (axis != normalAxis) {
            axes[next++] = axis;
        }
    }
    return axes;
}

inline bool intersects(const IntBox& left, const IntBox& right,
    int dimension) noexcept
{
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        if (left.upper[i] < right.lower[i] || right.upper[i] < left.lower[i]) {
            return false;
        }
    }
    return true;
}

// Offset into a FAB's component-major values (first axis fastest); the point
// must lie inside box. Mirrors SliceQuery's valueOffset without the overflow
// checks: the caller only passes points of the intersected valid box.
inline std::size_t fabValueOffset(const IntBox& box, int i, int j, int k,
    int dimension) noexcept
{
    const auto extent = [&box](std::size_t axis) {
        return static_cast<std::uint64_t>(
            static_cast<std::int64_t>(box.upper[axis]) - box.lower[axis] + 1);
    };
    const auto nx = extent(0);
    const auto x = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(i) - box.lower[0]);
    if (dimension == 1) {
        return static_cast<std::size_t>(x);
    }
    const auto ny = extent(1);
    const auto y = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(j) - box.lower[1]);
    if (dimension == 2) {
        return static_cast<std::size_t>(x + nx * y);
    }
    const auto z = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(k) - box.lower[2]);
    return static_cast<std::size_t>(x + nx * (y + ny * z));
}

} // namespace dataset_extract_detail

// Extracts the raw values of field at levelIndex over region (physical
// coordinates). For 3-D the normal axis is clamped to the cell holding
// slicePosition; for 2-D normalAxis/slicePosition are ignored. An in-plane
// extent beyond maxExtent keeps only the first maxExtent cells of that axis
// and sets the matching truncated flag. A region missing the level domain
// entirely returns an extract with nx == ny == 0. Read errors (including
// ReadCancelled) propagate to the caller.
[[nodiscard]] inline DatasetLevelExtract extractDatasetLevel(
    PlotfileDataset& dataset, FieldId field, int levelIndex,
    const RealBox& region, int normalAxis, double slicePosition,
    int maxExtent, StopToken cancellation = {})
{
    const auto& metadata = dataset.metadata();
    if (metadata.dimension < 2 || metadata.dimension > 3) {
        throw std::invalid_argument(
            "dataset extraction requires a 2-D or 3-D dataset");
    }
    if (levelIndex < 0
        || levelIndex >= static_cast<int>(metadata.levels.size())) {
        throw std::invalid_argument("dataset extraction level is unavailable");
    }
    if (field.value >= metadata.fields.size()) {
        throw std::invalid_argument("dataset extraction field is unavailable");
    }
    if (metadata.dimension == 3 && (normalAxis < 0 || normalAxis > 2)) {
        throw std::invalid_argument("dataset extraction normal axis is invalid");
    }
    if (maxExtent < 1) {
        throw std::invalid_argument(
            "dataset extraction extent cap must be positive");
    }

    const auto& level = metadata.levels[static_cast<std::size_t>(levelIndex)];
    const auto axes = dataset_extract_detail::inPlaneAxes(
        metadata.dimension, normalAxis);

    DatasetLevelExtract extract;
    // Map the physical region into this level's index space (SliceQuery's
    // floor convention, nextafter on the half-open upper edge) and clip it
    // against the level domain.
    std::array<std::int64_t, 2> lower{};
    std::array<std::int64_t, 2> upper{};
    for (std::size_t entry = 0; entry < 2; ++entry) {
        const auto axis = static_cast<std::size_t>(axes[entry]);
        const auto domainLower
            = static_cast<std::int64_t>(level.domain.lower[axis]);
        const auto domainUpper
            = static_cast<std::int64_t>(level.domain.upper[axis]);
        const auto rawLower = static_cast<std::int64_t>(
            sampleIndex(level, axes[entry], region.lower[axis]));
        const auto rawUpper = static_cast<std::int64_t>(
            sampleIndex(level, axes[entry],
                std::nextafter(region.upper[axis],
                    -std::numeric_limits<double>::infinity())));
        if (rawUpper < domainLower || rawLower > domainUpper) {
            return extract;  // region misses this level's domain entirely
        }
        lower[entry] = std::max(rawLower, domainLower);
        upper[entry] = std::min(rawUpper, domainUpper);
    }

    // Over-long axes keep only their first maxExtent cells.
    for (std::size_t entry = 0; entry < 2; ++entry) {
        if (upper[entry] - lower[entry] + 1
            > static_cast<std::int64_t>(maxExtent)) {
            upper[entry] = lower[entry] + maxExtent - 1;
            if (entry == 0) {
                extract.truncatedX = true;
            } else {
                extract.truncatedY = true;
            }
        }
        extract.lower[entry] = static_cast<int>(lower[entry]);
        extract.upper[entry] = static_cast<int>(upper[entry]);
    }
    extract.nx = static_cast<int>(upper[0] - lower[0] + 1);
    extract.ny = static_cast<int>(upper[1] - lower[1] + 1);

    if (metadata.dimension == 3) {
        const auto normal = static_cast<std::size_t>(normalAxis);
        const auto domainLower
            = static_cast<std::int64_t>(level.domain.lower[normal]);
        const auto domainUpper
            = static_cast<std::int64_t>(level.domain.upper[normal]);
        const auto raw = static_cast<std::int64_t>(
            sampleIndex(level, normalAxis, slicePosition));
        extract.sliceIndex
            = static_cast<int>(std::clamp(raw, domainLower, domainUpper));
    }

    auto query = level.domain;
    for (std::size_t entry = 0; entry < 2; ++entry) {
        const auto axis = static_cast<std::size_t>(axes[entry]);
        query.lower[axis] = extract.lower[entry];
        query.upper[axis] = extract.upper[entry];
    }
    if (metadata.dimension == 3) {
        const auto normal = static_cast<std::size_t>(normalAxis);
        query.lower[normal] = extract.sliceIndex;
        query.upper[normal] = extract.sliceIndex;
    }

    const auto cellCount = static_cast<std::size_t>(extract.nx)
        * static_cast<std::size_t>(extract.ny);
    extract.values.assign(cellCount, 0.0F);
    extract.covered.assign(cellCount, std::uint8_t{0});

    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    auto minimum = std::numeric_limits<double>::infinity();
    auto maximum = -std::numeric_limits<double>::infinity();
    for (std::size_t grid = 0; grid < level.blocks.size(); ++grid) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        // Coverage follows the grid's valid box; the FAB payload may carry
        // ghost cells, so value offsets use the FAB's own box.
        const auto validBox = level.blocks[grid].box;
        if (!dataset_extract_detail::intersects(
                validBox, query, metadata.dimension)) {
            continue;
        }
        BlockRequest request;
        request.dataset = dataset.id();
        request.level = levelIndex;
        request.gridIndex = static_cast<int>(grid);
        request.field = field;
        request.firstComponent = 0;
        request.componentCount = 1;
        const auto access = dataset.requestBlock(request, cancellation);
        const auto& fab = *access.handle;

        const auto iLower = std::max(validBox.lower[xAxis], extract.lower[0]);
        const auto iUpper = std::min(validBox.upper[xAxis], extract.upper[0]);
        const auto jLower = std::max(validBox.lower[yAxis], extract.lower[1]);
        const auto jUpper = std::min(validBox.upper[yAxis], extract.upper[1]);
        const auto k = metadata.dimension == 3 ? extract.sliceIndex : 0;
        for (auto j = jLower; j <= jUpper; ++j) {
            if (cancellation.stop_requested()) {
                throw ReadCancelled();
            }
            const auto valueY = static_cast<std::size_t>(
                static_cast<std::int64_t>(j) - extract.lower[1]);
            for (auto i = iLower; i <= iUpper; ++i) {
                const auto value = fab.values[
                    dataset_extract_detail::fabValueOffset(
                        fab.box, i, j, k, metadata.dimension)];
                const auto valueX = static_cast<std::size_t>(
                    static_cast<std::int64_t>(i) - extract.lower[0]);
                const auto offset = valueX
                    + static_cast<std::size_t>(extract.nx) * valueY;
                extract.values[offset] = static_cast<float>(value);
                extract.covered[offset] = std::uint8_t{1};
                if (std::isfinite(value)) {
                    minimum = std::min(minimum, value);
                    maximum = std::max(maximum, value);
                }
            }
        }
    }
    if (std::isfinite(minimum) && std::isfinite(maximum)) {
        extract.minimum = minimum;
        extract.maximum = maximum;
        extract.hasFiniteValues = true;
    }
    return extract;
}

} // namespace amrvis::qt
