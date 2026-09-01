// Coverage for DatasetColoring, the value-to-color mapping the Dataset window
// draws its numbers with. The load-bearing case is the cross-check against
// renderScalarPlane: the whole point of the header is that a number in the
// table and the pixel it stands for in the image carry one color, so the
// expectation comes from the renderer itself rather than from a second copy of
// the formula here.

#include "DatasetColoring.hpp"
#include "Theme.hpp"

#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/render2d/Palette.hpp>
#include <amrexplorer/render2d/ScalarRenderer.hpp>

#include <QColor>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::uint32_t argbOf(const QColor& color)
{
    return static_cast<std::uint32_t>(color.rgb());
}

// Every value drawn the way the renderer draws that same value: same palette,
// same range, same log flag. Values are floats because that is what both the
// image plane and the dataset extract hold.
void requireAgreesWithRenderer(const std::string& what,
    const std::vector<float>& values, const amrvis::Palette& palette,
    double minimum, double maximum, bool logarithmic)
{
    amrvis::ScalarPlane plane;
    plane.width = static_cast<int>(values.size());
    plane.height = 1;
    plane.values = values;
    plane.valid.assign(values.size(), 1);
    plane.sourceLevel.assign(values.size(), 0);
    const auto image = amrvis::renderScalarPlane(plane,
        amrvis::ScalarRenderSettings{
            .minimum = minimum,
            .maximum = maximum,
            .logarithmic = logarithmic,
            .palette = &palette
        });
    require(image.valid(), what + ": the renderer produced no image");

    const auto coloring = amrvis::qt::makeDatasetColoring(
        palette, minimum, maximum, logarithmic);
    require(coloring.range.has_value(), what + ": the range did not resolve");
    std::size_t distinct = 0;
    for (std::size_t pixel = 0; pixel < values.size(); ++pixel) {
        const auto drawn = argbOf(amrvis::qt::datasetValueColor(
            coloring, static_cast<double>(values[pixel])));
        require(drawn == image.rgba[pixel],
            what + ": value " + std::to_string(values[pixel])
                + " is drawn in " + std::to_string(drawn)
                + " but rendered as " + std::to_string(image.rgba[pixel]));
        distinct += pixel > 0 && image.rgba[pixel] != image.rgba[pixel - 1];
    }
    // Otherwise the agreement above is the agreement of one flat color.
    require(distinct >= 3, what + ": the values barely differ in color");
}

} // namespace

