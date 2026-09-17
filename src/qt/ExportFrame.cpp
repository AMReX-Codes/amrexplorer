#include "ExportFrame.hpp"
#include "ColorBarWidget.hpp"
#include "NumberFormat.hpp"
#include "Theme.hpp"

#include <QPainter>
#include <algorithm>
#include <cmath>

namespace amrvis::qt {

std::array<ExportAxis, 2> exportAxes(const RealBox& region, int dimension, int normal,
                                     int coordinateSystem, SphericalDisplay spherical,
                                     bool hasPhysicalGeometry, const QString& lengthUnit) {
    std::array<int, 2> indices{0, 1};
    if (dimension == 3) {
        int next = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis != normal) {
                indices[static_cast<std::size_t>(next++)] = axis;
            }
        }
    }
    const std::array<QString, 3> cartesian{QStringLiteral("x"), QStringLiteral("y"),
                                           QStringLiteral("z")};
    std::array<ExportAxis, 2> result;
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const auto index = static_cast<std::size_t>(indices[axis]);
        result[axis] = {cartesian[index], region.lower[index], region.upper[index]};
    }
    int angular = -1;
    if (dimension == 2 && hasPhysicalGeometry) {
        if (coordinateSystem == static_cast<int>(CoordinateSystem::Cylindrical) ||
            coordinateSystem == static_cast<int>(CoordinateSystem::Spherical)) {
            result[0].label = QStringLiteral("R");
            result[1].label = QStringLiteral("Z");
        }
        if (coordinateSystem == static_cast<int>(CoordinateSystem::Spherical) &&
            spherical != SphericalDisplay::RZ) {
            angular = spherical == SphericalDisplay::ThetaR ? 0 : 1;
            result[static_cast<std::size_t>(angular)].label = QString(QChar(0x03b8));
            result[static_cast<std::size_t>(1 - angular)].label = QStringLiteral("r");
            // displayRegion already has the displayed (possibly swapped) bounds.
        }
    }
    if (hasPhysicalGeometry && !lengthUnit.isEmpty()) {
        for (std::size_t axis = 0; axis < 2; ++axis) {
            result[axis].label += QStringLiteral(" (%1)").arg(
                static_cast<int>(axis) == angular ? QStringLiteral("rad") : lengthUnit);
        }
    }
    return result;
}

QString exportNumber(double value, const QString& format, const QFontMetrics& metrics,
                     int availableWidth) {
    // Never clip or elide numeric values. Compact notation is the fallback
    // when a fixed animation layout cannot fit the requested notation.
    const auto normalized = value == 0.0 ? 0.0 : value;
    const auto digits = std::max(minimumDisplayDigits, formatDigits(format));
    const auto label = fitNumber(normalized, format, digits,
        1,
        [&metrics](const QString& text) {
            return metrics.horizontalAdvance(text);
        },
        availableWidth);
    return metrics.horizontalAdvance(label) <= availableWidth ? label : QString();
}

// Width to allow a label of `digits` significant digits: the mantissa plus
// sign, point, 'e', exponent sign and three exponent digits, with a glyph of
// slack. Both the layout and the drawing pass must reach this same number, or
// a label measured under one budget is drawn under another.
int labelBudget(int glyphWidth, int digits)
{
    return (std::clamp(digits, minimumDisplayDigits, maximumDisplayDigits) + 9) * glyphWidth;
}

namespace {

int xLabelSpacing(const QFontMetrics& metrics, const ExportAxis& axis,
    const QString& format, int labelWidth)
{
    return std::max(
        metrics.horizontalAdvance(exportNumber(axis.minimum, format, metrics, labelWidth)),
        metrics.horizontalAdvance(exportNumber(axis.maximum, format, metrics, labelWidth)))
        + 2 * metrics.horizontalAdvance(QChar('0')) + 12;
}

} // namespace

