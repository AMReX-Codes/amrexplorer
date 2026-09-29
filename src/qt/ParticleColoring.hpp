#pragma once

#include <amrexplorer/io/ParticleReader.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>

namespace amrvis::qt {

// The value range particles are colored over.
struct ParticleColorRange {
    double minimum = 0.0;
    double maximum = 0.0;
    friend bool operator==(const ParticleColorRange&, const ParticleColorRange&) = default;
};

// Whether a value can be placed on the color scale: finite, and positive on a
// logarithmic one.
[[nodiscard]] inline bool particleValuePlaceable(double value, bool logarithmic) noexcept
{
    return std::isfinite(value) && (!logarithmic || value > 0.0);
}

// The range of the placeable values across the samples that carry an
// attribute; none when there is no such value.
[[nodiscard]] inline std::optional<ParticleColorRange> particleValueRange(
    std::span<const ParticleSample> samples, bool logarithmic) noexcept
{
    std::optional<ParticleColorRange> range;
    for (const auto& sample : samples) {
        if (!sample.attribute) {
            continue;
        }
        for (const auto& point : sample.points) {
            if (!particleValuePlaceable(point.value, logarithmic)) {
                continue;
            }
            if (!range) {
                range = ParticleColorRange{point.value, point.value};
            } else {
                range->minimum = std::min(range->minimum, point.value);
                range->maximum = std::max(range->maximum, point.value);
            }
        }
    }
    return range;
}

// Where a value falls on the scale, clamped to [0, 1]; the middle for a flat
// range; none for a value that cannot be placed, which is drawn in its
// species' own color.
[[nodiscard]] inline std::optional<double> particleColorFraction(
    double value, const ParticleColorRange& range, bool logarithmic) noexcept
{
    if (!particleValuePlaceable(value, logarithmic)) {
        return std::nullopt;
    }
    const auto map = [logarithmic](double v) { return logarithmic ? std::log10(v) : v; };
    const auto lower = map(range.minimum);
    const auto upper = map(range.maximum);
    if (!(upper > lower)) {
        return 0.5;
    }
    return std::clamp((map(value) - lower) / (upper - lower), 0.0, 1.0);
}

} // namespace amrvis::qt
