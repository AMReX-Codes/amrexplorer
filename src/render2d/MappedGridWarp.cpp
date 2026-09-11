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

// Subsamples per pixel edge: 3x3 positions at (n + (k + 0.5) / 3), the
// middle one being the pixel centre, which is what the source index records.
constexpr int subsamples = 3;
constexpr int centreSubsample = 1;
constexpr int samplesPerPixel = subsamples * subsamples;

// Per output pixel: the colour channels summed over the subsamples cells
// covered, how many they covered, and the cell covering the centre.
struct Coverage {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> red;
    std::vector<std::uint32_t> green;
    std::vector<std::uint32_t> blue;
    std::vector<std::uint16_t> count;
    std::vector<std::int32_t> centre;
};

// Accumulates the triangle into every subsample it covers (inside or on an
// edge). Edge functions are normalised by the triangle's orientation so
// either winding works; a zero-area triangle covers nothing and is skipped.
// A subsample on a shared edge counts for both cells rather than neither:
// the sum then holds two colours that both belong there, and the count
// exceeds the nominal nine, which the final division absorbs. A transparent
// source cell (an invalid sample) records its index at the centre but adds
// no colour, so the pixel stays as clear as the cell.
void fillTriangle(Point p0, Point p1, Point p2, std::uint32_t colour,
    std::int32_t index, Coverage& out)
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
    // Pixel n covers [n, n+1); its subsamples lie strictly inside. Clamped as
    // doubles before the cast: a window deep inside a cell puts the cell's
    // corners beyond any int.
    if (xHi < 0.0 || yHi < 0.0 || xLo >= out.width || yLo >= out.height) {
        return;
    }
    const auto first = [](double lo) {
        return static_cast<int>(std::max(0.0, std::floor(lo)));
    };
    const auto last = [](double hi, int size) {
        return static_cast<int>(std::min(static_cast<double>(size - 1), std::floor(hi)));
    };
    const int colStart = first(xLo);
    const int colEnd = last(xHi, out.width);
    const int rowStart = first(yLo);
    const int rowEnd = last(yHi, out.height);
    // A thousandth of a pixel of slack perpendicular to each edge, so a
    // subsample on a shared edge still counts for both cells. Scaled by the
    // edge's length, not the area, which deep zoom blows up to whole pixels.
    constexpr double edgeSlack = 1e-3;
    const auto slack = [](Point from, Point to) {
        return edgeSlack * std::hypot(to.x - from.x, to.y - from.y);
    };
    const double tol0 = slack(p0, p1);
    const double tol1 = slack(p1, p2);
    const double tol2 = slack(p2, p0);
    const bool opaque = ((colour >> 24U) & 0xFFU) != 0U;
    const auto r = (colour >> 16U) & 0xFFU;
    const auto g = (colour >> 8U) & 0xFFU;
    const auto b = colour & 0xFFU;
    const auto inside = [&](double cx, double cy) {
        const double e0 = orientation
            * ((p1.x - p0.x) * (cy - p0.y) - (cx - p0.x) * (p1.y - p0.y));
        const double e1 = orientation
            * ((p2.x - p1.x) * (cy - p1.y) - (cx - p1.x) * (p2.y - p1.y));
        const double e2 = orientation
            * ((p0.x - p2.x) * (cy - p2.y) - (cx - p2.x) * (p0.y - p2.y));
        return !(e0 < -tol0 || e1 < -tol1 || e2 < -tol2);
    };
    constexpr double step = 1.0 / subsamples;
    for (int row = rowStart; row <= rowEnd; ++row) {
        for (int col = colStart; col <= colEnd; ++col) {
            const auto offset = static_cast<std::size_t>(row)
                    * static_cast<std::size_t>(out.width)
                + static_cast<std::size_t>(col);
            unsigned covered = 0;
            for (int sy = 0; sy < subsamples; ++sy) {
                const double cy = static_cast<double>(row) + (sy + 0.5) * step;
                for (int sx = 0; sx < subsamples; ++sx) {
                    const double cx = static_cast<double>(col) + (sx + 0.5) * step;
                    if (!inside(cx, cy)) {
                        continue;
                    }
                    ++covered;
                    if (sx == centreSubsample && sy == centreSubsample) {
                        out.centre[offset] = index;
                    }
                }
            }
            if (covered == 0 || !opaque) {
                continue;
            }
            out.red[offset] += r * covered;
            out.green[offset] += g * covered;
            out.blue[offset] += b * covered;
            out.count[offset] = static_cast<std::uint16_t>(
                std::min<unsigned>(out.count[offset] + covered, 0xFFFFU));
        }
    }
}

