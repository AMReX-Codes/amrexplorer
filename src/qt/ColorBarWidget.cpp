#include "ColorBarWidget.hpp"
#include "NumberFormat.hpp"
#include "Theme.hpp"

#include <amrexplorer/render2d/Palette.hpp>

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace amrvis::qt {

namespace {

// Legacy Amrvis TOTALPALWIDTH: the whole color bar panel is 150 px wide
// (see ColorBarWidget::panelWidth).
constexpr int margin = 8;
constexpr int titleHeight = 24;
constexpr int barWidth = 24;
constexpr int labelGap = 6;
constexpr int labelCount = 8;

QString boundedNumber(double value, const QString& format, const QFontMetrics& metrics, int width) {
    auto text = formatNumber(value, format);
    for (int precision = 6; precision >= 1 && metrics.horizontalAdvance(text) > width;
         --precision) {
        text = QString::number(value, 'g', precision);
    }
    return text;
}

// Value at a given label fraction (0 = top = max, 1 = bottom = min), honoring
// log spacing so the labels match the drawn gradient.
double tickValue(double minimum, double maximum, ColorScaleConfig scale,
    double fraction)
{
    if (scale.scale == ColorScale::Linear) {
        return maximum + fraction * (minimum - maximum);
    }
    if (scale.scale == ColorScale::Logarithmic) {
        return minimum * std::pow(maximum / minimum, 1.0 - fraction);
    }
    const auto range = resolveValueRange(minimum, maximum, scale);
    if (!range) return maximum + fraction * (minimum - maximum);
    return inverseTransformedValue(
        range->minimum + (1.0 - fraction) * range->span, scale);
}

// Pixel width of the widest tick label for the format/range. Because it
// measures the actual formatted strings, exponent forms (from %e / %g) are
// included at their full width.
int maxTickLabelWidth(const QFontMetrics& fm, double minimum, double maximum,
    ColorScaleConfig scale, const QString& format)
{
    int maxWidth = 0;
    for (int label = 0; label < labelCount; ++label) {
        const auto fraction = static_cast<double>(label)
            / static_cast<double>(labelCount - 1);
        maxWidth = std::max(maxWidth,
            fm.horizontalAdvance(formatNumber(
                tickValue(minimum, maximum, scale, fraction), format)));
    }
    return maxWidth;
}

} // namespace

ColorBarWidget::ColorBarWidget(QWidget* parent)
    : QWidget(parent)
    , m_numberFormat(defaultNumberFormat())
{
    // panelWidth is the floor, not the width: a wide user format (say
    // "%.10e") produces tick labels past 150 px, and a fixed width clipped
    // them on screen while exports -- which size themselves with
    // preferredWidth -- came out fine. Grow to fit whatever the labels need.
    setFixedWidth(panelWidth);
    setMinimumHeight(280);
}

void ColorBarWidget::setPalette(const amrvis::Palette* palette)
{
    m_palette = palette;
    update();
}

void ColorBarWidget::setFieldRange(QString fieldName, double minimum, double maximum)
{
    m_fieldName = std::move(fieldName);
    m_minimum = minimum;
    m_maximum = maximum;
    m_hasRange = true;
    applyPreferredWidth();
    update();
}

void ColorBarWidget::setNumberFormat(QString format)
{
    m_numberFormat = std::move(format);
    applyPreferredWidth();
    update();
}

void ColorBarWidget::setScale(ColorScaleConfig scale)
{
    m_scale = scale;
    applyPreferredWidth();
    update();
}

void ColorBarWidget::clearRange()
{
    m_fieldName.clear();
    m_hasRange = false;
    applyPreferredWidth();
    update();
}

void ColorBarWidget::applyPreferredWidth()
{
    setFixedWidth(std::max(panelWidth, preferredWidth()));
}

void ColorBarWidget::paintBar(QPainter* painter, const QRect& target, bool transparentBackground,
                              bool boundedLabels) const {
    painter->save();
    painter->translate(target.topLeft());
    const int w = target.width();
    const int h = target.height();
    if (!transparentBackground) {
        painter->fillRect(0, 0, w, h, viewportBackground());
    }
    const QColor foreground = transparentBackground ? Qt::black : Qt::white;
    painter->setPen(foreground);
    const int labelHeight = painter->fontMetrics().height();
    const int paintMargin =
        boundedLabels ? std::min(std::max(margin, labelHeight / 4), std::max(0, (h - 1) / 2))
                      : margin;
    const int paintTitleHeight =
        boundedLabels ? (h >= 3 * labelHeight + 2 * paintMargin ? labelHeight + paintMargin : 0)
                      : titleHeight;
    const int paintBarWidth = boundedLabels ? std::max(barWidth, labelHeight) : barWidth;
    const int paintLabelGap = boundedLabels ? std::max(margin, labelHeight / 4) : labelGap;

    if (!m_hasRange) {
        painter->drawText(QRect(margin, margin, w - 2 * margin, h - 2 * margin),
            Qt::AlignCenter | Qt::TextWordWrap, tr("No scalar range"));
        painter->restore();
        return;
    }

    const QRect bar(paintMargin, paintMargin + paintTitleHeight, paintBarWidth,
                    std::max(1, h - 2 * paintMargin - paintTitleHeight));
    const auto title =
        painter->fontMetrics().elidedText(m_fieldName, Qt::ElideRight, w - 2 * paintMargin);
    painter->drawText(QRect(paintMargin, paintMargin, w - 2 * paintMargin, paintTitleHeight),
                      Qt::AlignLeft | Qt::AlignVCenter, title);

    const auto& palette = m_palette != nullptr
        ? *m_palette : builtinPalette(BuiltinPalette::Rainbow);
    const auto rows = std::max(1, bar.height() - 1);
    for (int row = 0; row < bar.height(); ++row) {
        const auto normalized = 1.0
            - static_cast<double>(row) / static_cast<double>(rows);
        painter->setPen(QColor::fromRgb(static_cast<QRgb>(palette.argb(normalized))));
        painter->drawLine(bar.left(), bar.top() + row,
            bar.left() + bar.width() - 1, bar.top() + row);
    }
    painter->setPen(foreground);
    painter->drawRect(bar.adjusted(0, 0, -1, -1));

    const auto labelLeft = bar.left() + bar.width() + paintLabelGap;
    const int count =
        boundedLabels ? std::clamp(bar.height() / (labelHeight + 4), 0, labelCount) : labelCount;
    for (int label = 0; label < count; ++label) {
        const auto fraction =
            static_cast<double>(label) / static_cast<double>(std::max(1, count - 1));
        // In log mode the labels must be geometrically spaced to match the
        // gradient: the color at vertical position `fraction` (from the top)
        // corresponds to min*(max/min)^(1-fraction).
        const auto value = tickValue(m_minimum, m_maximum, m_scale, fraction);
        const auto center = bar.top()
            + static_cast<int>(std::lround(fraction * static_cast<double>(rows)));
        const auto top = std::clamp(center - labelHeight / 2, bar.top(),
            std::max(bar.top(), bar.top() + bar.height() - labelHeight));
        const auto text = boundedLabels
                              ? boundedNumber(value, m_numberFormat, painter->fontMetrics(),
                                              w - labelLeft - paintMargin)
                              : formatNumber(value, m_numberFormat);
        painter->drawText(QRect(labelLeft, top, w - labelLeft - paintMargin, labelHeight),
                          Qt::AlignLeft | Qt::AlignVCenter, text);
    }
    painter->restore();
}

void ColorBarWidget::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    QPainter painter(this);
    paintBar(&painter, rect());
}

