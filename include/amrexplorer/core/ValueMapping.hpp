#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

namespace amrvis {

enum class ColorScale : std::uint8_t { Linear, Logarithmic, SymLogarithmic };

struct ColorScaleConfig {
    ColorScale scale = ColorScale::Linear;
    double linearThreshold = 1.0;
    friend bool operator==(const ColorScaleConfig&,
        const ColorScaleConfig&) = default;
};

// Mapping a field value to a slot of a colour lookup -- a palette slot for a
// slice, a transfer-function entry for a volume. Both renderers map through
// this, because a value must take the same slot in both: a volume rendered
// beside a slice of the same field with the same range is read as the same
// picture, and a colour bar drawn from the palette has to agree with both.
//
// Slot 0 at or below the minimum, the last slot at or above the maximum,
// truncation between -- never rounding, never interpolating.
//
// Defined inline rather than in a .cpp: this runs once per pixel of a slice
// and once per ray sample of a volume, and an out-of-line call there costs
// the volume renderer about seven percent.

// A range with its logarithm taken and its span subtracted once, so the
// per-sample work is a subtract, a divide and a compare. Resolving is the
// only place std::log of the bounds happens: it sets errno, so a compiler
// cannot hoist it out of a pixel or sample loop on its own.
struct ResolvedValueRange {
    double minimum = 0.0;   // already transformed by `scale`
    double span = 1.0;      // maximum - minimum, in the same terms
    ColorScaleConfig scale;
    constexpr ResolvedValueRange() = default;
    constexpr ResolvedValueRange(double minimumIn, double spanIn, ColorScaleConfig scaleConfig)
        : minimum(minimumIn), span(spanIn), scale(scaleConfig) {}
};

inline constexpr double symmetricLogLinearScale = 10.0 / 9.0;

[[nodiscard]] inline double transformedValue(
    double value, const ColorScaleConfig& config) noexcept
{
    if (config.scale == ColorScale::Logarithmic) return std::log(value);
    if (config.scale != ColorScale::SymLogarithmic) return value;
    const auto magnitude = std::abs(value);
    if (magnitude <= config.linearThreshold) {
        return value * symmetricLogLinearScale;
    }
    return std::copysign(config.linearThreshold
            * (symmetricLogLinearScale
                + std::log10(magnitude / config.linearThreshold)), value);
}

[[nodiscard]] inline double inverseTransformedValue(
    double value, const ColorScaleConfig& config) noexcept
{
    if (config.scale == ColorScale::Logarithmic) return std::exp(value);
    if (config.scale != ColorScale::SymLogarithmic) return value;
    const auto limit = config.linearThreshold * symmetricLogLinearScale;
    const auto magnitude = std::abs(value);
    if (magnitude <= limit) return value / symmetricLogLinearScale;
    return std::copysign(config.linearThreshold
            * std::pow(10.0, magnitude / config.linearThreshold
                    - symmetricLogLinearScale), value);
}

// nullopt for a range no value can be mapped through: a non-finite bound, an
// empty or unordered span, a span so wide it is infinite (every value would
// land in slot 0), or a logarithmic range reaching to zero.
[[nodiscard]] inline std::optional<ResolvedValueRange> resolveValueRange(
    double minimum, double maximum, ColorScaleConfig scale = {}) noexcept
{
    // Both bounds are tested for positivity, not just the minimum: this is
    // noexcept and public, so an unordered logarithmic range reaches it, and
    // std::log of a negative maximum raises FE_INVALID and sets errno. The
    // span test below would reject the range anyway -- but not before a build
    // running with feenableexcept(FE_INVALID) had taken SIGFPE.
    if (!std::isfinite(minimum) || !std::isfinite(maximum)
        || (scale.scale == ColorScale::Logarithmic
            && !(minimum > 0.0 && maximum > 0.0))
        || (scale.scale == ColorScale::SymLogarithmic
            && !(scale.linearThreshold > 0.0
                && std::isfinite(scale.linearThreshold)))) {
        return std::nullopt;
    }
    ResolvedValueRange resolved;
    resolved.scale = scale;
    resolved.minimum = transformedValue(minimum, scale);
    const auto top = transformedValue(maximum, scale);
    resolved.span = top - resolved.minimum;
    if (!(resolved.span > 0.0) || !std::isfinite(resolved.span)) {
        return std::nullopt;
    }
    return resolved;
}

// Whether the range can map this value at all: non-finite values, and
// non-positive ones under a logarithmic range, have no slot.
[[nodiscard]] inline bool mappableValue(
    double value, const ResolvedValueRange& range) noexcept
{
    return std::isfinite(value)
        && !(range.scale.scale == ColorScale::Logarithmic && !(value > 0.0));
}

// The slot, for a value mappableValue accepts and a slotCount of at least 1.
//
// The division is deliberately not turned into a multiply by a precomputed
// 1 / span. Hoisting the subtraction is exact -- same operands, same result --
// but the reciprocal is not, and the difference is visible in the picture:
//
//   - At the top of the range, for about one span in seven (e.g. [0, 49]), a
//     value equal to the maximum normalizes to just under 1.0 and truncates
//     into the second-to-last slot while a colour bar shows the last.
//   - In the interior it costs the exact ties. Over [0, 49] the value 24.5
//     divides to exactly 0.5 (slot 126) but multiplies to 0.49999999999999994
//     (slot 125). Only about three interior floats in a hundred thousand
//     shift, but they are the round ones -- midpoints and similar -- which are
//     the ones a reader is most likely to check.
//
// A sweep of *random* spans and values shows no interior disagreement, which
// is what makes this worth a comment: random doubles essentially never land on
// the ties, so sampling does not find them and only the exact cases do. Keep
// the division.
[[nodiscard]] inline int valueSlot(double value, const ResolvedValueRange& range,
    int slotCount) noexcept
{
    const auto mapped = transformedValue(value, range.scale);
    const auto normalized = (mapped - range.minimum) / range.span;
    if (!(normalized > 0.0)) {
        return 0;
    }
    if (!(normalized < 1.0)) {
        return slotCount - 1;
    }
    return static_cast<int>(normalized * static_cast<double>(slotCount - 1));
}

} // namespace amrvis
