#pragma once

#include "AspectMode.hpp"

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/Metadata.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>

// Two plotfiles shown in one window: how their domains relate, and how a
// panel places each one's raster in a shared scene. Qt-free so the unit test
// needs no QApplication.
namespace amrvis::qt {

// A rectangle in scene units; QRectF without Qt.
struct SceneRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] constexpr double right() const noexcept { return x + width; }
    [[nodiscard]] constexpr double bottom() const noexcept { return y + height; }
    [[nodiscard]] constexpr bool contains(double px, double py) const noexcept
    {
        return px >= x && px < right() && py >= y && py < bottom();
    }
    friend constexpr bool operator==(const SceneRect&, const SceneRect&) = default;
};

// Two 3-D datasets that touch along one axis and overlap along the other two:
// the one axis whose sample bounds do not overlap is the perpendicular axis;
// the plane they share is normal to it. Layer 0 is the primary dataset,
// layer 1 the companion; upperLayer is the one on the greater side.
struct PairGeometry {
    int perpendicularAxis = 2;
    std::array<int, 2> sharedAxes{0, 1};
    std::size_t upperLayer = 0;
    std::array<RealBox, 2> bounds;
    RealBox unionBounds;
    // Per axis, the smaller of the two finest cell sizes: the unit a shared
    // axis is measured in so both rasters keep at least one pixel per cell.
    Real3 referenceCellSize;
    std::array<Real3, 2> finestCellSize;

    [[nodiscard]] std::size_t lowerLayer() const noexcept { return 1 - upperLayer; }
    // The layer a position along the perpendicular axis belongs to: the
    // upper layer from its lower bound up (the interface included, and
    // anything above), the lower layer below that.
    [[nodiscard]] std::size_t layerAt(double position) const noexcept
    {
        const auto p = static_cast<std::size_t>(perpendicularAxis);
        return position >= bounds[upperLayer].lower[p] ? upperLayer : lowerLayer();
    }
    // Finest cells along the perpendicular axis per layer, and the stacked
    // index space the slice position control uses: the lower layer's rows
    // first, then the upper layer's.
    [[nodiscard]] int perpendicularCells(std::size_t layer) const noexcept
    {
        const auto p = static_cast<std::size_t>(perpendicularAxis);
        const auto extent = bounds[layer].upper[p] - bounds[layer].lower[p];
        return std::max(1, static_cast<int>(std::lround(extent / finestCellSize[layer][p])));
    }
    [[nodiscard]] int stackedCellCount() const noexcept
    {
        return perpendicularCells(0) + perpendicularCells(1);
    }
    [[nodiscard]] double positionForStackedIndex(int index) const noexcept
    {
        const auto p = static_cast<std::size_t>(perpendicularAxis);
        const auto lowerCells = perpendicularCells(lowerLayer());
        const auto layer = index < lowerCells ? lowerLayer() : upperLayer;
        const auto local = index < lowerCells ? index : index - lowerCells;
        const auto clamped = std::clamp(local, 0, perpendicularCells(layer) - 1);
        return bounds[layer].lower[p] + (clamped + 0.5) * finestCellSize[layer][p];
    }
    [[nodiscard]] int stackedIndexForPosition(double position) const noexcept
    {
        const auto p = static_cast<std::size_t>(perpendicularAxis);
        const auto layer = layerAt(position);
        const auto cells = perpendicularCells(layer);
        const auto local = std::clamp(static_cast<int>(std::floor(
            (position - bounds[layer].lower[p]) / finestCellSize[layer][p])),
            0, cells - 1);
        return layer == lowerLayer() ? local : local + perpendicularCells(lowerLayer());
    }
    // The shared axes are indexed over the union at the reference cell size.
    [[nodiscard]] int unionCellCount(int axis) const noexcept
    {
        const auto a = static_cast<std::size_t>(axis);
        return std::max(1, static_cast<int>(std::lround(
            (unionBounds.upper[a] - unionBounds.lower[a]) / referenceCellSize[a])));
    }
    [[nodiscard]] double positionForUnionIndex(int axis, int index) const noexcept
    {
        const auto a = static_cast<std::size_t>(axis);
        const auto clamped = std::clamp(index, 0, unionCellCount(axis) - 1);
        return unionBounds.lower[a] + (clamped + 0.5) * referenceCellSize[a];
    }
    [[nodiscard]] int unionIndexForPosition(int axis, double position) const noexcept
    {
        const auto a = static_cast<std::size_t>(axis);
        return std::clamp(static_cast<int>(std::floor(
            (position - unionBounds.lower[a]) / referenceCellSize[a])),
            0, unionCellCount(axis) - 1);
    }
};

