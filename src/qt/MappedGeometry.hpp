#pragma once

#include "PairGeometry.hpp"

#include <amrexplorer/core/Geometry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>

// A slice drawn on its mapped (stretched) grid: how the panel places the
// warped raster in its scene. Qt-free so the unit test needs no QApplication.
namespace amrvis::qt {

// Scene units are display units (a fixed scale N is N screen pixels per
// scene unit), physical lengths times the axis-scale factors, normalized so
// the dataset's tightest finest cell at unit factor is one scene unit -- the
// rule displayStretchFor applies to a Cartesian raster. The scene is
// anchored at the canvas: the node bounding box of the whole domain on the
// panel's axes, with scene y counting down from its top. The layout depends
// only on that box and the axis factors, never on the current zoom, so pan
// and zoom never move the tile, and a raster drawn for any window lands at
// its physical place -- the one-layer sibling of PairLayout.
class MappedLayout {
public:
    MappedLayout() = default;
    MappedLayout(const RealBox& canvasBounds, std::array<int, 2> axes,
        const std::array<double, 3>& axisScale, const Real3& finestCellSize,
        int dimension)
        : m_bounds(canvasBounds)
        , m_axes(axes)
    {
        const auto sane = [](double value) {
            return std::isfinite(value) && value > 0.0 ? value : 1.0;
        };
        for (std::size_t axis = 0; axis < 3; ++axis) {
            m_unitsPerLength[axis] = sane(axisScale[axis]);
        }
        // Normalize over the dataset's axes (not the panel's two) so every
        // panel shows an axis at the same pixels per length at a fixed scale.
        double smallest = std::numeric_limits<double>::infinity();
        const auto shown = static_cast<std::size_t>(std::clamp(dimension, 2, 3));
        for (std::size_t axis = 0; axis < shown; ++axis) {
            const auto perCell = m_unitsPerLength[axis] * finestCellSize[axis];
            if (std::isfinite(perCell) && perCell > 0.0) {
                smallest = std::min(smallest, perCell);
            }
        }
        if (std::isfinite(smallest) && smallest > 0.0) {
            for (auto& value : m_unitsPerLength.values) {
                value /= smallest;
            }
        }
    }

    [[nodiscard]] const RealBox& bounds() const noexcept { return m_bounds; }
    [[nodiscard]] const std::array<int, 2>& axes() const noexcept { return m_axes; }
    [[nodiscard]] double unitsPerLength(int axis) const noexcept
    {
        return m_unitsPerLength[static_cast<std::size_t>(axis)];
    }

    // Scene coordinate of a physical position along a displayed axis.
    [[nodiscard]] double sceneFromPhysical(int axis, double position) const noexcept
    {
        const auto a = static_cast<std::size_t>(axis);
        const auto k = m_unitsPerLength[a];
        // Vertical scene coordinates count down from the canvas top.
        return axis == m_axes[1] ? (m_bounds.upper[a] - position) * k
                                 : (position - m_bounds.lower[a]) * k;
    }

    // sceneFromPhysical's inverse.
    [[nodiscard]] double physicalFromScene(int axis, double scene) const noexcept
    {
        const auto a = static_cast<std::size_t>(axis);
        const auto k = m_unitsPerLength[a];
        return axis == m_axes[1] ? m_bounds.upper[a] - scene / k
                                 : m_bounds.lower[a] + scene / k;
    }

    // The scene rect a raster over a physical region occupies (the region's
    // in-plane axes only).
    [[nodiscard]] SceneRect sceneRectForRegion(const RealBox& region) const noexcept
    {
        const auto h = m_axes[0];
        const auto v = m_axes[1];
        const auto hs = static_cast<std::size_t>(h);
        const auto vs = static_cast<std::size_t>(v);
        const auto left = sceneFromPhysical(h, region.lower[hs]);
        const auto right = sceneFromPhysical(h, region.upper[hs]);
        const auto top = sceneFromPhysical(v, region.upper[vs]);
        const auto bottom = sceneFromPhysical(v, region.lower[vs]);
        return {left, top, right - left, bottom - top};
    }

    // The physical window under a scene rect, sceneRectForRegion's inverse
    // (not cut to the canvas); the normal axis carries the canvas bounds.
    [[nodiscard]] RealBox regionForSceneRect(const SceneRect& rect) const noexcept
    {
        const auto h = m_axes[0];
        const auto v = m_axes[1];
        const auto hs = static_cast<std::size_t>(h);
        const auto vs = static_cast<std::size_t>(v);
        RealBox region = m_bounds;
        region.lower[hs] = physicalFromScene(h, rect.x);
        region.upper[hs] = physicalFromScene(h, rect.right());
        region.upper[vs] = physicalFromScene(v, rect.y);
        region.lower[vs] = physicalFromScene(v, rect.bottom());
        return region;
    }

    // The whole canvas: the scene rect, and what Fit frames.
    [[nodiscard]] SceneRect canvasRect() const noexcept
    {
        return sceneRectForRegion(m_bounds);
    }

private:
    RealBox m_bounds;
    std::array<int, 2> m_axes{0, 1};
    Real3 m_unitsPerLength{{1.0, 1.0, 1.0}};
};

// Where one view's tile sits: alone on its mapped canvas, or on the pair's
// shared canvas as one of its layers. Every placement asks this, so the
// arms that once picked a layout agree.
struct TilePlacement {
    std::optional<MappedLayout> single;
    const PairLayout* pair = nullptr;
    std::size_t layer = 0;
    SceneRect canvas;  // what the tile is placed against

    [[nodiscard]] SceneRect sceneRectForRegion(const RealBox& region) const noexcept
    {
        return pair ? pair->sceneRectForRegion(layer, region)
                    : single->sceneRectForRegion(region);
    }
    // The physical window under a scene rect, not cut to what the tile can
    // span: a warp is drawn for whole device pixels, which may reach past it.
    [[nodiscard]] RealBox regionForSceneRect(const SceneRect& rect) const noexcept
    {
        return pair ? pair->windowForSceneRect(layer, rect)
                    : single->regionForSceneRect(rect);
    }
    // The scene rect the tile can span: its layer's over a pair, the whole
    // canvas alone.
    [[nodiscard]] SceneRect extent() const noexcept
    {
        return pair ? pair->tileRect(layer) : single->canvasRect();
    }
};

} // namespace amrvis::qt
