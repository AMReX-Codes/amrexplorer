#include <amrexplorer/render2d/ScalarRenderer.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
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
    return 0;
}
