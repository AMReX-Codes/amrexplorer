#include <amrexplorer/render2d/ScalarRenderer.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// Renders and expects a std::invalid_argument whose what() contains `needle`.
// Matching the message (not just the type) pins which check fired -- the
// renderer validates in a fixed order (dimensions, stride, storage, range
// extent, range finiteness, log positivity), which cases below rely on.
void expectRejected(const amrvis::ScalarPlane& plane,
    const amrvis::ScalarRenderSettings& settings,
    const char* needle, const char* what)
{
    try {
        (void)amrvis::renderScalarPlane(plane, settings);
    } catch (const std::invalid_argument& error) {
        if (std::string(error.what()).find(needle) != std::string::npos) {
            return;
        }
        std::cerr << "FAILED: " << what << " threw the wrong message: "
                  << error.what() << '\n';
        std::exit(1);
    }
    std::cerr << "FAILED: " << what << " (no exception thrown)\n";
    std::exit(1);
}

} // namespace

int main()
{
    amrvis::ScalarPlane plane;
    plane.width = 2;
    plane.height = 2;
    plane.values = {0.0F, 0.5F, 1.0F, std::numeric_limits<float>::quiet_NaN()};
    plane.valid = {1, 1, 0, 1};
    plane.sourceLevel = {0, 0, -1, 0};

    amrvis::ScalarRenderSettings settings;
    settings.invalidColor = 0xFF010203U;
    settings.nanColor = 0xFF040506U;
    const auto image = amrvis::renderScalarPlane(plane, settings);
    require(image.valid(), "renderer produced an invalid image buffer");
    require(image.rgba[0] != image.rgba[1], "range endpoints mapped to one color");
    require(image.rgba[2] == settings.invalidColor, "invalid pixel color mismatch");
    require(image.rgba[3] == settings.nanColor, "NaN pixel color mismatch");

    plane.values = {1.0F, 10.0F, 100.0F, -1.0F};
    settings.minimum = 1.0;
    settings.maximum = 100.0;
    settings.scale = {amrvis::ColorScale::Logarithmic};
    const auto logarithmic = amrvis::renderScalarPlane(plane, settings);
    require(logarithmic.rgba[0] != logarithmic.rgba[1]
            && logarithmic.rgba[1] != logarithmic.rgba[2],
        "logarithmic range did not distinguish decades");
    require(logarithmic.rgba[3] == settings.nanColor,
        "non-positive logarithmic value color mismatch");

    plane.values = {
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN(),
        1.0F
    };
    plane.valid = {1, 1, 1, 0};
    settings.minimum = 0.0;
    settings.maximum = 1.0;
    settings.scale = {amrvis::ColorScale::Linear};
    const auto nonFinite = amrvis::renderScalarPlane(plane, settings);
    require(nonFinite.rgba[0] == settings.nanColor,
        "positive infinity pixel color mismatch");
    require(nonFinite.rgba[1] == settings.nanColor,
        "negative infinity pixel color mismatch");
    require(nonFinite.rgba[2] == settings.nanColor,
        "NaN pixel color mismatch");
    require(nonFinite.rgba[3] == settings.invalidColor,
        "invalid mask did not take precedence over value");

    amrvis::ScalarPlane tooWide;
    tooWide.width = std::numeric_limits<int>::max();
    tooWide.height = 1;
    bool threw = false;
    try {
        (void)amrvis::renderScalarPlane(tooWide, settings);
    } catch (const std::overflow_error&) {
        threw = true;
    }
    require(threw, "renderer accepted an unrepresentable row stride");

    // An extreme but finite range (±DBL_MAX) overflows the span to infinity;
    // the renderer must reject it instead of normalizing every value to zero.
    settings.minimum = -std::numeric_limits<double>::max();
    settings.maximum = std::numeric_limits<double>::max();
    threw = false;
    try {
        (void)amrvis::renderScalarPlane(plane, settings);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "renderer accepted a range whose span overflows to infinity");

    // Non-finite endpoints are also rejected: ±inf where the ordering still
    // holds fails the span check, and NaN fails the ordering check outright.
    settings.minimum = -std::numeric_limits<double>::infinity();
    settings.maximum = std::numeric_limits<double>::infinity();
    threw = false;
    try {
        (void)amrvis::renderScalarPlane(plane, settings);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "renderer accepted an infinite range");

    settings.minimum = std::numeric_limits<double>::quiet_NaN();
    settings.maximum = 1.0;
    threw = false;
    try {
        (void)amrvis::renderScalarPlane(plane, settings);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "renderer accepted a NaN range endpoint");

    // A well-formed baseline the remaining throw-path cases perturb one field
    // at a time; it must render so the negatives below prove the perturbation.
    amrvis::ScalarPlane good;
    good.width = 2;
    good.height = 2;
    good.values = {0.0F, 0.5F, 1.0F, 0.25F};
    good.valid = {1, 1, 1, 1};
    good.sourceLevel = {0, 0, 0, 0};
    amrvis::ScalarRenderSettings base;
    base.minimum = 0.0;
    base.maximum = 1.0;
    require(amrvis::renderScalarPlane(good, base).valid(),
        "the throw-path baseline did not render");

    // Non-positive dimensions (the first check).
    {
        auto badPlane = good;
        badPlane.width = 0;
        expectRejected(badPlane, base, "dimensions must be positive",
            "a zero-width plane was not rejected");
        badPlane = good;
        badPlane.height = -1;
        expectRejected(badPlane, base, "dimensions must be positive",
            "a negative-height plane was not rejected");
    }

    // Storage that does not match width*height, on either backing vector.
    {
        auto badPlane = good;
        badPlane.values.pop_back();   // three values for a 2x2 plane
        expectRejected(badPlane, base, "storage does not match",
            "a values vector shorter than width*height was not rejected");
        badPlane = good;
        badPlane.valid.pop_back();
        expectRejected(badPlane, base, "storage does not match",
            "a valid mask shorter than width*height was not rejected");
    }

    // A degenerate (min == max) range has no positive extent.
    {
        auto badSettings = base;
        badSettings.minimum = 1.0;
        badSettings.maximum = 1.0;
        expectRejected(good, badSettings, "positive extent",
            "a zero-width value range was not rejected");
    }

    // Logarithmic rendering requires a strictly positive minimum.
    {
        auto badSettings = base;
        badSettings.scale = {amrvis::ColorScale::Logarithmic};
        badSettings.minimum = 0.0;
        badSettings.maximum = 10.0;
        expectRejected(good, badSettings, "logarithmic scalar range must be positive",
            "a zero logarithmic minimum was not rejected");
        badSettings.minimum = -1.0;
        expectRejected(good, badSettings, "logarithmic scalar range must be positive",
            "a negative logarithmic minimum was not rejected");
        // A logarithmic range can also be too narrow to map even though every
        // check above passes: adjacent doubles at 1e300 are ordered, finite
        // and positive, and their span is finite, but their logarithms are
        // the same double, so there is no span to spread the slots over.
        // Before the shared mapping this rendered a step function; it is a
        // refusal now, and validateVolumeRenderRequest refuses it too, so the
        // two never disagree about what a legal range is.
        badSettings.minimum = 1.0e300;
        badSettings.maximum = std::nextafter(
            1.0e300, std::numeric_limits<double>::infinity());
        expectRejected(good, badSettings, "same logarithm",
            "a logarithmic range narrower than one logarithm was not rejected");
    }

    // Check order: an earlier fault must win. Dimensions are validated before
    // the range, and storage before the range/log checks, so a plane that
    // violates several conditions reports the earliest one.
    {
        auto badPlane = good;
        badPlane.width = 0;                        // dimension fault ...
        auto badSettings = base;
        badSettings.minimum = 5.0;
        badSettings.maximum = 1.0;                 // ... and a bad range
        expectRejected(badPlane, badSettings, "dimensions must be positive",
            "the dimension check did not precede the range check");

        badPlane = good;
        badPlane.values.pop_back();                // storage fault ...
        badSettings.scale = {amrvis::ColorScale::Logarithmic};
        badSettings.minimum = -1.0;                // ... and a bad log range
        expectRejected(badPlane, badSettings, "storage does not match",
            "the storage check did not precede the range/log checks");
    }

    // The renderer resolves the palette into a lookup table once instead of
    // calling Palette::argb per pixel. That is only allowed to be faster, never
    // different: this walks a plane across the whole range, including both
    // clipped ends and the slot boundaries in between, and requires every pixel
    // to equal what argb would have produced.
    {
        const auto& palette = amrvis::builtinPalette(
            amrvis::BuiltinPalette::Rainbow);
        constexpr int samples = 4096;
        amrvis::ScalarPlane sweep;
        sweep.width = samples;
        sweep.height = 1;
        sweep.values.resize(static_cast<std::size_t>(samples));
        sweep.valid.assign(static_cast<std::size_t>(samples), 1);
        // Deliberately runs outside [0, 1] at both ends so the two clipping
        // branches are covered, not just the interior.
        for (int i = 0; i < samples; ++i) {
            sweep.values[static_cast<std::size_t>(i)] = static_cast<float>(
                -0.25 + 1.5 * static_cast<double>(i)
                    / static_cast<double>(samples - 1));
        }
        amrvis::ScalarRenderSettings sweepSettings;
        sweepSettings.minimum = 0.0;
        sweepSettings.maximum = 1.0;
        sweepSettings.palette = &palette;
        const auto sweepImage = amrvis::renderScalarPlane(sweep, sweepSettings);
        for (int i = 0; i < samples; ++i) {
            const auto value
                = static_cast<double>(sweep.values[static_cast<std::size_t>(i)]);
            const auto normalized = (value - sweepSettings.minimum)
                / (sweepSettings.maximum - sweepSettings.minimum);
            require(sweepImage.rgba[static_cast<std::size_t>(i)]
                    == palette.argb(normalized),
                "the palette lookup table disagrees with Palette::argb");
        }
    }

    // The sweep above uses [0, 1], whose reciprocal is exact, so it cannot see
    // what hoisting the division out of the loop costs. [0, 49] can, at both
    // ends of the failure: 49 * (1/49) is 0.99999999999999988898 rather than
    // 1.0, dropping the maximum into the second-to-last slot, and 24.5 * (1/49)
    // is 0.49999999999999994 rather than exactly 0.5, dropping the midpoint
    // from slot 126 to 125. Random sampling does not find the second one --
    // random doubles do not land on exact ties -- so it is pinned by value
    // here rather than left to the sweep.
    {
        const auto& palette = amrvis::builtinPalette(
            amrvis::BuiltinPalette::Rainbow);
        amrvis::ScalarPlane peak;
        peak.width = 3;
        peak.height = 1;
        peak.values = {0.0F, 24.5F, 49.0F};
        peak.valid = {1, 1, 1};
        amrvis::ScalarRenderSettings peakSettings;
        peakSettings.minimum = 0.0;
        peakSettings.maximum = 49.0;
        peakSettings.palette = &palette;
        const auto peakImage = amrvis::renderScalarPlane(peak, peakSettings);
        require(peakImage.rgba[0] == palette.slotArgb(amrvis::Palette::paletteStart),
            "a pixel at the range minimum did not get the first data slot");
        require(peakImage.rgba[2] == palette.slotArgb(amrvis::Palette::paletteEnd),
            "a pixel at the range maximum did not get the last data slot");
        require(peakImage.rgba[2] == palette.argb(1.0),
            "a pixel at the range maximum disagrees with the color bar");
        // The midpoint of the range is the midpoint of the palette.
        require(peakImage.rgba[1] == palette.argb(0.5),
            "a pixel at the range midpoint disagrees with the color bar");
        require(peakImage.rgba[1]
                == palette.slotArgb(amrvis::Palette::paletteStart + 126),
            "the range midpoint did not get the midpoint slot");
    }

    {
        const amrvis::ColorScaleConfig scale{amrvis::ColorScale::SymLogarithmic, 1.0};
        const auto range = amrvis::resolveValueRange(-100.0, 100.0, scale);
        require(range.has_value(), "a valid symmetric-log range was rejected");
        require(amrvis::valueSlot(0.0, *range, 253) == 126,
            "zero did not map to the palette midpoint");
        for (const double value : {-100.0, -1.0, 0.0, 1.0, 100.0}) {
            const auto roundTrip = amrvis::inverseTransformedValue(
                amrvis::transformedValue(value, scale), scale);
            require(std::abs(roundTrip - value) <= 1.0e-12 * std::max(1.0, std::abs(value)),
                "the symmetric-log transform did not invert");
        }
    }
    {
        const amrvis::ColorScaleConfig scale{amrvis::ColorScale::SymLogarithmic, 1.0e-300};
        const auto range = amrvis::resolveValueRange(-1.0e20, 1.0e20, scale);
        require(range.has_value(), "an overflowing intermediate ratio rejected a valid symlog range");
        require(amrvis::valueSlot(0.0, *range, 253) == 126,
            "an extreme symlog range lost its midpoint");
        for (const double value : {-1.0e20, -1.0e10, 1.0e10, 1.0e20}) {
            const auto mapped = amrvis::transformedValue(value, scale);
            const auto expected = std::copysign(1.0e-300
                * (amrvis::symmetricLogLinearScale + std::log10(std::abs(value)) + 300.0), value);
            require(std::abs(mapped / expected - 1.0) < 1.0e-14,
                "an extreme symlog value transformed incorrectly");
            const auto roundTrip = amrvis::inverseTransformedValue(expected, scale);
            require(std::isfinite(roundTrip) && std::abs(roundTrip / value - 1.0) < 1.0e-12,
                "symlog inversion overflowed an intermediate power");
        }
    }
    return 0;
}
