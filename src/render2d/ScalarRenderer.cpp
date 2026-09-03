#include <amrexplorer/render2d/ScalarRenderer.hpp>
#include <amrexplorer/render2d/detail/PlaneValidation.hpp>

#include <amrexplorer/core/ValueMapping.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace amrvis {
namespace {

constexpr std::array<std::array<double, 3>, 6> viridis{{
    {{68.0, 1.0, 84.0}},
    {{59.0, 82.0, 139.0}},
    {{33.0, 145.0, 140.0}},
    {{94.0, 201.0, 98.0}},
    {{170.0, 220.0, 50.0}},
    {{253.0, 231.0, 37.0}}
}};

} // namespace

std::array<std::uint8_t, 3> sampleViridis(double normalized) noexcept
{
    normalized = std::clamp(normalized, 0.0, 1.0);
    const auto scaled = normalized * static_cast<double>(viridis.size() - 1);
    const auto lower = static_cast<std::size_t>(scaled);
    const auto upper = std::min(lower + 1, viridis.size() - 1);
    const auto fraction = scaled - static_cast<double>(lower);

    std::array<std::uint8_t, 3> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
        const auto value = viridis[lower][channel]
            + fraction * (viridis[upper][channel] - viridis[lower][channel]);
        result[channel] = static_cast<std::uint8_t>(std::lround(value));
    }
    return result;
}

ImageBuffer renderScalarPlane(
    const ScalarPlane& plane, const ScalarRenderSettings& settings)
{
    const auto scale = effectiveColorScale(settings.logarithmic, settings.scale);
    // Check order is pinned by the unit test: positive extent, then the
    // stride representation, then the shared storage-match rule (extent is
    // already vetted here, so AllowEmpty cannot actually pass an empty plane).
    if (plane.width <= 0 || plane.height <= 0) {
        throw std::invalid_argument("scalar plane dimensions must be positive");
    }
    if (plane.width > std::numeric_limits<int>::max()
            / static_cast<int>(sizeof(std::uint32_t))) {
        throw std::overflow_error("scalar plane row stride exceeds the image representation");
    }
    detail::validatePlaneStorage(plane, detail::PlaneExtent::AllowEmpty);
    if (!(settings.minimum < settings.maximum)) {
        throw std::invalid_argument("scalar render range must have positive extent");
    }
    if (!std::isfinite(settings.minimum) || !std::isfinite(settings.maximum)
        || !std::isfinite(settings.maximum - settings.minimum)) {
        throw std::invalid_argument(
            "scalar render range must be finite with a finite span");
    }
    if (scale.scale == ColorScale::Logarithmic && !(settings.minimum > 0.0)) {
        throw std::invalid_argument("logarithmic scalar range must be positive");
    }
    if (scale.scale == ColorScale::SymLogarithmic
        && !(scale.linearThreshold > 0.0 && std::isfinite(scale.linearThreshold))) {
        throw std::invalid_argument(
            "symmetric-log scalar threshold must be finite and positive");
    }

    // The checks above name the three ways a range is refused; this resolves
    // the mapping itself, shared with the volume ray caster so a value takes
    // the same slot in a slice and in a volume of the same field.
    const auto range = resolveValueRange(
        settings.minimum, settings.maximum, scale);
    if (!range) {
        // Everything the checks above cover has already thrown, so the only
        // way to arrive here is a logarithmic range so narrow that both
        // bounds share a logarithm -- [1e300, nextafter(1e300)] differ by a
        // relative 1e-16, far under an ulp of log(1e300). The span the slots
        // would be spread over is zero, so there is no mapping to make.
        if (scale.scale == ColorScale::Logarithmic) {
            throw std::invalid_argument(
                "logarithmic scalar range is too narrow: its bounds have the same logarithm");
        }
        throw std::invalid_argument(
            "scalar range is too narrow after applying its color scale");
    }

    ImageBuffer image;
    image.width = plane.width;
    image.height = plane.height;
    image.strideBytes = plane.width * static_cast<int>(sizeof(std::uint32_t));
    image.rgba.resize(plane.values.size());
    const Palette& palette = settings.palette != nullptr
        ? *settings.palette : builtinPalette(BuiltinPalette::Rainbow);

    // Palette::argb reassembles its result from three stored bytes on every
    // call, which at the 4096 output cap is up to 16.7 million times for a
    // palette of 253 colors. Resolve them once and index instead. valueSlot
    // clips at or below zero to the first data slot (NaN fails that test too,
    // though NaN never reaches here), at or above one to the last, and
    // otherwise truncates -- never rounds, never interpolates -- into a slot,
    // which is argb's own behaviour.
    std::array<std::uint32_t, Palette::colorSlots> slots{};
    for (int index = 0; index < Palette::colorSlots; ++index) {
        slots[static_cast<std::size_t>(index)]
            = palette.slotArgb(Palette::paletteStart + index);
    }

    for (std::size_t pixel = 0; pixel < image.rgba.size(); ++pixel) {
        if (plane.valid[pixel] == 0) {
            image.rgba[pixel] = settings.invalidColor;
            continue;
        }
        const auto value = static_cast<double>(plane.values[pixel]);
        if (!mappableValue(value, *range)) {
            image.rgba[pixel] = settings.nanColor;
            continue;
        }
        image.rgba[pixel] = slots[static_cast<std::size_t>(
            valueSlot(value, *range, Palette::colorSlots))];
    }
    return image;
}

} // namespace amrvis
