#pragma once

#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/MappedGrid.hpp>
#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/pipeline/ParticleProjection.hpp>
#include <amrexplorer/render2d/MappedGridWarp.hpp>

#include <QPointF>
#include <QRectF>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace amrvis::qt {

// The physical box that a rectangle of raster pixels covers, given the box the
// whole raster covers. Only the two axes the view displays are narrowed;
// `rasterRegion`'s third stays as it is, which for a slice is the thickness the
// plane was requested with.
//
// Raster rows count down while physical coordinates count up, so the vertical
// edges swap: the rect's *bottom* gives the lower physical bound. Getting that
// backwards mirrors the region about the plane's middle, which looks entirely
// plausible on a symmetric domain -- which is why this is one definition used
// by both callers rather than the formula written out twice.
[[nodiscard]] inline RealBox physicalRegionForRasterRect(
    const RealBox& rasterRegion, double rasterWidth, double rasterHeight,
    const QRectF& rasterRect, const std::array<int, 2>& axes) noexcept
{
    auto region = rasterRegion;
    if (!(rasterWidth > 0.0) || !(rasterHeight > 0.0)) {
        return region;
    }
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto xExtent = rasterRegion.upper[xAxis] - rasterRegion.lower[xAxis];
    const auto yExtent = rasterRegion.upper[yAxis] - rasterRegion.lower[yAxis];
    region.lower[xAxis]
        = rasterRegion.lower[xAxis] + rasterRect.left() / rasterWidth * xExtent;
    region.upper[xAxis]
        = rasterRegion.lower[xAxis] + rasterRect.right() / rasterWidth * xExtent;
    region.lower[yAxis] = rasterRegion.lower[yAxis]
        + (rasterHeight - rasterRect.bottom()) / rasterHeight * yExtent;
    region.upper[yAxis] = rasterRegion.lower[yAxis]
        + (rasterHeight - rasterRect.top()) / rasterHeight * yExtent;
    return region;
}

// Converts between a view's logical in-plane physical coordinates and scene
// (pixmap-pixel) coordinates, absorbing the display that is not the logical
// grid: the 2-D spherical layouts (the R-Z warp, or the r-theta / theta-r
// logical, optionally axis-swapped, placements) and the mapped-grid warp,
// where every raster cell is drawn as the quadrilateral of its node
// positions. Scene y is top-down because the displayed raster is flipped
// vertically, so plane row 0 maps to the bottom of the scene.
//
// The logical axes are `axes` of the dataset (0 and 1 for a 2-D view: (x, y)
// for Cartesian, (r, theta) for spherical; the two in-plane axes of a 3-D
// panel). For a plain Cartesian display displayRegion == logicalRegion and
// every mapping is the linear one the overlay call sites use inline; the
// struct is only built and used on the spherical and mapped paths, so those
// sites keep their exact prior behavior.
//
// On the mapped path the pixmap is physical and uniform, so display <-> scene
// stays linear over displayRegion; what changes is plane pixel -> display
// (bilinear through the node positions) and its inverse, which is a lookup in
// the warp's per-pixel source index rather than arithmetic. The spherical R-Z
// wedge is drawn the same way (a window at the view's pixels), but keeps the
// analytic spherical arms, which stay exact over that window.
struct PlaneMapping {
    bool spherical = false;
    SphericalDisplay mode = SphericalDisplay::RZ;
    // Mapped-grid warp: plane pixels map to the scene through `nodes`, and
    // scene pixels back to plane pixels through `sourceIndex` (both from the
    // SliceDisplayResult that produced the pixmap; sourceIndex is parallel to
    // the pixmap with row 0 at the bottom).
    bool mapped = false;
    std::array<int, 2> axes{0, 1};
    std::shared_ptr<const MappedGridPlane> nodes;
    std::shared_ptr<const std::vector<std::int32_t>> sourceIndex;
    RealBox logicalRegion;  // plane.physicalRegion: (x, y) or (r, theta)
    RealBox displayRegion;  // pixmap display bounds (== logicalRegion if !spherical)
    double sceneWidth = 1.0;
    double sceneHeight = 1.0;
    double planeWidth = 1.0;
    double planeHeight = 1.0;