struct PairGeometryResult {
    std::optional<PairGeometry> geometry;
    std::string error;
};

// Decide whether two datasets can share a window, and how. Refusals name the
// reason for the user.
[[nodiscard]] inline PairGeometryResult pairGeometry(
    const DatasetMetadata& primary, const DatasetMetadata& companion)
{
    const std::array<const DatasetMetadata*, 2> metadata{&primary, &companion};
    for (std::size_t layer = 0; layer < 2; ++layer) {
        const auto& m = *metadata[layer];
        const char* which = layer == 0 ? "the open dataset" : "the companion";
        if (m.dimension != 3) {
            return {std::nullopt,
                std::string(which) + " is not three-dimensional"};
        }
        if (!m.hasPhysicalGeometry || m.levels.empty()) {
            return {std::nullopt,
                std::string(which) + " carries no physical geometry"};
        }
        if (m.coordinateSystem != 0) {
            return {std::nullopt,
                std::string(which) + " is not in Cartesian coordinates"};
        }
    }
    PairGeometry geometry;
    for (std::size_t layer = 0; layer < 2; ++layer) {
        geometry.bounds[layer] = datasetSampleBounds(*metadata[layer]);
        geometry.finestCellSize[layer] = metadata[layer]->levels.back().cellSize;
        if (!geometry.bounds[layer].valid(3)) {
            return {std::nullopt, "a dataset's domain is not a valid box"};
        }
    }
    // The perpendicular axis is the one along which the two do not overlap.
    // Touching counts as not overlapping; a tolerance absorbs the rounding
    // of a coordinate written with few digits.
    int perpendicular = -1;
    int separated = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const auto a = static_cast<std::size_t>(axis);
        const auto low = std::max(geometry.bounds[0].lower[a], geometry.bounds[1].lower[a]);
        const auto high = std::min(geometry.bounds[0].upper[a], geometry.bounds[1].upper[a]);
        const auto span = std::max(
            geometry.bounds[0].upper[a] - geometry.bounds[0].lower[a],
            geometry.bounds[1].upper[a] - geometry.bounds[1].lower[a]);
        if (high - low <= 1.0e-9 * span) {
            ++separated;
            perpendicular = axis;
        }
    }
    if (separated == 0) {
        return {std::nullopt,
            "the two domains overlap along every axis, so they share no plane"};
    }
    if (separated > 1) {
        return {std::nullopt,
            "the two domains are apart along more than one axis, so they share no plane"};
    }
    {
        // Apart, not just non-overlapping: a gap would be drawn shut and the
        // positions inside it would belong to nobody, so the domains must
        // meet within the same tolerance.
        const auto a = static_cast<std::size_t>(perpendicular);
        const auto gap = std::max(geometry.bounds[0].lower[a], geometry.bounds[1].lower[a])
            - std::min(geometry.bounds[0].upper[a], geometry.bounds[1].upper[a]);
        const auto span = std::max(
            geometry.bounds[0].upper[a] - geometry.bounds[0].lower[a],
            geometry.bounds[1].upper[a] - geometry.bounds[1].lower[a]);
        if (gap > 1.0e-9 * span) {
            return {std::nullopt,
                "the two domains do not touch, so they share no plane"};
        }
    }
    geometry.perpendicularAxis = perpendicular;
    std::size_t next = 0;
    for (int axis = 0; axis < 3; ++axis) {
        if (axis != perpendicular) {
            geometry.sharedAxes[next++] = axis;
        }
    }
    const auto p = static_cast<std::size_t>(perpendicular);
    geometry.upperLayer
        = geometry.bounds[0].lower[p] >= geometry.bounds[1].lower[p] ? 0 : 1;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        geometry.unionBounds.lower[axis] = std::min(
            geometry.bounds[0].lower[axis], geometry.bounds[1].lower[axis]);
        geometry.unionBounds.upper[axis] = std::max(
            geometry.bounds[0].upper[axis], geometry.bounds[1].upper[axis]);
        geometry.referenceCellSize[axis] = std::min(
            geometry.finestCellSize[0][axis], geometry.finestCellSize[1][axis]);
    }
    return {geometry, {}};
}

