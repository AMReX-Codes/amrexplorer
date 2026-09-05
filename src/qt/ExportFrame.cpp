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
    auto label = formatNumber(value == 0.0 ? 0.0 : value, format);
    if (metrics.horizontalAdvance(label) <= availableWidth) {
        return label;
    }
    // Never clip or elide numeric values. The layout reserves enough width
    // for scientific notation, independent of a frame's magnitude or sign.
    for (int precision = 6; precision >= 1; --precision) {
        label = QString::number(value, 'g', precision);
        if (metrics.horizontalAdvance(label) <= availableWidth) {
            return label;
        }
    }
    return {};
}

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
    // 11-point text at an intended figure width of 7 inches. Solve for the
    // margins as well: sizing text from the raster alone makes it too small
    // once the axes and color scale occupy part of the published figure.
    int fontPixels = 12;
    for (int iteration = 0; iteration < 24; ++iteration) {
        layout.font.setPixelSize(fontPixels);
        const QFontMetrics fm(layout.font);
        int glyphWidth = 0;
        for (const QChar character : QStringLiteral("0123456789.e+-")) {
            glyphWidth = std::max(glyphWidth, fm.horizontalAdvance(character));
        }
        const int maximumLabelWidth = 16 * glyphWidth;
        // Movies keep room for compact scientific notation, not sixteen
        // widest-case glyphs. Stills only need the labels they actually draw.
        const int growthWidth = reserveLabelGrowth
                                    ? std::max(fm.horizontalAdvance(QStringLiteral("-9e-308")),
                                               fm.horizontalAdvance(QStringLiteral("-9e+308")))
                                    : 0;
        layout.labelWidth = growthWidth;
        for (double endpoint : {axes[0].minimum, axes[0].maximum}) {
            layout.labelWidth = std::max(
                layout.labelWidth, fm.horizontalAdvance(exportNumber(endpoint, options.numberFormat,
                                                                     fm, maximumLabelWidth)));
        }
        layout.verticalLabelWidth = growthWidth;
        int xOverhang = 0;
        for (std::size_t axis = 0; axis < axes.size(); ++axis) {
            const int length = axis == 0 ? rasterSize.width() : rasterSize.height();
            const int spacing =
                axis == 0
                    ? std::max(fm.horizontalAdvance(exportNumber(
                                   axes[0].minimum, options.numberFormat, fm, maximumLabelWidth)),
                               fm.horizontalAdvance(exportNumber(
                                   axes[0].maximum, options.numberFormat, fm, maximumLabelWidth))) +
                          2 * fm.horizontalAdvance(QChar('0')) + 12
                    : fm.height() + 8;
            for (const auto& tick : exportTicks(axes[axis], length, spacing, options.numberFormat,
                                                fm, maximumLabelWidth)) {
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
        const int left =
            options.includeAxes
                ? std::max(layout.verticalLabelWidth + fm.height() + tickLength + 3 * gap,
                           xOverhang + gap)
                : 0;
        const int top = options.includeAxes ? fm.height() / 2 + gap : 0;
        const int bottom = options.includeAxes ? 2 * fm.height() + tickLength + 3 * gap : 0;
        const int right = options.includeAxes ? xOverhang + gap : 0;
        layout.dataRect = QRect(QPoint(left, top), rasterSize);
        int width = left + rasterSize.width() + right;
        if (options.includeColorBar) {
            const int labels =
                colorBar != nullptr
                    ? colorBar->exportLabelWidth(fm, maximumLabelWidth, rasterSize.height())
                    : growthWidth;
            const int barWidth = ColorBarWidget::exportWidth(fm, std::max(labels, growthWidth));
            // The color scale is beside the data, above the x tick labels.
            // Their endpoint overhang must not become an inter-panel gutter.
            layout.colorBarRect =
                QRect(left + rasterSize.width() + gap, top, barWidth, rasterSize.height());
            width = std::max(width, layout.colorBarRect.right() + 1);
        }
        layout.canvasSize = QSize(width, top + rasterSize.height() + bottom);
        const int nextFontPixels =
            std::max(12, static_cast<int>(std::lround(width * 11.0 / (72.0 * 7.0))));
        if (nextFontPixels == fontPixels) {
            break;
        }
        fontPixels = nextFontPixels;
    }
    layout.dotsPerMeter =
        static_cast<int>(std::lround(layout.font.pixelSize() * 72.0 / (11.0 * 0.0254)));
    return layout;
}

bool exportAspectMatches(QSize rasterSize, const ExportLayout& layout) {
    return !rasterSize.isEmpty() && !layout.dataRect.isEmpty() &&
           std::abs(static_cast<double>(rasterSize.height()) * layout.dataRect.width() /
                        rasterSize.width() -
                    layout.dataRect.height()) <= 1.0;
}

QImage composeExportImage(const QImage& raster, const std::array<ExportAxis, 2>& axes,
                          const ExportOptions& options, const ExportLayout& layout,
                          const ColorBarWidget* colorBar) {
    if (raster.isNull() || raster.size() != layout.dataRect.size()) {
        return {};
    }
    if (!options.includeAxes && !options.includeColorBar) {
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
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(layout.dataRect.topLeft(), raster);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (options.includeColorBar && colorBar != nullptr) {
        colorBar->paintBar(&painter, layout.colorBarRect, true, true);
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
    painter.drawLine(x0, rect.top(), x0, y0);
    painter.drawLine(x0, y0, rect.right(), y0);
    const int xLabelSpacing =
        std::max(fm.horizontalAdvance(
                     exportNumber(axes[0].minimum, options.numberFormat, fm, layout.labelWidth)),
                 fm.horizontalAdvance(
                     exportNumber(axes[0].maximum, options.numberFormat, fm, layout.labelWidth))) +
        2 * fm.horizontalAdvance(QChar('0')) + 12;
    for (const auto& tick : exportTicks(axes[0], rect.width(), xLabelSpacing, options.numberFormat,
                                        fm, layout.labelWidth)) {
        const double x = rect.left() + tick.fraction * (rect.width() - 1);
        painter.drawLine(QPointF(x, y0), QPointF(x, y0 + tickLength));
        painter.drawText(QRectF(x - layout.labelWidth / 2.0, y0 + tickLength + gap,
                                layout.labelWidth, fm.height()),
                         Qt::AlignHCenter | Qt::AlignTop, tick.label);
    }
    for (const auto& tick : exportTicks(axes[1], rect.height(), fm.height() + 8,
                                        options.numberFormat, fm, layout.verticalLabelWidth)) {
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
    painter.translate(x0 - tickLength - 2 * gap - layout.verticalLabelWidth - fm.height(),
                      rect.center().y());
    painter.rotate(-90);
    painter.drawText(QRect(-rect.height() / 2, 0, rect.height(), fm.height()), Qt::AlignCenter,
                     axes[1].label);
    painter.restore();
    return result;
}
} // namespace amrvis::qt
