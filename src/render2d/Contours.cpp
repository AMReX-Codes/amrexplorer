#include <amrexplorer/render2d/Contours.hpp>
#include <amrexplorer/render2d/detail/PlaneValidation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace amrvis {

std::vector<double> contourValues(
    double minimum, double maximum, int count, bool logarithmic)
{
    if (count < 1) {
        throw std::invalid_argument("contour count must be positive");
    }
    if (!(minimum < maximum)) {
        throw std::invalid_argument("contour range must have positive extent");
    }
    if (!std::isfinite(minimum) || !std::isfinite(maximum)
        || !std::isfinite(maximum - minimum)) {
        throw std::invalid_argument(
            "contour range must be finite with a finite span");
    }
    if (logarithmic && !(minimum > 0.0)) {
        throw std::invalid_argument("logarithmic contour range must be positive");
    }
    std::vector<double> values(static_cast<std::size_t>(count));
    const double rangeMinimum = logarithmic ? std::log(minimum) : minimum;
    const double rangeMaximum = logarithmic ? std::log(maximum) : maximum;
    const double span = rangeMaximum - rangeMinimum;
    for (int i = 0; i < count; ++i) {
        const auto value = rangeMinimum
            + (0.5 + static_cast<double>(i)) / count * span;
        values[static_cast<std::size_t>(i)] =
            logarithmic ? std::exp(value) : value;
    }
    return values;
}

std::vector<ContourSegment> generateContours(
    const ScalarPlane& plane, const std::vector<double>& values)
{
    detail::validatePlaneStorage(plane, detail::PlaneExtent::AllowEmpty);

    std::vector<ContourSegment> segments;
    if (plane.width < 2 || plane.height < 2) {
        return segments;
    }
    // Collect valid contour values once.
    std::vector<double> finiteValues;
    finiteValues.reserve(values.size());
    for (double value : values) {
        if (std::isfinite(value)) {
            finiteValues.push_back(value);
        }
    }
    if (finiteValues.empty()) {
        return segments;
    }
    // A field whose finite range is degenerate has no meaningful iso-lines.
    // The display range handed to contourValues is padded so minimum < maximum
    // always holds, so it looks healthy even for a constant field — and then
    // the contour levels collapse onto that single value and marching squares
    // marks every edge of every cell as crossed, tiling the image with spurious
    // saddle segments. Detect the flat case from the actual data instead. The
    // Treat variation below 1e-6 of the field's own magnitude as effectively
    // constant. Do not impose an absolute scale floor: fields such as density
    // routinely have meaningful variation entirely below 1e-20.
    double dataMinimum = 0.0;
    double dataMaximum = 0.0;
    bool hasFinite = false;
    for (std::size_t pixel = 0; pixel < plane.values.size(); ++pixel) {
        if (plane.valid[pixel] == 0) {
            continue;
        }
        const auto value = static_cast<double>(plane.values[pixel]);
        if (!std::isfinite(value)) {
            continue;
        }
        if (!hasFinite) {
            dataMinimum = value;
            dataMaximum = value;
            hasFinite = true;
        } else {
            dataMinimum = std::min(dataMinimum, value);
            dataMaximum = std::max(dataMaximum, value);
        }
    }
    if (!hasFinite) {
        return segments;
    }
    const auto scale = std::max(
        std::fabs(dataMinimum), std::fabs(dataMaximum));
    if (dataMaximum - dataMinimum <= 1.0e-6 * scale) {
        return segments;
    }
    // Process cells in the outer loop so each cell's four corner values are
    // loaded once regardless of the contour count — a 10× reduction in
    // memory traffic for ten contours vs. the original value-major order.
    for (int j = 0; j + 1 < plane.height; ++j) {
        for (int i = 0; i + 1 < plane.width; ++i) {
            const auto rowStride = static_cast<std::size_t>(plane.width);
            const auto blIdx = static_cast<std::size_t>(i)
                + static_cast<std::size_t>(j) * rowStride;
            const auto brIdx = blIdx + 1;
            const auto tlIdx = blIdx + rowStride;
            const auto trIdx = tlIdx + 1;
            if (plane.valid[blIdx] == 0 || plane.valid[brIdx] == 0
                || plane.valid[tlIdx] == 0 || plane.valid[trIdx] == 0) {
                continue;
            }
            const double bl = plane.values[blIdx];
            const double br = plane.values[brIdx];
            const double tl = plane.values[tlIdx];
            const double tr = plane.values[trIdx];
            if (!std::isfinite(bl) || !std::isfinite(br)
                || !std::isfinite(tl) || !std::isfinite(tr)) {
                continue;
            }
            const auto x0 = static_cast<float>(i);
            const auto x1 = static_cast<float>(i + 1);
            const auto y0 = static_cast<float>(j);
            const auto y1 = static_cast<float>(j + 1);
            // Precompute edge-interpolation denominators (inverse).
            const double invDYl = (tl != bl) ? 1.0 / (tl - bl) : 0.0;
            const double invDYr = (tr != br) ? 1.0 / (tr - br) : 0.0;
            const double invDXb = (br != bl) ? 1.0 / (br - bl) : 0.0;
            const double invDXt = (tr != tl) ? 1.0 / (tr - tl) : 0.0;
            for (double value : finiteValues) {
                const bool left   = (bl <= value && value <= tl)
                    || (bl >= value && value >= tl);
                const bool right  = (br <= value && value <= tr)
                    || (br >= value && value >= tr);
                const bool bottom = (bl <= value && value <= br)
                    || (bl >= value && value >= br);
                const bool top    = (tl <= value && value <= tr)
                    || (tl >= value && value >= tr);
                if (!left && !right && !bottom && !top) {
                    continue;
                }

                float xL = x0, yL = y0;
                float xR = x1, yR = y1;
                float xB = x0, yB = y0;
                float xT = x1, yT = y1;
                if (left)   yL = y0 + static_cast<float>((value - bl) * invDYl);
                if (right)  yR = y0 + static_cast<float>((value - br) * invDYr);
                if (bottom) xB = x0 + static_cast<float>((value - bl) * invDXb);
                if (top)    xT = x0 + static_cast<float>((value - tl) * invDXt);

                const auto emit = [&](float ax, float ay, float bx, float by) {
                    segments.push_back({ax, ay, bx, by, value});
                };

                if (left && right && bottom && top) {
                    const double center = (bl + br + tl + tr) / 4.0;
                    if ((bl > value) != (center > value)) {
                        emit(xL, yL, xB, yB);
                        emit(xT, yT, xR, yR);
                    } else {
                        emit(xL, yL, xT, yT);
                        emit(xB, yB, xR, yR);
                    }
                } else if (top && bottom) {
                    emit(xT, yT, xB, yB);
                } else if (left) {
                    if (right) emit(xL, yL, xR, yR);
                    else if (top) emit(xL, yL, xT, yT);
                    else if (bottom) emit(xL, yL, xB, yB);
                } else if (right) {
                    if (top) emit(xR, yR, xT, yT);
                    else if (bottom) emit(xR, yR, xB, yB);
                }
            }
        }
    }
    // Segments are interleaved by cell, not grouped by value. Sort so
    // chainSegments can group contiguous same-value runs.
    std::sort(segments.begin(), segments.end(),
        [](const ContourSegment& a, const ContourSegment& b) {
            return a.value < b.value;
        });
    return segments;
}

