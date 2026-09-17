#include <amrexplorer/render2d/MappedGridWarp.hpp>

#include <amrexplorer/render2d/detail/QuadRasterizer.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace amrvis {

std::optional<RealBox> mappedGridWindow(const MappedGridPlane& nodes,
    std::array<int, 2> axes, const RealBox& window)
{
    const auto bounds = mappedGridDisplayBounds(nodes, axes);
    if (!bounds || !detail::validOnAxes(window, axes)) {
        return bounds;
    }
    RealBox drawn = *bounds;
    for (const auto axis : axes) {
        const auto i = static_cast<std::size_t>(axis);
        drawn.lower[i] = window.lower[i];
        drawn.upper[i] = window.upper[i];
    }
    return drawn;
}

std::optional<RealBox> mappedGridDisplayBounds(
    const MappedGridPlane& nodes, std::array<int, 2> axes)
{
    if (nodes.width < 2 || nodes.height < 2
        || axes[0] < 0 || axes[0] > 2 || axes[1] < 0 || axes[1] > 2
        || axes[0] == axes[1]) {
        return std::nullopt;
    }
    const auto nodeCount = static_cast<std::size_t>(nodes.width)
        * static_cast<std::size_t>(nodes.height);
    if (nodes.a.size() != nodeCount || nodes.b.size() != nodeCount) {
        return std::nullopt;
    }
    double minA = std::numeric_limits<double>::infinity();
    double maxA = -minA;
    double minB = minA;
    double maxB = -minA;
    for (std::size_t node = 0; node < nodeCount; ++node) {
        const double a = nodes.a[node];
        const double b = nodes.b[node];
        if (!std::isfinite(a) || !std::isfinite(b)) {
            return std::nullopt;
        }
        minA = std::min(minA, a);
        maxA = std::max(maxA, a);
        minB = std::min(minB, b);
        maxB = std::max(maxB, b);
    }
    if (!(maxA > minA) || !(maxB > minB)) {
        return std::nullopt;
    }
    RealBox bounds = nodes.physicalRegion;
    bounds.lower[static_cast<std::size_t>(axes[0])] = minA;
    bounds.upper[static_cast<std::size_t>(axes[0])] = maxA;
    bounds.lower[static_cast<std::size_t>(axes[1])] = minB;
    bounds.upper[static_cast<std::size_t>(axes[1])] = maxB;
    return bounds;
}

MappedWarpedRaster warpMappedGrid(const ImageBuffer& src,
    const MappedGridPlane& nodes, std::array<int, 2> axes,
    const RealBox& window, std::array<int, 2> pixels)
{
    MappedWarpedRaster out;
    const auto fallback = [&]() {
        out.image = src;
        out.displayRegion = nodes.physicalRegion;
        out.mappedBounds = nodes.physicalRegion;
        out.sourceIndex.reset();
        return out;
    };

    const int srcW = src.width;
    const int srcH = src.height;
    if (srcW <= 0 || srcH <= 0 || nodes.width != srcW + 1
        || nodes.height != srcH + 1) {
        return fallback();
    }
    const auto bounds = mappedGridDisplayBounds(nodes, axes);
    const auto drawn = mappedGridWindow(nodes, axes, window);
    const auto pixelCount = static_cast<std::size_t>(srcW)
        * static_cast<std::size_t>(srcH);
    if (!bounds || !drawn || src.rgba.size() < pixelCount) {
        return fallback();
    }
    const auto axisA = static_cast<std::size_t>(axes[0]);
    const auto axisB = static_cast<std::size_t>(axes[1]);
    const double minA = drawn->lower[axisA];
    const double minB = drawn->lower[axisB];
    const double spanA = drawn->upper[axisA] - minA;
    const double spanB = drawn->upper[axisB] - minB;

    // The output is the caller's screen window: its pixel count is given,
    // and the pitch follows from the window. A zero count means the raster's
    // own size along that axis. Bounded so a corrupt request cannot ask for
    // an image no display would show.
    constexpr int maximumPixels = 16384;
    const int width = std::clamp(pixels[0] > 0 ? pixels[0] : srcW, 1, maximumPixels);
    const int height = std::clamp(pixels[1] > 0 ? pixels[1] : srcH, 1, maximumPixels);

    const double scaleA = static_cast<double>(width) / spanA;
    const double scaleB = static_cast<double>(height) / spanB;
    const auto stride = static_cast<std::size_t>(nodes.width);
    // Only ever asked at the cell corners (no subdivision): the node there.
    const auto corner = [&](double col, double row) {
        const auto node = static_cast<std::size_t>(row) * stride
            + static_cast<std::size_t>(col);
        return detail::Point{(nodes.a[node] - minA) * scaleA,
            (nodes.b[node] - minB) * scaleB};
    };
    auto drawnRaster = detail::rasterizeCells(src, width, height, {1, 1}, 0.0, corner);

    out.displayRegion = *drawn;
    out.mappedBounds = *bounds;
    out.image = std::move(drawnRaster.image);
    out.sourceIndex = std::move(drawnRaster.sourceIndex);
    return out;
}

std::array<double, 2> mappedDisplayPosition(
    const MappedGridPlane& nodes, double col, double row)
{
    const int cellsW = nodes.width - 1;
    const int cellsH = nodes.height - 1;
    const auto nodeCount = static_cast<std::size_t>(std::max(0, nodes.width))
        * static_cast<std::size_t>(std::max(0, nodes.height));
    if (cellsW < 1 || cellsH < 1 || nodes.a.size() != nodeCount
        || nodes.b.size() != nodeCount || !std::isfinite(col)
        || !std::isfinite(row)) {
        constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
        return {nan, nan};
    }
    col = std::clamp(col, 0.0, static_cast<double>(cellsW));
    row = std::clamp(row, 0.0, static_cast<double>(cellsH));
    const int i = std::min(cellsW - 1, static_cast<int>(std::floor(col)));
    const int j = std::min(cellsH - 1, static_cast<int>(std::floor(row)));
    const double fx = col - static_cast<double>(i);
    const double fy = row - static_cast<double>(j);
    const auto stride = static_cast<std::size_t>(nodes.width);
    const auto n00 = static_cast<std::size_t>(j) * stride + static_cast<std::size_t>(i);
    const auto n10 = n00 + 1;
    const auto n01 = n00 + stride;
    const auto n11 = n01 + 1;
    const auto blend = [&](const std::vector<double>& v) {
        return (1.0 - fy) * ((1.0 - fx) * v[n00] + fx * v[n10])
            + fy * ((1.0 - fx) * v[n01] + fx * v[n11]);
    };
    return {blend(nodes.a), blend(nodes.b)};
}

} // namespace amrvis