int ColorBarWidget::exportWidth(const QFontMetrics& metrics, int labelWidth) {
    return labelWidth + std::max(barWidth, metrics.height()) +
           3 * std::max(margin, metrics.height() / 4);
}

int ColorBarWidget::exportLabelWidth(const QFontMetrics& metrics, int maximumWidth,
                                     int height) const {
    int width = 0;
    const int labelHeight = metrics.height();
    const int paintMargin =
        std::min(std::max(margin, labelHeight / 4), std::max(0, (height - 1) / 2));
    const int paintTitleHeight =
        height >= 3 * labelHeight + 2 * paintMargin ? labelHeight + paintMargin : 0;
    const int barHeight = std::max(1, height - 2 * paintMargin - paintTitleHeight);
    const int count = std::clamp(barHeight / (labelHeight + 4), 0, labelCount);
    for (int label = 0; label < count; ++label) {
        const double fraction = static_cast<double>(label) / std::max(1, count - 1);
        width = std::max(width, metrics.horizontalAdvance(boundedNumber(
                                    tickValue(m_minimum, m_maximum, m_scale, fraction),
                                    m_numberFormat, metrics, maximumWidth)));
    }
    // Keep short field names intact without allowing long expressions to
    // dictate the width of the entire figure (paintBar elides those).
    const int titleWidth = std::min(maximumWidth, metrics.horizontalAdvance(m_fieldName));
    return std::max(width, titleWidth - std::max(barWidth, metrics.height()) -
                               std::max(margin, metrics.height() / 4));
}

int ColorBarWidget::preferredWidth() const
{
    const QFontMetrics fm = fontMetrics();
    int labelWidth = 0;
    if (m_hasRange) {
        labelWidth = maxTickLabelWidth(
            fm, m_minimum, m_maximum, m_scale, m_numberFormat);
    }
    // The default %g format tops out at 13 characters (e.g. "-1.23456e-308"),
    // so reserving that width keeps the panel stable across ranges while still
    // fitting every %g label. A wider format (e.g. %f on large magnitudes)
    // grows past it via the max() above, so nothing clips.
    labelWidth = std::max(labelWidth,
        fm.horizontalAdvance(QStringLiteral("-1.23456e-308")));
    return 2 * margin + barWidth + labelGap + labelWidth;
}

} // namespace amrvis::qt