// How one panel places the two rasters in its scene. Scene units are display
// units: a fixed scale N is N screen pixels per scene unit. Along a shared
// axis both layers use one linear map anchored at the union's lower bound;
// along the perpendicular axis each layer has its own band, the upper layer's
// first (top, or left), and its own scale. The panels are normalized so the
// primary's tightest raster pixel along any axis is one scene unit, the same
// rule displayStretchFor applies to the primary alone. The layout depends only on
// the geometry and the aspect settings, never on the current zoom, so pan and
// zoom never move a tile.
class PairLayout {
public:
    PairLayout() = default;
    PairLayout(const PairGeometry& geometry, int normal, AspectMode mode,
        const std::array<double, 3>& axisScale,
        const std::array<double, 2>& perpendicularScale)
        : m_geometry(geometry)
        , m_normal(normal)
    {
        std::size_t next = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis != normal) {
                m_axes[next++] = axis;
            }
        }
        const bool physical = mode == AspectMode::PhysicalSize;
        const auto sane = [](double value) {
            return std::isfinite(value) && value > 0.0 ? value : 1.0;
        };
        const auto p = static_cast<std::size_t>(geometry.perpendicularAxis);
        // Scene units per physical unit, before normalization.
        for (std::size_t axis = 0; axis < 3; ++axis) {
            m_sharedUnitsPerLength[axis] = sane(axisScale[axis])
                * (physical ? 1.0 : 1.0 / geometry.referenceCellSize[axis]);
        }
        for (std::size_t layer = 0; layer < 2; ++layer) {
            m_perpendicularUnitsPerLength[layer] = sane(perpendicularScale[layer])
                * (physical ? 1.0 : 1.0 / geometry.finestCellSize[layer][p]);
        }
        // Normalize: the smallest scene-units-per-raster-pixel over the
        // primary's three axes becomes one, so a fixed scale shows an axis at
        // the same size on every panel (see displayStretchFor) and means
        // what it meant with the primary alone. The companion is drawn in
        // the same units: finer cells of its own fall below a scene unit,
        // which its perpendicular factor is there to stretch.
        double smallest = std::numeric_limits<double>::infinity();
        for (int axis = 0; axis < 3; ++axis) {
            const auto a = static_cast<std::size_t>(axis);
            const auto perPixel = axis == geometry.perpendicularAxis
                ? m_perpendicularUnitsPerLength[0] * geometry.finestCellSize[0][a]
                : m_sharedUnitsPerLength[a] * geometry.finestCellSize[0][a];
            smallest = std::min(smallest, perPixel);
        }
        if (std::isfinite(smallest) && smallest > 0.0) {
            for (auto& value : m_sharedUnitsPerLength.values) {
                value /= smallest;
            }
            for (auto& value : m_perpendicularUnitsPerLength) {
                value /= smallest;
            }
        }
        // Bands along the perpendicular axis, when the panel shows it.
        const auto upper = geometry.upperLayer;
        const auto lower = geometry.lowerLayer();
        const auto extent = [&](std::size_t layer) {
            return (geometry.bounds[layer].upper[p] - geometry.bounds[layer].lower[p])
                * m_perpendicularUnitsPerLength[layer];
        };
        if (m_axes[1] == geometry.perpendicularAxis) {
            // Vertical: scene y grows downward, so the upper layer is on top.
            m_bandStart[upper] = 0.0;
            m_bandStart[lower] = extent(upper);
        } else {
            // Horizontal: the lower layer is on the left.
            m_bandStart[lower] = 0.0;
            m_bandStart[upper] = extent(lower);
        }
    }

    [[nodiscard]] int normal() const noexcept { return m_normal; }
    [[nodiscard]] bool showsPerpendicular() const noexcept
    {
        return m_normal != m_geometry.perpendicularAxis;
    }
    [[nodiscard]] const std::array<int, 2>& axes() const noexcept { return m_axes; }

    // Scene coordinate of a physical position along a displayed axis for a
    // layer; the layer matters only along the perpendicular axis.
    [[nodiscard]] double sceneFromPhysical(
        std::size_t layer, int axis, double position) const noexcept
    {
        const auto a = static_cast<std::size_t>(axis);
        const bool vertical = axis == m_axes[1];
        if (axis == m_geometry.perpendicularAxis) {
            const auto& bounds = m_geometry.bounds[layer];
            const auto k = m_perpendicularUnitsPerLength[layer];
            return vertical
                ? m_bandStart[layer] + (bounds.upper[a] - position) * k
                : m_bandStart[layer] + (position - bounds.lower[a]) * k;
        }
        const auto& union_ = m_geometry.unionBounds;
        const auto k = m_sharedUnitsPerLength[a];
        return vertical ? (union_.upper[a] - position) * k
                        : (position - union_.lower[a]) * k;
    }

    // The physical position under a scene coordinate along a displayed axis:
    // sceneFromPhysical's inverse, with each layer's band extended past its
    // edges so a rect can be cut to the layer's domain afterwards.
    [[nodiscard]] double physicalFromScene(
        std::size_t layer, int axis, double scene) const noexcept
    {
        const auto a = static_cast<std::size_t>(axis);
        const bool vertical = axis == m_axes[1];
        if (axis == m_geometry.perpendicularAxis) {
            const auto& bounds = m_geometry.bounds[layer];
            const auto k = m_perpendicularUnitsPerLength[layer];
            return vertical
                ? bounds.upper[a] - (scene - m_bandStart[layer]) / k
                : bounds.lower[a] + (scene - m_bandStart[layer]) / k;
        }
        const auto& union_ = m_geometry.unionBounds;
        const auto k = m_sharedUnitsPerLength[a];
        return vertical ? union_.upper[a] - scene / k
                        : union_.lower[a] + scene / k;
    }

    // The part of a layer's domain under a scene rect: the rect through the
    // layer's maps, cut to its bounds, the normal axis whole. Nothing when
    // the rect misses the layer, or meets it within the pairing tolerance
    // only (a selection ending at the interface belongs to one side).
    [[nodiscard]] std::optional<RealBox> regionForSceneRect(
        std::size_t layer, const SceneRect& rect) const noexcept
    {
        const auto& bounds = m_geometry.bounds[layer];
        const auto h = m_axes[0];
        const auto v = m_axes[1];
        const auto hs = static_cast<std::size_t>(h);
        const auto vs = static_cast<std::size_t>(v);
        RealBox region = bounds;
        region.lower[hs] = std::max(bounds.lower[hs], physicalFromScene(layer, h, rect.x));
        region.upper[hs]
            = std::min(bounds.upper[hs], physicalFromScene(layer, h, rect.right()));
        // Vertical scene coordinates count down: the rect's top is the
        // region's upper bound.
        region.upper[vs] = std::min(bounds.upper[vs], physicalFromScene(layer, v, rect.y));
        region.lower[vs]
            = std::max(bounds.lower[vs], physicalFromScene(layer, v, rect.bottom()));
        for (const auto axis : {hs, vs}) {
            const auto span = bounds.upper[axis] - bounds.lower[axis];
            if (!(region.upper[axis] - region.lower[axis] > 1e-9 * span)) {
                return std::nullopt;
            }
        }
        return region;
    }

    // The scene rect a layer's raster over a physical region occupies.
    [[nodiscard]] SceneRect sceneRectForRegion(
        std::size_t layer, const RealBox& region) const noexcept
    {
        const auto h = m_axes[0];
        const auto v = m_axes[1];
        const auto hs = static_cast<std::size_t>(h);
        const auto vs = static_cast<std::size_t>(v);
        const auto left = sceneFromPhysical(layer, h, region.lower[hs]);
        const auto right = sceneFromPhysical(layer, h, region.upper[hs]);
        // Vertical scene coordinates count down, so the region's upper bound
        // is its top.
        const auto top = sceneFromPhysical(layer, v, region.upper[vs]);
        const auto bottom = sceneFromPhysical(layer, v, region.lower[vs]);
        return {left, top, right - left, bottom - top};
    }

    // A layer's whole domain.
    [[nodiscard]] SceneRect tileRect(std::size_t layer) const noexcept
    {
        return sceneRectForRegion(layer, m_geometry.bounds[layer]);
    }

    // The union of both layers' tiles: the scene rect and what Fit frames.
    [[nodiscard]] SceneRect canvasRect() const noexcept
    {
        const auto a = tileRect(0);
        const auto b = tileRect(1);
        const auto left = std::min(a.x, b.x);
        const auto top = std::min(a.y, b.y);
        const auto right = std::max(a.right(), b.right());
        const auto bottom = std::max(a.bottom(), b.bottom());
        return {left, top, right - left, bottom - top};
    }

