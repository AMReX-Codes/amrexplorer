#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/render2d/ImageBuffer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

// The forward rasterizer behind the display warps (MappedGridWarp,
// SphericalWarp): every raster cell is drawn as the quadrilateral of its
// corner positions, split into two triangles, into an output whose pixels
// are the screen's. Each pixel takes 3x3 subsamples: the covering cells'
// mean colour with the covered fraction as alpha, so an edge crossing a
// pixel is antialiased and the domain's boundary fades rather than steps,
// and the cell drawn at the pixel's centre is recorded for the probe.
namespace amrvis::detail {

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
inline void fillTriangle(Point p0, Point p1, Point p2, std::uint32_t colour,
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
inline bool validOnAxes(const RealBox& box, std::array<int, 2> axes)
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

struct QuadRasterOutput {
    // ARGB32, row 0 = bottom; pixels no cell covers are transparent.
    ImageBuffer image;
    // Per pixel, the source raster pixel drawn at its centre, or -1.
    std::shared_ptr<const std::vector<std::int32_t>> sourceIndex;
};

// The cells of a raster to draw: columns [colBegin, colEnd) and rows
// [rowBegin, rowEnd). The default is every cell.
struct CellRange {
    int colBegin = 0;
    int colEnd = std::numeric_limits<int>::max();
    int rowBegin = 0;
    int rowEnd = std::numeric_limits<int>::max();
};

// Draws the cells of `src` (row-major, row 0 = bottom) in `cells` into a
// width x height output. corner(col, row) gives the output-pixel position of
// the raster corner at fractional (col, row), integer values being the cell
// corners; a cell is cut into subdivisions[0] x subdivisions[1] sub-quads
// along its two axes, all drawn with the cell's colour and index, so a
// mapping curved between the corners is followed rather than chorded. A
// cell whose four corners lie more than cullPad pixels outside the output
// is skipped: the pad is how far a curved edge can bulge past its chord.
// A caller that knows which cells can reach the output narrows `cells`,
// sparing the corners of the rest.
template <class Corner>
QuadRasterOutput rasterizeCells(const ImageBuffer& src, int width, int height,
    std::array<int, 2> subdivisions, double cullPad, Corner corner,
    CellRange cells = {})
{
    QuadRasterOutput out;
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

    const int ka = std::max(1, subdivisions[0]);
    const int kb = std::max(1, subdivisions[1]);
    const double maxX = static_cast<double>(width) + cullPad;
    const double maxY = static_cast<double>(height) + cullPad;
    const int srcW = src.width;
    const int srcH = src.height;
    const int colBegin = std::clamp(cells.colBegin, 0, srcW);
    const int colEnd = std::clamp(cells.colEnd, colBegin, srcW);
    const int rowBegin = std::clamp(cells.rowBegin, 0, srcH);
    const int rowEnd = std::clamp(cells.rowEnd, rowBegin, srcH);
    for (int row = rowBegin; row < rowEnd; ++row) {
        for (int col = colBegin; col < colEnd; ++col) {
            const auto p00 = corner(static_cast<double>(col), static_cast<double>(row));
            const auto p10 = corner(static_cast<double>(col + 1), static_cast<double>(row));
            const auto p01 = corner(static_cast<double>(col), static_cast<double>(row + 1));
            const auto p11 = corner(static_cast<double>(col + 1), static_cast<double>(row + 1));
            // Cells outside the window are skipped without touching a pixel.
            const double xLo = std::min({p00.x, p10.x, p01.x, p11.x});
            const double xHi = std::max({p00.x, p10.x, p01.x, p11.x});
            const double yLo = std::min({p00.y, p10.y, p01.y, p11.y});
            const double yHi = std::max({p00.y, p10.y, p01.y, p11.y});
            if (xHi < -cullPad || yHi < -cullPad || xLo > maxX || yLo > maxY) {
                continue;
            }
            const auto pixel = static_cast<std::size_t>(row)
                    * static_cast<std::size_t>(srcW)
                + static_cast<std::size_t>(col);
            const auto colour = src.rgba[pixel];
            const auto index = static_cast<std::int32_t>(pixel);
            if (ka == 1 && kb == 1) {
                fillTriangle(p00, p10, p11, colour, index, coverage);
                fillTriangle(p00, p11, p01, colour, index, coverage);
                continue;
            }
            // Sub-corners at exact fractions: a neighbour drawing the shared
            // edge asks for the same (col, row) and lands on the same points.
            for (int j = 0; j < kb; ++j) {
                const double b0 = row + static_cast<double>(j) / kb;
                const double b1 = row + static_cast<double>(j + 1) / kb;
                for (int i = 0; i < ka; ++i) {
                    const double a0 = col + static_cast<double>(i) / ka;
                    const double a1 = col + static_cast<double>(i + 1) / ka;
                    const auto q00 = corner(a0, b0);
                    const auto q10 = corner(a1, b0);
                    const auto q01 = corner(a0, b1);
                    const auto q11 = corner(a1, b1);
                    fillTriangle(q00, q10, q11, colour, index, coverage);
                    fillTriangle(q00, q11, q01, colour, index, coverage);
                }
            }
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

} // namespace amrvis::detail