int main()
{
    const auto& rainbow = amrvis::builtinPalette(amrvis::BuiltinPalette::Rainbow);
    const auto reversedViridis
        = amrvis::builtinPalette(amrvis::BuiltinPalette::Viridis).reversed();

    // A span whose top and whose exact interior ties are the cases
    // ValueMapping documents as sensitive to how the normalization is spelled,
    // plus values under and over the range.
    requireAgreesWithRenderer("linear rainbow",
        {-3.0F, 0.0F, 1.0F, 12.25F, 24.5F, 36.75F, 48.0F, 49.0F, 60.0F},
        rainbow, 0.0, 49.0, false);
    requireAgreesWithRenderer("linear reversed viridis",
        {-3.0F, 0.0F, 1.0F, 12.25F, 24.5F, 36.75F, 48.0F, 49.0F, 60.0F},
        reversedViridis, 0.0, 49.0, false);
    requireAgreesWithRenderer("logarithmic rainbow",
        {1e-4F, 1e-3F, 1e-2F, 0.1F, 1.0F, 10.0F, 100.0F, 1e3F},
        rainbow, 1e-3, 100.0, true);
    requireAgreesWithRenderer("negative span",
        {-20.0F, -10.0F, -7.5F, -5.0F, -2.5F, 0.0F, 5.0F},
        rainbow, -10.0, 0.0, false);

    // The ends, named outright: the color bar's bottom and top are the
    // palette's first and last data slots, and the reversal swaps them.
    const auto linear = amrvis::qt::makeDatasetColoring(rainbow, 2.0, 8.0, false);
    require(argbOf(amrvis::qt::datasetValueColor(linear, 2.0))
            == rainbow.slotArgb(amrvis::Palette::paletteStart),
        "the minimum is not the palette's first data slot");
    require(argbOf(amrvis::qt::datasetValueColor(linear, -100.0))
            == rainbow.slotArgb(amrvis::Palette::paletteStart),
        "a value under the range does not clamp to the first data slot");
    require(argbOf(amrvis::qt::datasetValueColor(linear, 8.0))
            == rainbow.slotArgb(amrvis::Palette::paletteEnd),
        "the maximum is not the palette's last data slot");
    require(argbOf(amrvis::qt::datasetValueColor(linear, 100.0))
            == rainbow.slotArgb(amrvis::Palette::paletteEnd),
        "a value over the range does not clamp to the last data slot");
    const auto reversed = amrvis::qt::makeDatasetColoring(
        rainbow.reversed(), 2.0, 8.0, false);
    require(argbOf(amrvis::qt::datasetValueColor(reversed, 2.0))
            == rainbow.slotArgb(amrvis::Palette::paletteEnd),
        "reversal did not swap the low end");
    require(argbOf(amrvis::qt::datasetValueColor(reversed, 8.0))
            == rainbow.slotArgb(amrvis::Palette::paletteStart),
        "reversal did not swap the high end");

    // Values no range can map take the renderer's nan color, so an unplottable
    // number is as conspicuous in the table as its pixel is in the image.
    const auto nanColor = amrvis::ScalarRenderSettings{}.nanColor;
    require(argbOf(amrvis::qt::datasetValueColor(
                linear, std::numeric_limits<double>::quiet_NaN())) == nanColor,
        "NaN is not drawn in the renderer's nan color");
    require(argbOf(amrvis::qt::datasetValueColor(
                linear, std::numeric_limits<double>::infinity())) == nanColor,
        "an infinity is not drawn in the renderer's nan color");
    const auto logarithmic
        = amrvis::qt::makeDatasetColoring(rainbow, 1e-3, 100.0, true);
    require(argbOf(amrvis::qt::datasetValueColor(logarithmic, 0.0)) == nanColor,
        "zero under a log range is not drawn in the renderer's nan color");
    require(argbOf(amrvis::qt::datasetValueColor(logarithmic, -1.0)) == nanColor,
        "a negative under a log range is not drawn in the nan color");

    // A range nothing can be mapped through: no color is meaningful, so the
    // numbers stay legible in the plain viewport foreground instead of all
    // taking the first slot's color.
    for (const auto& unusable : {
             amrvis::qt::makeDatasetColoring(rainbow, 5.0, 5.0, false),
             amrvis::qt::makeDatasetColoring(rainbow, 8.0, 2.0, false),
             amrvis::qt::makeDatasetColoring(rainbow, -1.0, 10.0, true),
             amrvis::qt::makeDatasetColoring(rainbow,
                 std::numeric_limits<double>::quiet_NaN(), 1.0, false)}) {
        require(!unusable.range.has_value(),
            "an unusable range resolved anyway");
        require(amrvis::qt::datasetValueColor(unusable, 5.0)
                == amrvis::qt::viewportForeground(),
            "an unusable range does not fall back to the viewport foreground");
    }

    // Samples no grid covers at a level are shaded in the color the renderer
    // fills a pixel with when no level provides one.
    require(argbOf(amrvis::qt::datasetUncoveredBackground())
            == amrvis::ScalarRenderSettings{}.invalidColor,
        "the uncovered shade is not the renderer's invalid color");
    require(amrvis::qt::datasetUncoveredBackground()
            != amrvis::qt::viewportBackground(),
        "uncovered cells are indistinguishable from covered ones");

    std::cout << "dataset colors OK\n";
    return 0;
}
