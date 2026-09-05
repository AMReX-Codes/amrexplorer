#pragma once

// The colors the Dataset window draws its numbers in: the palette slot the
// scalar renderer gives the same value under the same display range, so a
// number in the table, the pixel it stands for in the image, and the color bar
// beside them all carry one color.

#include "Theme.hpp"

#include <amrexplorer/core/ValueMapping.hpp>
#include <amrexplorer/render2d/Palette.hpp>
#include <amrexplorer/render2d/ScalarRenderer.hpp>

#include <QColor>

#include <optional>

namespace amrvis::qt {

// The palette is held by value rather than by pointer as the color bar holds
// it: the Dataset window is a top-level WA_DeleteOnClose widget whose deferred
// delete can outlive the MainWindow that owns the PaletteController, and a
// palette is 1 KB of trivially copyable bytes.
struct DatasetColoring {
    Palette palette;
    // nullopt for a range nothing can be mapped through (see
    // resolveValueRange); the window then falls back to a plain foreground
    // rather than drawing every number in the first slot's color.
    std::optional<ResolvedValueRange> range;
};

[[nodiscard]] inline DatasetColoring makeDatasetColoring(
    const Palette& palette, double minimum, double maximum,
    ColorScaleConfig scale)
{
    return DatasetColoring{
        palette, resolveValueRange(minimum, maximum, scale)};
}

// What a value is drawn in. Mapped through ValueMapping, exactly as
// renderScalarPlane maps the same value, so the two cannot drift apart at the
// truncation ties valueSlot documents. Values outside the range take the end
// colors, as they do in the image; a value the range cannot map at all -- a
// NaN, or a non-positive one under a logarithmic range -- takes the renderer's
// own nan color.
[[nodiscard]] inline QColor datasetValueColor(
    const DatasetColoring& coloring, double value)
{
    if (!coloring.range) {
        return viewportForeground();
    }
    if (!mappableValue(value, *coloring.range)) {
        return QColor::fromRgb(
            static_cast<QRgb>(ScalarRenderSettings{}.nanColor));
    }
    const auto slot = valueSlot(value, *coloring.range, Palette::colorSlots);
    return QColor::fromRgb(static_cast<QRgb>(
        coloring.palette.slotArgb(Palette::paletteStart + slot)));
}

// Behind a sample no grid covers at that level, matching the color the
// renderer fills a pixel with when no level provides one.
[[nodiscard]] inline QColor datasetUncoveredBackground()
{
    return QColor::fromRgb(
        static_cast<QRgb>(ScalarRenderSettings{}.invalidColor));
}

} // namespace amrvis::qt
