#pragma once

#include <QPointF>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace amrvis::qt {

// Follow a pan only as far as there are samples beneath its fixed anchor.
// A companion can have a narrower footprint beyond the interface; clamping
// to the pair's bounding box alone would let the crosshair enter empty space.
inline QPointF clampScanPath(const QPointF& start, const QPointF& wanted,
    const std::vector<QRectF>& domains)
{
    const auto delta = wanted - start;
    std::vector<std::pair<double, double>> intervals;
    for (const auto& rect : domains) {
        double enter = 0.0, leave = 1.0;
        const auto clip = [&](double origin, double step, double low, double high) {
            if (step == 0.0) return origin >= low && origin <= high;
            auto a = (low - origin) / step;
            auto b = (high - origin) / step;
            if (a > b) std::swap(a, b);
            enter = std::max(enter, a);
            leave = std::min(leave, b);
            return enter <= leave;
        };
        if (clip(start.x(), delta.x(), rect.left(), rect.right())
            && clip(start.y(), delta.y(), rect.top(), rect.bottom())) {
            intervals.emplace_back(enter, leave);
        }
    }
    std::sort(intervals.begin(), intervals.end());
    double reach = 0.0;
    for (const auto& [enter, leave] : intervals) {
        if (enter > reach + 1.e-12) break;
        reach = std::max(reach, leave);
    }
    return start + std::clamp(reach, 0.0, 1.0) * delta;
}

} // namespace amrvis::qt
