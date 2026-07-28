#include <amrexplorer/render2d/ScalarRenderer.hpp>

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
    settings.logarithmic = true;
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
    settings.logarithmic = false;
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
        badSettings.logarithmic = true;
        badSettings.minimum = 0.0;
        badSettings.maximum = 10.0;
        expectRejected(good, badSettings, "logarithmic scalar range must be positive",
            "a zero logarithmic minimum was not rejected");
        badSettings.minimum = -1.0;
        expectRejected(good, badSettings, "logarithmic scalar range must be positive",
            "a negative logarithmic minimum was not rejected");
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
        badSettings.logarithmic = true;
        badSettings.minimum = -1.0;                // ... and a bad log range
        expectRejected(badPlane, badSettings, "storage does not match",
            "the storage check did not precede the range/log checks");
    }

    return 0;
}