std::vector<ExportTick> exportTicks(const ExportAxis& axis, int pixelLength, int labelSpacing,
                                    const QString& format, const QFontMetrics& metrics,
                                    int labelWidth) {
    std::vector<ExportTick> ticks;
    if (!std::isfinite(axis.minimum) || !std::isfinite(axis.maximum) ||
        !(axis.minimum < axis.maximum)) {
        return ticks;
    }
    const int intervals = std::clamp(pixelLength / std::max(1, labelSpacing), 1, 4);
    const double span = axis.maximum - axis.minimum;
    const double rawStep = span / intervals;
    const double magnitude = std::pow(10.0, std::floor(std::log10(rawStep)));
    const double normalized = rawStep / magnitude;
    const double step = (normalized <= 1.0   ? 1.0
                         : normalized <= 2.0 ? 2.0
                         : normalized <= 5.0 ? 5.0
                                             : 10.0) *
                        magnitude;
    const auto append = [&](double value, double fraction) {
        const auto label = exportNumber(value, format, metrics, labelWidth);
        if (!label.isEmpty() && (ticks.empty() || ticks.back().label != label)) {
            ticks.push_back({fraction, label});
        }
    };
    if (std::isfinite(step) && step > 0.0 && std::isfinite(span)) {
        const double first = std::ceil(axis.minimum / step) * step;
        for (int i = 0; i < 16; ++i) {
            const double value = first + i * step;
            if (!std::isfinite(value) || value > axis.maximum) {
                break;
            }
            if (value >= axis.minimum) {
                append(value, (value - axis.minimum) / span);
            }
        }
    }
    if (ticks.empty()) {
        // Overflowing or subnormal spans still have representable endpoints.
        for (int i = 0; i <= intervals; ++i) {
            const double fraction = static_cast<double>(i) / intervals;
            append(std::lerp(axis.minimum, axis.maximum, fraction), fraction);
        }
    }
    return ticks;
}

