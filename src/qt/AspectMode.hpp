#pragma once

#include <amrexplorer/core/Metadata.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

// How a slice panel's two axes are proportioned on screen. Qt-free so the
// helper below can be unit-tested without a QApplication.
namespace amrvis::qt {

enum class AspectMode : int {
    // One square screen pixel per finest cell: the panel's aspect is the
    // region's extent in cells. The historical default.
    CellCounts = 0,
    // Each axis is stretched by its finest cell size, so the panel's aspect is
    // the region's physical aspect.
    PhysicalSize = 1,
};

// The display stretch of each dataset axis: the user's factor times, in
// PhysicalSize mode, the finest level's cell size. The result is normalized
// so the smallest factor over the dataset's dimensions is one, which makes a
// fixed scale N mean N screen pixels per cell along the least stretched axis.
// Physical proportion is skipped when the dataset has no physical geometry
// (standalone FABs and MultiFabs carry unit cells anyway) and for 2-D
// spherical data, where the R-Z warp is already physical and r and theta do
// not share a unit. Non-positive or non-finite inputs count as one.
[[nodiscard]] inline std::array<double, 3> displayStretchPerAxis(
    const DatasetMetadata& metadata, AspectMode mode,
    const std::array<double, 3>& axisScale, bool spherical)
{
    const auto sane = [](double value) {
        return std::isfinite(value) && value > 0.0 ? value : 1.0;
    };
    const bool physical = mode == AspectMode::PhysicalSize
        && metadata.hasPhysicalGeometry && !spherical
        && !metadata.levels.empty();
    // Indexed only once levels is known non-empty (physical implies it).
    const LevelMetadata* finest = physical
        ? &metadata.levels[static_cast<std::size_t>(
            std::clamp(metadata.finestLevel, 0,
                static_cast<int>(metadata.levels.size()) - 1))]
        : nullptr;
    const auto dimension = static_cast<std::size_t>(
        std::clamp(metadata.dimension, 1, 3));
    std::array<double, 3> stretch{1.0, 1.0, 1.0};
    double smallest = std::numeric_limits<double>::infinity();
    for (std::size_t axis = 0; axis < dimension; ++axis) {
        auto factor = sane(axisScale[axis]);
        if (finest != nullptr) {
            factor *= sane(finest->cellSize[axis]);
        }
        stretch[axis] = factor;
        smallest = std::min(smallest, factor);
    }
    if (std::isfinite(smallest) && smallest > 0.0) {
        for (std::size_t axis = 0; axis < dimension; ++axis) {
            stretch[axis] /= smallest;
        }
    }
    return stretch;
}

// Whether the screen shows the same number of pixels per physical unit along
// every dataset axis, which is what a single horizontal scale bar needs to
// be truthful. In CellCounts mode with unit factors this is "the finest cells
// are square"; PhysicalSize mode with equal factors always qualifies.
[[nodiscard]] inline bool displayIsPhysicallyIsotropic(
    const DatasetMetadata& metadata, const std::array<double, 3>& stretch)
{
    if (!metadata.hasPhysicalGeometry || metadata.levels.empty()) {
        return false;
    }
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::clamp(metadata.finestLevel, 0,
            static_cast<int>(metadata.levels.size()) - 1))];
    const auto dimension = static_cast<std::size_t>(
        std::clamp(metadata.dimension, 1, 3));
    const auto density = [&](std::size_t axis) {
        return stretch[axis] / finest.cellSize[axis];
    };
    const auto reference = density(0);
    if (!std::isfinite(reference) || !(reference > 0.0)) {
        return false;
    }
    for (std::size_t axis = 1; axis < dimension; ++axis) {
        if (std::abs(density(axis) - reference) > 1.0e-9 * reference) {
            return false;
        }
    }
    return true;
}

} // namespace amrvis::qt
