#include <amrexplorer/render2d/ScalarRenderer.hpp>
#include <amrexplorer/render2d/detail/PlaneValidation.hpp>

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
    if (settings.logarithmic && !(settings.minimum > 0.0)) {
        throw std::invalid_argument("logarithmic scalar range must be positive");
    }

    const auto rangeMinimum = settings.logarithmic
        ? std::log(settings.minimum) : settings.minimum;
    const auto rangeMaximum = settings.logarithmic
        ? std::log(settings.maximum) : settings.maximum;

    ImageBuffer image;
    image.width = plane.width;
    image.height = plane.height;
    image.strideBytes = plane.width * static_cast<int>(sizeof(std::uint32_t));
    image.rgba.resize(plane.values.size());
    const Palette& palette = settings.palette != nullptr
        ? *settings.palette : builtinPalette(BuiltinPalette::Rainbow);

    // Palette::argb reassembles its result from three stored bytes on every
    // call, which at the 4096 output cap is up to 16.7 million times for a
    // palette of 253 colors. Resolve them once and index instead. The three
    // cases below are argb's own, kept in the same order: it clips at or below
    // zero to the first data slot (NaN fails that test too, though NaN never
    // reaches here), at or above one to the last, and otherwise truncates --
    // never rounds, never interpolates -- into a slot.
    std::array<std::uint32_t, Palette::colorSlots> slots{};
    for (int index = 0; index < Palette::colorSlots; ++index) {
        slots[static_cast<std::size_t>(index)]
            = palette.slotArgb(Palette::paletteStart + index);
    }
    constexpr auto lastSlot = static_cast<std::size_t>(Palette::colorSlots - 1);
    const auto scale = static_cast<double>(Palette::colorSlots - 1);

    // The subtraction is hoisted; the division is not. Hoisting `span` is
    // exact -- same operands, same result, once instead of per pixel -- but
    // turning the per-pixel divide into a multiply by 1/span is not, and the
    // difference is visible in the picture:
    //
    //   - At the top of the range, for about one span in seven (e.g. [0, 49]),
    //     a value equal to the maximum normalizes to just under 1.0 and
    //     truncates into the second-to-last slot while the color bar, which
    //     goes through Palette::argb, shows the last.
    //   - In the interior it costs the exact ties. Over [0, 49] the value 24.5
    //     divides to exactly 0.5 (slot 126) but multiplies to
    //     0.49999999999999994 (slot 125). Only about three interior floats in
    //     a hundred thousand shift, but they are the round ones -- midpoints
    //     and similar -- which are the ones a reader is most likely to check.
    //
    // A sweep of *random* spans and values shows no interior disagreement,
    // which is what makes this worth a comment: random doubles essentially
    // never land on the ties, so sampling does not find them and only the
    // exact cases do. Keep the division.
    const auto span = rangeMaximum - rangeMinimum;

    for (std::size_t pixel = 0; pixel < image.rgba.size(); ++pixel) {
        if (plane.valid[pixel] == 0) {
            image.rgba[pixel] = settings.invalidColor;
            continue;
        }
        const auto value = static_cast<double>(plane.values[pixel]);
        if (!std::isfinite(value)
            || (settings.logarithmic && !(value > 0.0))) {
            image.rgba[pixel] = settings.nanColor;
            continue;
        }
        const auto mapped = settings.logarithmic ? std::log(value) : value;
        const auto normalized = (mapped - rangeMinimum) / span;
        if (!(normalized > 0.0)) {
            image.rgba[pixel] = slots[0];
        } else if (!(normalized < 1.0)) {
            image.rgba[pixel] = slots[lastSlot];
        } else {
            image.rgba[pixel]
                = slots[static_cast<std::size_t>(normalized * scale)];
        }
    }
    return image;
}

} // namespace amrvis