ExportLayout makeExportLayout(QSize rasterSize, const ExportOptions& options,
                              const std::array<ExportAxis, 2>& axes, const ColorBarWidget* colorBar,
                              bool reserveLabelGrowth) {
    ExportLayout layout;
    if (rasterSize.isEmpty()) {
        return layout;
    }
    layout.font = options.font;
    for (std::size_t axis = 0; axis < axes.size(); ++axis) {
        layout.axisFormats[axis] = resolveNumberFormat(
            options.numberFormat, axes[axis].minimum, axes[axis].maximum);
    }
    // 11-point text at a seven-inch reference width, including annotations.
    // Use the raster's longer side so a narrow portrait slice gets the same
    // readable text as a square slice of comparable height.
    int fontPixels = 12;
    for (int iteration = 0; iteration < 24; ++iteration) {
        layout.font.setPixelSize(fontPixels);
        const QFontMetrics fm(layout.font);
        int glyphWidth = 0;
        for (const QChar character : QStringLiteral("0123456789.e+-")) {
            glyphWidth = std::max(glyphWidth, fm.horizontalAdvance(character));
        }
        std::array<int, 2> maximumLabelWidths{};
        std::array<int, 2> growthWidths{};
        for (std::size_t axis = 0; axis < axes.size(); ++axis) {
            const int digits = formatDigits(layout.axisFormats[axis]);
            maximumLabelWidths[axis] = labelBudget(glyphWidth, digits);
            growthWidths[axis] = reserveLabelGrowth
                ? std::max(fm.horizontalAdvance(QStringLiteral("-9e-308")),
                      fm.horizontalAdvance(QStringLiteral("-9e+308")))
                    + std::max(0, digits - minimumDisplayDigits) * glyphWidth
                : 0;
        }
        layout.labelWidth = growthWidths[0];
        for (double endpoint : {axes[0].minimum, axes[0].maximum}) {
            layout.labelWidth = std::max(layout.labelWidth,
                fm.horizontalAdvance(exportNumber(endpoint, layout.axisFormats[0],
                    fm, maximumLabelWidths[0])));
        }
        layout.verticalLabelWidth = growthWidths[1];
        int xOverhang = 0;
        for (std::size_t axis = 0; axis < axes.size(); ++axis) {
            const int length = axis == 0 ? rasterSize.width() : rasterSize.height();
            const int spacing =
                axis == 0
                    ? xLabelSpacing(fm, axes[0], layout.axisFormats[0], layout.labelWidth)
                    : fm.height() + 8;
            for (const auto& tick : exportTicks(axes[axis], length, spacing, layout.axisFormats[axis],
                                                fm, maximumLabelWidths[axis])) {
                const int width = fm.horizontalAdvance(tick.label);
                if (axis == 0) {
                    layout.labelWidth = std::max(layout.labelWidth, width);
                    xOverhang = std::max(xOverhang, width / 2 + 1);
                } else {
                    layout.verticalLabelWidth = std::max(layout.verticalLabelWidth, width);
                }
            }
        }
        // Non-finite/degenerate ranges may have no ticks.
        layout.labelWidth = std::max(1, layout.labelWidth);
        layout.verticalLabelWidth = std::max(1, layout.verticalLabelWidth);
        if (reserveLabelGrowth) {
            xOverhang = std::max(xOverhang, layout.labelWidth / 2 + 1);
        }
        const int tickLength = std::max(4, fontPixels / 4);
        const int gap = std::max(4, fm.height() / 4);
        const int verticalTitleGap = std::max(2, gap / 2);
        const int left =
            options.includeAxes
                ? std::max(layout.verticalLabelWidth + fm.height() + tickLength + 2 * gap +
                               verticalTitleGap,
                           xOverhang + gap)
                : 0;
        const int top = options.includeAxes ? fm.height() / 2 + gap : 0;
        const int bottom = options.includeAxes ? 2 * fm.height() + tickLength + 3 * gap : 0;
        const int right = options.includeAxes ? xOverhang + gap : 0;
        layout.dataRect = QRect(QPoint(left, top), rasterSize);
        int width = left + rasterSize.width() + right;
        if (options.includeColorBar) {
            // Color values have their own precision budget, independent of
            // the spatial axes. Movies allow the full compact notation at
            // that precision even if the first frame has short tick labels.
            const auto presentation = colorBar != nullptr
                ? colorBar->exportPresentation(fm, QRect(0, 0, 0, rasterSize.height()))
                : ColorBarWidget::NumberPresentation{
                    options.numberFormat, options.numberFormat, false};
            const int maximumLabelWidth = labelBudget(glyphWidth,
                formatDigits(presentation.tickFormat));
            const int labels = colorBar != nullptr
                ? colorBar->exportLabelWidth(fm, maximumLabelWidth, rasterSize.height()) : 0;
            int barWidth = ColorBarWidget::exportWidth(fm,
                std::max(labels, reserveLabelGrowth ? maximumLabelWidth : 0));
            if (reserveLabelGrowth && presentation.offsetLine) {
                // A later narrow range may have no usable offset, so leave
                // room for full-value ticks beside the bar as well.
                barWidth = std::max(barWidth,
                    ColorBarWidget::exportWidth(fm,
                        labelBudget(glyphWidth, formatDigits(presentation.valueFormat))));
            }
            // The color scale is beside the data, above the x tick labels.
            // Their endpoint overhang must not become an inter-panel gutter.
            layout.colorBarRect =
                QRect(left + rasterSize.width() + gap, top, barWidth, rasterSize.height());
            width = std::max(width, layout.colorBarRect.right() + 1);
        }
        layout.canvasSize = QSize(width, top + rasterSize.height() + bottom);
        // Long labels need wider margins, but must not keep enlarging the
        // font that those margins were measured with. Limit their contribution
        // to the reference width to the data's longer side. Ordinary layouts
        // retain their sizing, and high precision can grow the canvas without
        // making the text overwhelm the data.
        const int dataLength = std::max(rasterSize.width(), rasterSize.height());
        const int referenceWidth =
            dataLength + std::min(width - rasterSize.width(), dataLength);
        const int nextFontPixels =
            std::max(12, static_cast<int>(std::lround(referenceWidth * 11.0 / (72.0 * 7.0))));
        if (nextFontPixels == fontPixels) {
            break;
        }
        fontPixels = nextFontPixels;
    }
    if (options.includeColorBar && colorBar != nullptr) {
        layout.colorBarPresentation = colorBar->exportPresentation(
            QFontMetrics(layout.font), layout.colorBarRect);
    }
    layout.dotsPerMeter =
        static_cast<int>(std::lround(layout.font.pixelSize() * 72.0 / (11.0 * 0.0254)));
    return layout;
}

bool exportAspectMatches(QSizeF rasterSize, const ExportLayout& layout) {
    if (rasterSize.isEmpty() || layout.dataRect.isEmpty()) {
        return false;
    }
    // A one-pixel axis is the export size floor, not a rounded value: the
    // scale is then set by the other axis, and the raster matches when that
    // scale would round this axis to one pixel or less.
    const double width = layout.dataRect.width();
    const double height = layout.dataRect.height();
    if (width == 1.0 && height > 1.0) {
        return rasterSize.width() * (height / rasterSize.height()) <= 1.5;
    }
    if (height == 1.0 && width > 1.0) {
        return rasterSize.height() * (width / rasterSize.width()) <= 1.5;
    }
    // Both output dimensions are rounded independently after applying one
    // scale. Allow half an output pixel on each axis: eliminating the scale
    // gives |h * W - w * H| <= (w + h) / 2. This also handles portrait rasters
    // without amplifying width rounding into a false aspect-ratio change.
    return
           std::abs(rasterSize.height() * layout.dataRect.width() -
                    rasterSize.width() * layout.dataRect.height()) <=
               0.5 * (rasterSize.width() + rasterSize.height());
}