    // Logical (r, theta) -> display axes (u, v) matching displayRegion/pixmap.
    // Not meaningful on the mapped path (a logical position has no single
    // display position without its plane pixel): callers use
    // sceneFromPlanePixel there.
    [[nodiscard]] std::array<double, 2> displayFromLogical(
        double r, double theta) const
    {
        if (!spherical) {
            return {r, theta};
        }
        switch (mode) {
        case SphericalDisplay::RZ:
            return sphericalToDisplay(r, theta);
        case SphericalDisplay::ThetaR:
            return {theta, r};
        case SphericalDisplay::RTheta:
        default:
            return {r, theta};
        }
    }

    // Inverse of displayFromLogical: display axes (u, v) -> logical (r, theta).
    // On the mapped path display is physical and the logical position is
    // found through planePixelFromScene instead; this returns (u, v).
    [[nodiscard]] std::array<double, 2> logicalFromDisplay(
        double u, double v) const
    {
        if (!spherical) {
            return {u, v};
        }
        switch (mode) {
        case SphericalDisplay::RZ:
            return displayToSpherical(u, v);
        case SphericalDisplay::ThetaR:
            return {v, u};  // u = theta, v = r
        case SphericalDisplay::RTheta:
        default:
            return {u, v};
        }
    }

    // Logical (x, y)/(r, theta) -> scene point. On the mapped path the
    // logical position is converted to its fractional plane pixel first, so
    // the node positions place it.
    [[nodiscard]] QPointF sceneFromLogical(double a, double b) const
    {
        if (mapped) {
            const auto x0 = static_cast<std::size_t>(axes[0]);
            const auto y0 = static_cast<std::size_t>(axes[1]);
            const double spanA = logicalRegion.upper[x0] - logicalRegion.lower[x0];
            const double spanB = logicalRegion.upper[y0] - logicalRegion.lower[y0];
            const double col = spanA != 0.0
                ? (a - logicalRegion.lower[x0]) / spanA * planeWidth : 0.0;
            const double row = spanB != 0.0
                ? (b - logicalRegion.lower[y0]) / spanB * planeHeight : 0.0;
            return sceneFromPlanePixel(col, row);
        }
        const auto display = displayFromLogical(a, b);
        return sceneFromDisplay(display[0], display[1]);
    }

    // Display-space (u, v) -> scene point: the linear map over displayRegion
    // every layout shares. Used directly for overlays already expressed in
    // display coordinates (the R-Z vector glyphs, mapped-grid particles).
    [[nodiscard]] QPointF sceneFromDisplay(double u, double v) const
    {
        const auto x0 = static_cast<std::size_t>(axes[0]);
        const auto y0 = static_cast<std::size_t>(axes[1]);
        const double spanX = displayRegion.upper[x0] - displayRegion.lower[x0];
        const double spanY = displayRegion.upper[y0] - displayRegion.lower[y0];
        const double x = spanX != 0.0
            ? (u - displayRegion.lower[x0]) / spanX * sceneWidth : 0.0;
        const double y = spanY != 0.0
            ? sceneHeight - (v - displayRegion.lower[y0]) / spanY * sceneHeight
            : 0.0;
        return {x, y};
    }

    // Scene point -> display-space (u, v): the inverse of sceneFromDisplay.
    [[nodiscard]] std::array<double, 2> displayFromScene(
        double px, double py) const
    {
        const auto x0 = static_cast<std::size_t>(axes[0]);
        const auto y0 = static_cast<std::size_t>(axes[1]);
        const double spanX = displayRegion.upper[x0] - displayRegion.lower[x0];
        const double spanY = displayRegion.upper[y0] - displayRegion.lower[y0];
        const double u = displayRegion.lower[x0] + px / sceneWidth * spanX;
        const double v = displayRegion.lower[y0]
            + (sceneHeight - py) / sceneHeight * spanY;
        return {u, v};
    }

    // Scene point -> logical (x, y)/(r, theta). Inverse of sceneFromLogical
    // on the spherical layouts; on the mapped path this is the physical
    // display position (see logicalFromDisplay).
    [[nodiscard]] std::array<double, 2> logicalFromScene(double px, double py) const
    {
        const auto display = displayFromScene(px, py);
        return logicalFromDisplay(display[0], display[1]);
    }