// A box on the two in-plane axes: ordered and finite on both.
bool validOnAxes(const RealBox& box, std::array<int, 2> axes)
{
    for (const auto axis : axes) {
        const auto i = static_cast<std::size_t>(axis);
        if (!std::isfinite(box.lower[i]) || !std::isfinite(box.upper[i])
            || !(box.lower[i] < box.upper[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<RealBox> mappedGridWindow(const MappedGridPlane& nodes,
    std::array<int, 2> axes, const RealBox& window)
{
    const auto bounds = mappedGridDisplayBounds(nodes, axes);
    if (!bounds || !validOnAxes(window, axes)) {
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

    out.displayRegion = *drawn;
    out.mappedBounds = *bounds;
    out.image.width = width;
    out.image.height = height;
    out.image.strideBytes = width * static_cast<int>(sizeof(std::uint32_t));
    const auto outCount = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height);
    out.image.rgba.assign(outCount, 0U);

    Coverage coverage;
    coverage.width = width;
    coverage.height = height;
    coverage.red.assign(outCount, 0U);
    coverage.green.assign(outCount, 0U);
    coverage.blue.assign(outCount, 0U);
    coverage.count.assign(outCount, 0U);
    coverage.centre.assign(outCount, -1);

    const double scaleA = static_cast<double>(width) / spanA;
    const double scaleB = static_cast<double>(height) / spanB;
    const auto toPixel = [&](std::size_t node) {
        return Point{(nodes.a[node] - minA) * scaleA, (nodes.b[node] - minB) * scaleB};
    };
    const auto stride = static_cast<std::size_t>(nodes.width);
    const double maxX = static_cast<double>(width);
    const double maxY = static_cast<double>(height);
    for (int row = 0; row < srcH; ++row) {
        for (int col = 0; col < srcW; ++col) {
            const auto n00 = static_cast<std::size_t>(row) * stride
                + static_cast<std::size_t>(col);
            const auto p00 = toPixel(n00);
            const auto p10 = toPixel(n00 + 1);
            const auto p01 = toPixel(n00 + stride);
            const auto p11 = toPixel(n00 + stride + 1);
            // Cells outside the window are skipped without touching a pixel.
            const double xLo = std::min({p00.x, p10.x, p01.x, p11.x});
            const double xHi = std::max({p00.x, p10.x, p01.x, p11.x});
            const double yLo = std::min({p00.y, p10.y, p01.y, p11.y});
            const double yHi = std::max({p00.y, p10.y, p01.y, p11.y});
            if (xHi < 0.0 || yHi < 0.0 || xLo > maxX || yLo > maxY) {
                continue;
            }
            const auto pixel = static_cast<std::size_t>(row)
                    * static_cast<std::size_t>(srcW)
                + static_cast<std::size_t>(col);
            const auto colour = src.rgba[pixel];
            const auto index = static_cast<std::int32_t>(pixel);
            fillTriangle(p00, p10, p11, colour, index, coverage);
            fillTriangle(p00, p11, p01, colour, index, coverage);
        }
    }

    for (std::size_t offset = 0; offset < outCount; ++offset) {
        const unsigned count = coverage.count[offset];
        if (count == 0) {
            continue;
        }
        const auto channel = [count](std::uint32_t sum) {
            return static_cast<std::uint32_t>(
                std::min<std::uint32_t>(255U, (sum + count / 2) / count));
        };
        const auto alpha = static_cast<std::uint32_t>(std::min<unsigned>(
            255U, (count * 255U + samplesPerPixel / 2) / samplesPerPixel));
        out.image.rgba[offset] = (alpha << 24U)
            | (channel(coverage.red[offset]) << 16U)
            | (channel(coverage.green[offset]) << 8U)
            | channel(coverage.blue[offset]);
    }
    out.sourceIndex = std::make_shared<const std::vector<std::int32_t>>(
        std::move(coverage.centre));
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
