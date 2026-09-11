#include <amrexplorer/render2d/MappedGridWarp.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace amrvis {

namespace {

struct Point {
    double x = 0.0;
    double y = 0.0;
};

// Fills every output pixel whose centre lies inside (or on) the triangle.
// Edge functions are normalised by the triangle's orientation so either
// winding works; a zero-area triangle covers nothing and is skipped. Pixels
// on a shared edge are written by both neighbours, which is harmless: both
// write a colour that belongs there.
void fillTriangle(Point p0, Point p1, Point p2, std::uint32_t colour,
    std::int32_t index, ImageBuffer& image, std::vector<std::int32_t>& source)
{
    const double area = (p1.x - p0.x) * (p2.y - p0.y)
        - (p2.x - p0.x) * (p1.y - p0.y);
    if (!(std::abs(area) > 0.0)) {
        return;
    }
    const double orientation = area > 0.0 ? 1.0 : -1.0;
    const double xLo = std::min({p0.x, p1.x, p2.x});
    const double xHi = std::max({p0.x, p1.x, p2.x});
    const double yLo = std::min({p0.y, p1.y, p2.y});
    const double yHi = std::max({p0.y, p1.y, p2.y});
    // Pixel centres are at integer + 0.5, so pixel n covers [n, n+1).
    const int colStart = std::max(0, static_cast<int>(std::floor(xLo - 0.5)));
    const int colEnd = std::min(image.width - 1,
        static_cast<int>(std::ceil(xHi - 0.5)));
    const int rowStart = std::max(0, static_cast<int>(std::floor(yLo - 0.5)));
    const int rowEnd = std::min(image.height - 1,
        static_cast<int>(std::ceil(yHi - 0.5)));
    // Tolerance so a centre on a shared edge counts for both cells rather
    // than neither.
    const double epsilon = 1e-9 * std::abs(area);
    for (int row = rowStart; row <= rowEnd; ++row) {
        const double cy = static_cast<double>(row) + 0.5;
        for (int col = colStart; col <= colEnd; ++col) {
            const double cx = static_cast<double>(col) + 0.5;
            const double e0 = orientation
                * ((p1.x - p0.x) * (cy - p0.y) - (cx - p0.x) * (p1.y - p0.y));
            const double e1 = orientation
                * ((p2.x - p1.x) * (cy - p1.y) - (cx - p1.x) * (p2.y - p1.y));
            const double e2 = orientation
                * ((p0.x - p2.x) * (cy - p2.y) - (cx - p2.x) * (p0.y - p2.y));
            if (e0 < -epsilon || e1 < -epsilon || e2 < -epsilon) {
                continue;
            }
            const auto offset = static_cast<std::size_t>(row)
                    * static_cast<std::size_t>(image.width)
                + static_cast<std::size_t>(col);
            image.rgba[offset] = colour;
            source[offset] = index;
        }
    }
}

} // namespace

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
    const MappedGridPlane& nodes, std::array<int, 2> axes, int maxDimension,
    int supersample)
{
    MappedWarpedRaster out;
    const auto fallback = [&]() {
        out.image = src;
        out.displayRegion = nodes.physicalRegion;
        out.sourceIndex.reset();
        return out;
    };

    const int srcW = src.width;
    const int srcH = src.height;
    if (srcW <= 0 || srcH <= 0 || maxDimension < 1
        || nodes.width != srcW + 1 || nodes.height != srcH + 1) {
        return fallback();
    }
    const auto bounds = mappedGridDisplayBounds(nodes, axes);
    const auto pixelCount = static_cast<std::size_t>(srcW)
        * static_cast<std::size_t>(srcH);
    if (!bounds || src.rgba.size() < pixelCount) {
        return fallback();
    }
    const auto axisA = static_cast<std::size_t>(axes[0]);
    const auto axisB = static_cast<std::size_t>(axes[1]);
    const double minA = bounds->lower[axisA];
    const double minB = bounds->lower[axisB];
    const double spanA = bounds->upper[axisA] - minA;
    const double spanB = bounds->upper[axisB] - minB;
    const auto& logical = nodes.physicalRegion;
    const double pitchA = (logical.upper[axisA] - logical.lower[axisA])
        / static_cast<double>(srcW);
    const double pitchB = (logical.upper[axisB] - logical.lower[axisB])
        / static_cast<double>(srcH);
    if (!(pitchA > 0.0) || !(pitchB > 0.0) || !std::isfinite(pitchA)
        || !std::isfinite(pitchB)) {
        return fallback();
    }

    // Output pitch per axis: that axis's raster pitch over the supersample
    // factor, so each raster cell spans `supersample` output pixels along
    // each axis at its uniform size and its stretched edges are traced
    // rather than quantised. Anisotropic on purpose: a square pitch would
    // spend the pixel budget on the long axis of a thin domain (an ocean
    // 50 km wide and 300 m deep) and starve the short one. A display pixel
    // is therefore the same physical shape as a native raster cell, and the
    // view applies the Physical Size stretch exactly as it does to a
    // Cartesian raster. maxDimension caps both dimensions by one factor.
    const int factor = std::max(1, supersample);
    const double outPitchA = pitchA / static_cast<double>(factor);
    const double outPitchB = pitchB / static_cast<double>(factor);
    int width = std::max(1, static_cast<int>(std::lround(spanA / outPitchA)));
    int height = std::max(1, static_cast<int>(std::lround(spanB / outPitchB)));
    if (width > maxDimension || height > maxDimension) {
        const double scale = static_cast<double>(maxDimension)
            / static_cast<double>(std::max(width, height));
        width = std::max(1, static_cast<int>(std::lround(width * scale)));
        height = std::max(1, static_cast<int>(std::lround(height * scale)));
    }

    out.displayRegion = *bounds;

    out.image.width = width;
    out.image.height = height;
    out.image.strideBytes = width * static_cast<int>(sizeof(std::uint32_t));
    const auto outCount = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height);
    out.image.rgba.assign(outCount, 0U);
    auto source = std::make_shared<std::vector<std::int32_t>>(outCount, -1);

    const double scaleA = static_cast<double>(width) / spanA;
    const double scaleB = static_cast<double>(height) / spanB;
    const auto toPixel = [&](std::size_t node) {
        return Point{(nodes.a[node] - minA) * scaleA, (nodes.b[node] - minB) * scaleB};
    };
    const auto stride = static_cast<std::size_t>(nodes.width);
    for (int row = 0; row < srcH; ++row) {
        for (int col = 0; col < srcW; ++col) {
            const auto n00 = static_cast<std::size_t>(row) * stride
                + static_cast<std::size_t>(col);
            const auto p00 = toPixel(n00);
            const auto p10 = toPixel(n00 + 1);
            const auto p01 = toPixel(n00 + stride);
            const auto p11 = toPixel(n00 + stride + 1);
            const auto pixel = static_cast<std::size_t>(row)
                    * static_cast<std::size_t>(srcW)
                + static_cast<std::size_t>(col);
            const auto colour = src.rgba[pixel];
            const auto index = static_cast<std::int32_t>(pixel);
            fillTriangle(p00, p10, p11, colour, index, out.image, *source);
            fillTriangle(p00, p11, p01, colour, index, out.image, *source);
        }
    }
    out.sourceIndex = std::move(source);
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