    // Plane-pixel (col, row; row 0 = bottom) -> scene point. Used to re-project
    // contour polylines, which are traced in the logical raster, and on the
    // mapped path everything anchored in raster pixels (glyphs, box outlines).
    [[nodiscard]] QPointF sceneFromPlanePixel(double col, double row) const
    {
        if (mapped && nodes) {
            const auto display = mappedDisplayPosition(*nodes, col, row);
            return sceneFromDisplay(display[0], display[1]);
        }
        const auto x0 = static_cast<std::size_t>(axes[0]);
        const auto y0 = static_cast<std::size_t>(axes[1]);
        const double spanX = logicalRegion.upper[x0] - logicalRegion.lower[x0];
        const double spanY = logicalRegion.upper[y0] - logicalRegion.lower[y0];
        const double a = logicalRegion.lower[x0] + col / planeWidth * spanX;
        const double b = logicalRegion.lower[y0] + row / planeHeight * spanY;
        return sceneFromLogical(a, b);
    }

    // Scene point -> the plane pixel (col, row; row 0 = bottom) drawn there,
    // or nothing where no cell was drawn (outside the stretched domain).
    // Mapped path only: the warp records which raster pixel each display
    // pixel came from, which is the inverse no formula gives. Elsewhere the
    // linear inverse over logicalRegion applies.
    [[nodiscard]] std::optional<std::array<int, 2>> planePixelFromScene(
        double px, double py) const
    {
        if (mapped) {
            if (!sourceIndex) {
                return std::nullopt;
            }
            const auto width = static_cast<int>(std::lround(sceneWidth));
            const auto height = static_cast<int>(std::lround(sceneHeight));
            // Checked as doubles before the cast: a point far off the pixmap
            // (a particle at deep zoom) is past any int.
            if (!(px >= 0.0) || !(py >= 0.0) || !(px < width) || !(py < height)) {
                return std::nullopt;
            }
            const auto column = static_cast<int>(std::floor(px));
            // The pixmap is flipped for display: scene row 0 is the top,
            // source-index row 0 the bottom.
            const auto row = height - 1 - static_cast<int>(std::floor(py));
            const auto offset = static_cast<std::size_t>(row)
                    * static_cast<std::size_t>(width)
                + static_cast<std::size_t>(column);
            if (offset >= sourceIndex->size()) {
                return std::nullopt;
            }
            const auto source = (*sourceIndex)[offset];
            if (source < 0) {
                return std::nullopt;
            }
            const auto planeW = std::max(1, static_cast<int>(std::lround(planeWidth)));
            return std::array<int, 2>{source % planeW, source / planeW};
        }
        const auto logical = logicalFromScene(px, py);
        const auto x0 = static_cast<std::size_t>(axes[0]);
        const auto y0 = static_cast<std::size_t>(axes[1]);
        const double spanX = logicalRegion.upper[x0] - logicalRegion.lower[x0];
        const double spanY = logicalRegion.upper[y0] - logicalRegion.lower[y0];
        if (!(spanX > 0.0) || !(spanY > 0.0)) {
            return std::nullopt;
        }
        const auto col = (logical[0] - logicalRegion.lower[x0]) / spanX * planeWidth;
        const auto row = (logical[1] - logicalRegion.lower[y0]) / spanY * planeHeight;
        if (!(col >= 0.0) || !(row >= 0.0) || !(col < std::floor(planeWidth))
            || !(row < std::floor(planeHeight))) {
            return std::nullopt;
        }
        return std::array<int, 2>{static_cast<int>(col), static_cast<int>(row)};
    }
};