namespace {

// Key built from the float bit patterns of a point, so endpoints match only
// when they are bit-identical. Shared cell-edge crossings are computed from
// the same corner pair by the same formula in both adjacent cells, so
// matching endpoints are bit-exact; no epsilon tolerance is needed.
std::uint64_t pointKey(float x, float y) noexcept
{
    std::uint32_t xBits = 0;
    std::uint32_t yBits = 0;
    std::memcpy(&xBits, &x, sizeof(xBits));
    std::memcpy(&yBits, &y, sizeof(yBits));
    return (static_cast<std::uint64_t>(xBits) << 32) | yBits;
}

struct EndpointRef {
    std::size_t segment = 0;
    int end = 0;  // 0 = segment start, 1 = segment end
};

std::vector<ContourPolyline> chainSegments(
    const ContourSegment* segments, std::size_t count)
{
    std::unordered_multimap<std::uint64_t, EndpointRef> byEndpoint;
    for (std::size_t i = 0; i < count; ++i) {
        byEndpoint.emplace(
            pointKey(segments[i].x0, segments[i].y0), EndpointRef{i, 0});
        byEndpoint.emplace(
            pointKey(segments[i].x1, segments[i].y1), EndpointRef{i, 1});
    }
    std::vector<bool> used(count, false);
    const auto takeAt = [&](std::uint64_t key) -> std::optional<EndpointRef> {
        const auto range = byEndpoint.equal_range(key);
        for (auto it = range.first; it != range.second; ++it) {
            if (!used[it->second.segment]) {
                return it->second;
            }
        }
        return std::nullopt;
    };

    std::vector<ContourPolyline> polylines;
    for (std::size_t seed = 0; seed < count; ++seed) {
        if (used[seed]) {
            continue;
        }
        used[seed] = true;
        ContourPolyline polyline;
        polyline.value = segments[seed].value;
        polyline.points.push_back({segments[seed].x0, segments[seed].y0});
        polyline.points.push_back({segments[seed].x1, segments[seed].y1});
        // Grow the chain in both directions, appending the far endpoint of
        // each unused segment that touches the chain's current end.
        for (;;) {
            const auto& back = polyline.points.back();
            const auto next = takeAt(pointKey(back[0], back[1]));
            if (!next.has_value()) {
                break;
            }
            used[next->segment] = true;
            const auto& segment = segments[next->segment];
            if (next->end == 0) {
                polyline.points.push_back({segment.x1, segment.y1});
            } else {
                polyline.points.push_back({segment.x0, segment.y0});
            }
        }
        for (;;) {
            const auto& front = polyline.points.front();
            const auto next = takeAt(pointKey(front[0], front[1]));
            if (!next.has_value()) {
                break;
            }
            used[next->segment] = true;
            const auto& segment = segments[next->segment];
            if (next->end == 0) {
                polyline.points.insert(polyline.points.begin(),
                    {segment.x1, segment.y1});
            } else {
                polyline.points.insert(polyline.points.begin(),
                    {segment.x0, segment.y0});
            }
        }
        // A chain that returns to its start is a closed loop; drop the
        // duplicated closing point so the ring lists each vertex once.
        if (polyline.points.size() > 1) {
            const auto& first = polyline.points.front();
            const auto& last = polyline.points.back();
            if (pointKey(first[0], first[1]) == pointKey(last[0], last[1])) {
                polyline.closed = true;
                polyline.points.pop_back();
            }
        }
        polylines.push_back(std::move(polyline));
    }
    return polylines;
}

// One Chaikin corner-cutting pass: every segment (a, b) emits the points at
// 1/4 and 3/4 along it. Open chains keep their first and last point fixed
// (only interior corners are cut); closed loops cut every corner, wrapping
// around. Cuts are evaluated in double so each result rounds once.
void chaikinPass(ContourPolyline& polyline)
{
    const auto& points = polyline.points;
    if (points.size() < 2) {
        return;
    }
    const auto cut = [](const std::array<float, 2>& a,
        const std::array<float, 2>& b, double weight) {
        return std::array<float, 2>{
            static_cast<float>(weight * a[0] + (1.0 - weight) * b[0]),
            static_cast<float>(weight * a[1] + (1.0 - weight) * b[1])};
    };
    std::vector<std::array<float, 2>> smoothed;
    smoothed.reserve(points.size() * 2);
    if (polyline.closed) {
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto& next = points[(i + 1) % points.size()];
            smoothed.push_back(cut(points[i], next, 0.75));
            smoothed.push_back(cut(points[i], next, 0.25));
        }
    } else {
        smoothed.push_back(points.front());
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            smoothed.push_back(cut(points[i], points[i + 1], 0.75));
            smoothed.push_back(cut(points[i], points[i + 1], 0.25));
        }
        smoothed.push_back(points.back());
    }
    polyline.points = std::move(smoothed);
}

} // namespace

