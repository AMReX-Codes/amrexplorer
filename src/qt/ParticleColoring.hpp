#pragma once

#include <amrexplorer/core/ValueMapping.hpp>
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

// The range of the placeable values across the samples `colored` accepts;
// none when there is no such value.
template <class Colored>
[[nodiscard]] std::optional<ParticleColorRange> particleValueRange(
    std::span<const ParticleSample> samples, bool logarithmic, Colored colored)
{
    std::optional<ParticleColorRange> range;
    for (const auto& sample : samples) {
        if (!colored(sample)) {
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

// Likewise across every sample that carries an attribute.
[[nodiscard]] inline std::optional<ParticleColorRange> particleValueRange(
    std::span<const ParticleSample> samples, bool logarithmic)
{
    return particleValueRange(samples, logarithmic,
        [](const ParticleSample& sample) { return sample.attribute.has_value(); });
}

// The palette slot a value takes, through the mapping the slices use
// (core/ValueMapping.hpp); a flat range puts every value in the middle.
class ParticleColorScale {
public:
    ParticleColorScale(const ParticleColorRange& range, bool logarithmic,
        int slotCount) noexcept
        : m_resolved(resolveValueRange(range.minimum, range.maximum, logarithmic))
        , m_flat(std::isfinite(range.minimum) && range.minimum == range.maximum)
        , m_logarithmic(logarithmic)
        , m_slotCount(slotCount)
    {
    }

    // None for a value it cannot place, which keeps its species' color.
    [[nodiscard]] std::optional<int> slot(double value) const noexcept
    {
        if (!particleValuePlaceable(value, m_logarithmic)) {
            return std::nullopt;
        }
        if (m_resolved) {
            return valueSlot(value, *m_resolved, m_slotCount);
        }
        if (m_flat) {
            return (m_slotCount - 1) / 2;
        }
        return std::nullopt;
    }

private:
    std::optional<ResolvedValueRange> m_resolved;
    bool m_flat = false;
    bool m_logarithmic = false;
    int m_slotCount = 1;
};

} // namespace amrvis::qt