private:
    PairGeometry m_geometry;
    int m_normal = 2;
    std::array<int, 2> m_axes{0, 1};
    Real3 m_sharedUnitsPerLength{{1.0, 1.0, 1.0}};
    std::array<double, 2> m_perpendicularUnitsPerLength{1.0, 1.0};
    std::array<double, 2> m_bandStart{0.0, 0.0};
};

// The same proportions for the whole 3-D domain, for the isometric view: a
// shared linear map along the shared axes and one band per layer along the
// perpendicular axis (the lower layer's first), normalized over all three
// axes as PairLayout normalizes a panel.
class PairDisplayMap {
public:
    PairDisplayMap() = default;
    PairDisplayMap(const PairGeometry& geometry, AspectMode mode,
        const std::array<double, 3>& axisScale,
        const std::array<double, 2>& perpendicularScale)
        : m_geometry(geometry)
    {
        const bool physical = mode == AspectMode::PhysicalSize;
        const auto sane = [](double value) {
            return std::isfinite(value) && value > 0.0 ? value : 1.0;
        };
        const auto p = static_cast<std::size_t>(geometry.perpendicularAxis);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            m_sharedUnitsPerLength[axis] = sane(axisScale[axis])
                * (physical ? 1.0 : 1.0 / geometry.referenceCellSize[axis]);
        }
        for (std::size_t layer = 0; layer < 2; ++layer) {
            m_perpendicularUnitsPerLength[layer] = sane(perpendicularScale[layer])
                * (physical ? 1.0 : 1.0 / geometry.finestCellSize[layer][p]);
        }
        double smallest = std::numeric_limits<double>::infinity();
        for (std::size_t axis = 0; axis < 3; ++axis) {
            for (std::size_t layer = 0; layer < 2; ++layer) {
                const auto perPixel = axis == p
                    ? m_perpendicularUnitsPerLength[layer] * geometry.finestCellSize[layer][axis]
                    : m_sharedUnitsPerLength[axis] * geometry.finestCellSize[layer][axis];
                smallest = std::min(smallest, perPixel);
            }
        }
        if (std::isfinite(smallest) && smallest > 0.0) {
            for (auto& value : m_sharedUnitsPerLength.values) {
                value /= smallest;
            }
            for (auto& value : m_perpendicularUnitsPerLength) {
                value /= smallest;
            }
        }
        const auto lower = geometry.lowerLayer();
        m_bandStart[lower] = 0.0;
        m_bandStart[geometry.upperLayer]
            = (geometry.bounds[lower].upper[p] - geometry.bounds[lower].lower[p])
            * m_perpendicularUnitsPerLength[lower];
    }

    // A layer's physical point in display units.
    [[nodiscard]] Real3 displayFromPhysical(std::size_t layer, const Real3& point) const noexcept
    {
        Real3 display;
        const auto p = static_cast<std::size_t>(m_geometry.perpendicularAxis);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (axis == p) {
                display[axis] = m_bandStart[layer]
                    + (point[axis] - m_geometry.bounds[layer].lower[axis])
                        * m_perpendicularUnitsPerLength[layer];
            } else {
                display[axis] = (point[axis] - m_geometry.unionBounds.lower[axis])
                    * m_sharedUnitsPerLength[axis];
            }
        }
        return display;
    }

private:
    PairGeometry m_geometry;
    Real3 m_sharedUnitsPerLength{{1.0, 1.0, 1.0}};
    std::array<double, 2> m_perpendicularUnitsPerLength{1.0, 1.0};
    std::array<double, 2> m_bandStart{0.0, 0.0};
};

} // namespace amrvis::qt