std::vector<ContourPolyline> generateContourPolylines(
    const ScalarPlane& plane, const std::vector<double>& values,
    int smoothIterations)
{
    const auto segments = generateContours(plane, values);
    std::vector<ContourPolyline> polylines;
    // generateContours emits segments grouped by value in `values` order;
    // chain each group separately so different levels never join.
    std::size_t begin = 0;
    while (begin < segments.size()) {
        std::size_t end = begin + 1;
        while (end < segments.size()
            && segments[end].value == segments[begin].value) {
            ++end;
        }
        auto group = chainSegments(segments.data() + begin, end - begin);
        polylines.insert(polylines.end(),
            std::make_move_iterator(group.begin()),
            std::make_move_iterator(group.end()));
        begin = end;
    }
    for (int iteration = 0; iteration < smoothIterations; ++iteration) {
        for (auto& polyline : polylines) {
            chaikinPass(polyline);
        }
    }
    return polylines;
}

std::vector<ContourPolyline> contourPolylinesForDisplay(
    const ScalarPlane& finePlane, int fineFactor,
    const std::vector<double>& values, int displayWidth, int displayHeight)
{
    if (fineFactor < 1) {
        throw std::invalid_argument("fine factor must be at least 1");
    }
    // The contour plane is already at contour resolution; one Chaikin pass
    // softens the cell-scale corners.
    auto polylines = generateContourPolylines(finePlane, values, 1);
    if (finePlane.width < 1 || finePlane.height < 1) {
        return polylines;
    }
    // Recover the original (pre-refinement) dimensions the fine plane was
    // produced from, then map fine coordinates into display pixel space: fine
    // coordinate f is original sample coordinate f / fineFactor, and original
    // sample center j maps to display pixel
    // ((j + 0.5) * display / original) - 0.5, cell-center to cell-center.
    const auto originalWidth = (finePlane.width - 1) / fineFactor + 1;
    const auto originalHeight = (finePlane.height - 1) / fineFactor + 1;
    const auto scaleX = static_cast<double>(displayWidth)
        / (static_cast<double>(fineFactor) * static_cast<double>(originalWidth));
    const auto scaleY = static_cast<double>(displayHeight)
        / (static_cast<double>(fineFactor) * static_cast<double>(originalHeight));
    const auto offsetX = 0.5 * (static_cast<double>(displayWidth)
        / static_cast<double>(originalWidth) - 1.0);
    const auto offsetY = 0.5 * (static_cast<double>(displayHeight)
        / static_cast<double>(originalHeight) - 1.0);
    for (auto& polyline : polylines) {
        for (auto& point : polyline.points) {
            point[0] = static_cast<float>(
                scaleX * static_cast<double>(point[0]) + offsetX);
            point[1] = static_cast<float>(
                scaleY * static_cast<double>(point[1]) + offsetY);
        }
    }
    return polylines;
}

} // namespace amrvis