QImage composeExportImage(const QImage& raster, const std::array<ExportAxis, 2>& axes,
                          const ExportOptions& options, const ExportLayout& layout,
                          const ColorBarWidget* colorBar) {
    if (raster.isNull() || raster.size() != layout.dataRect.size()) {
        return {};
    }
    if (!options.includeAxes && !options.includeColorBar && options.transparentBackground) {
        return raster;
    }
    QImage result(layout.canvasSize, QImage::Format_ARGB32_Premultiplied);
    if (result.isNull()) {
        return {};
    }
    result.fill(options.transparentBackground ? Qt::transparent : Qt::white);
    result.setDotsPerMeterX(layout.dotsPerMeter);
    result.setDotsPerMeterY(layout.dotsPerMeter);
    QPainter painter(&result);
    painter.setFont(layout.font);
    // Preserve raster alpha only for transparent exports. White exports also
    // flatten holes inside the data rectangle (for example, outside an R-Z wedge).
    painter.setCompositionMode(options.transparentBackground ? QPainter::CompositionMode_Source
                                                            : QPainter::CompositionMode_SourceOver);
    painter.drawImage(layout.dataRect.topLeft(), raster);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (options.includeColorBar && colorBar != nullptr) {
        colorBar->paintBar(&painter, layout.colorBarRect, true, true,
            layout.colorBarPresentation ? &*layout.colorBarPresentation : nullptr);
    }
    if (!options.includeAxes) {
        return result;
    }
    QPen axisPen(Qt::black);
    axisPen.setWidthF(std::max(1.0, layout.font.pixelSize() / 16.0));
    painter.setPen(axisPen);
    painter.setClipRegion(QRegion(result.rect()).subtracted(QRegion(layout.dataRect)));
    const QFontMetrics fm(layout.font);
    const auto& rect = layout.dataRect;
    // Axis rules sit immediately outside the raster; no data pixels are covered.
    const int x0 = rect.left() - 1;
    const int y0 = rect.bottom() + 1;
    const int tickLength = std::max(4, layout.font.pixelSize() / 4);
    const int gap = std::max(4, fm.height() / 4);
    const int verticalTitleGap = std::max(2, gap / 2);
    painter.drawLine(x0, rect.top(), x0, y0);
    painter.drawLine(x0, y0, rect.right(), y0);
    const int spacing = xLabelSpacing(fm, axes[0], layout.axisFormats[0], layout.labelWidth);
    for (const auto& tick : exportTicks(axes[0], rect.width(), spacing, layout.axisFormats[0],
                                        fm, layout.labelWidth)) {
        const double x = rect.left() + tick.fraction * (rect.width() - 1);
        painter.drawLine(QPointF(x, y0), QPointF(x, y0 + tickLength));
        painter.drawText(QRectF(x - layout.labelWidth / 2.0, y0 + tickLength + gap,
                                layout.labelWidth, fm.height()),
                         Qt::AlignHCenter | Qt::AlignTop, tick.label);
    }
    for (const auto& tick : exportTicks(axes[1], rect.height(), fm.height() + 8,
                                        layout.axisFormats[1], fm, layout.verticalLabelWidth)) {
        const double y = rect.bottom() - tick.fraction * (rect.height() - 1);
        painter.drawLine(QPointF(x0 - tickLength, y), QPointF(x0, y));
        painter.drawText(QRectF(x0 - tickLength - gap - layout.verticalLabelWidth,
                                y - fm.height() / 2.0, layout.verticalLabelWidth, fm.height()),
                         Qt::AlignRight | Qt::AlignVCenter, tick.label);
    }
    painter.drawText(
        QRect(rect.left(), y0 + tickLength + fm.height() + 2 * gap, rect.width(), fm.height()),
        Qt::AlignCenter, axes[0].label);
    painter.save();
    painter.translate(x0 - tickLength - gap - verticalTitleGap - layout.verticalLabelWidth - fm.height(),
                      rect.center().y());
    painter.rotate(-90);
    painter.drawText(QRect(-rect.height() / 2, 0, rect.height(), fm.height()), Qt::AlignCenter,
                     axes[1].label);
    painter.restore();
    return result;
}
} // namespace amrvis::qt