// The physical faces of a cell the plane drew at `level`, along the slice
// normal, at the in-plane position (a, b) within it. The corner nodes are read
// as the two triangles the warp draws the cell from, so a sloped face is taken
// where the point is: the four-corner average is the cell's centre, which over
// a cell spanning a steep terrain is not the face above or below the point at
// all. Nothing when the node plane carries no faces for that level: a 2-D
// slice, which has no normal to be in or out of.
[[nodiscard]] inline std::optional<std::array<double, 2>> mappedCellFaces(
    const MappedGridPlane& nodes, int column, int row, double a, double b,
    int level)
{
    const auto count = static_cast<std::size_t>(std::max(0, nodes.width))
        * static_cast<std::size_t>(std::max(0, nodes.height));
    const auto block = mappedFaceOffset(nodes, level);
    if (!block || column < 0 || row < 0 || column + 1 >= nodes.width
        || row + 1 >= nodes.height || nodes.a.size() != count
        || nodes.b.size() != count || nodes.normalLower.size() < *block + count
        || nodes.normalUpper.size() < *block + count) {
        return std::nullopt;
    }
    const auto stride = static_cast<std::size_t>(nodes.width);
    const auto corner = static_cast<std::size_t>(row) * stride
        + static_cast<std::size_t>(column);
    // The warp's own split of the quad (see warpMappedGrid).
    const std::array<std::array<std::size_t, 3>, 2> triangles{
        std::array<std::size_t, 3>{corner, corner + 1, corner + stride + 1},
        std::array<std::size_t, 3>{corner, corner + stride + 1, corner + stride}};
    std::array<std::size_t, 3> nodesOf{};
    std::array<double, 3> weights{};
    bool weighted = false;
    for (const auto& triangle : triangles) {
        const double x0 = nodes.a[triangle[0]];
        const double y0 = nodes.b[triangle[0]];
        const double ux = nodes.a[triangle[1]] - x0;
        const double uy = nodes.b[triangle[1]] - y0;
        const double vx = nodes.a[triangle[2]] - x0;
        const double vy = nodes.b[triangle[2]] - y0;
        const double area = ux * vy - vx * uy;
        if (!(std::abs(area) > 0.0)) {
            continue;
        }
        const double px = a - x0;
        const double py = b - y0;
        std::array<double, 3> candidate{};
        candidate[1] = (px * vy - vx * py) / area;
        candidate[2] = (ux * py - px * uy) / area;
        candidate[0] = 1.0 - candidate[1] - candidate[2];
        // The triangle holding the point, or the one holding it most nearly
        // where rounding puts it just outside both.
        const auto inside = std::min({candidate[0], candidate[1], candidate[2]});
        if (weighted
            && !(inside > std::min({weights[0], weights[1], weights[2]}))) {
            continue;
        }
        nodesOf = triangle;
        weights = candidate;
        weighted = true;
    }
    const auto face = [&](const std::vector<double>& values) {
        const auto at = [&](std::size_t n) { return values[*block + n]; };
        if (!weighted) {
            // A cell with no area: its corners are all it says.
            return 0.25
                * (at(corner) + at(corner + 1) + at(corner + stride)
                    + at(corner + stride + 1));
        }
        return weights[0] * at(nodesOf[0]) + weights[1] * at(nodesOf[1])
            + weights[2] * at(nodesOf[2]);
    };
    const double lower = face(nodes.normalLower);
    const double upper = face(nodes.normalUpper);
    return std::array<double, 2>{
        std::min(lower, upper), std::max(lower, upper)};
}

// A particle on a mapped view: the tile point its physical in-plane position
// (a, b) lands on, kept only over a cell the warp drew -- the plane's logical
// bounds play no part, since the grid can reach past them. With slabs (see
// sliceCellSlabs) the normal coordinate must also lie in that cell: between
// its displaced faces where the nodes carry them, else in its level's slab.
[[nodiscard]] inline std::optional<QPointF> mappedParticlePoint(
    const PlaneMapping& mapping, const ScalarPlane& plane, double a, double b,
    double normal, std::span<const SliceCellSlab> levelSlabs)
{
    if (!std::isfinite(a) || !std::isfinite(b)) {
        return std::nullopt;
    }
    const auto scene = mapping.sceneFromDisplay(a, b);
    const auto pixel = mapping.planePixelFromScene(scene.x(), scene.y());
    if (!pixel) {
        return std::nullopt;
    }
    if (!levelSlabs.empty()) {
        const auto offset = static_cast<std::size_t>((*pixel)[0])
            + static_cast<std::size_t>(std::max(0, plane.width))
                * static_cast<std::size_t>((*pixel)[1]);
        if (offset >= plane.sourceLevel.size()) {
            return std::nullopt;
        }
        const auto level = plane.sourceLevel[offset];
        if (level < 0 || static_cast<std::size_t>(level) >= levelSlabs.size()) {
            return std::nullopt;
        }
        // The drawn cell's own faces where the node plane carries that level's:
        // the level's slab is logical, and a terrain-following cell is not
        // there.
        const auto faces = mapping.nodes
            ? mappedCellFaces(*mapping.nodes, (*pixel)[0], (*pixel)[1], a, b, level)
            : std::optional<std::array<double, 2>>{};
        const auto& slab = levelSlabs[static_cast<std::size_t>(level)];
        const auto lower = faces ? (*faces)[0] : slab.lower;
        const auto upper = faces ? (*faces)[1] : slab.upper;
        // Half-open, as the slice picks its cell.
        if (!(normal >= lower) || !(normal < upper)) {
            return std::nullopt;
        }
    }
    return scene;
}

} // namespace amrvis::qt
