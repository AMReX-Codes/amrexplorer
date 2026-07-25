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

// Value at a given label fraction (0 = top = max, 1 = bottom = min), honoring
// log spacing so the labels match the drawn gradient.
double tickValue(double minimum, double maximum, bool logarithmic, double fraction)
{
    return logarithmic
        ? minimum * std::pow(maximum / minimum, 1.0 - fraction)
        : maximum + fraction * (minimum - maximum);
}

// Pixel width of the widest tick label for the format/range. Because it
// measures the actual formatted strings, exponent forms (from %e / %g) are
// included at their full width.
int maxTickLabelWidth(const QFontMetrics& fm, double minimum, double maximum,
    bool logarithmic, const QString& format)
{
    int maxWidth = 0;
    for (int label = 0; label < labelCount; ++label) {
        const auto fraction = static_cast<double>(label)
            / static_cast<double>(labelCount - 1);
        maxWidth = std::max(maxWidth,
            fm.horizontalAdvance(formatNumber(
                tickValue(minimum, maximum, logarithmic, fraction), format)));
    }
    return maxWidth;
}

} // namespace

ColorBarWidget::ColorBarWidget(QWidget* parent)
    : QWidget(parent)
    , m_numberFormat(defaultNumberFormat())
{
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
    update();
}

void ColorBarWidget::setNumberFormat(QString format)
{
    m_numberFormat = std::move(format);
    update();
}

void ColorBarWidget::setLogarithmic(bool logarithmic)
{
    m_logarithmic = logarithmic;
    update();
}

void ColorBarWidget::clearRange()
{
    m_fieldName.clear();
    m_hasRange = false;
    update();
}

void ColorBarWidget::paintBar(QPainter* painter, const QRect& target) const
{
    painter->save();
    painter->translate(target.topLeft());
    const int w = target.width();
    const int h = target.height();
    painter->fillRect(0, 0, w, h, viewportBackground());
    painter->setPen(Qt::white);

    if (!m_hasRange) {
        painter->drawText(QRect(margin, margin, w - 2 * margin, h - 2 * margin),
            Qt::AlignCenter | Qt::TextWordWrap, tr("No scalar range"));
        painter->restore();
        return;
    }

    const QRect bar(margin, margin + titleHeight, barWidth,
        std::max(1, h - 2 * margin - titleHeight));
    const auto title = painter->fontMetrics().elidedText(
        m_fieldName, Qt::ElideRight, w - 2 * margin);
    painter->drawText(QRect(margin, margin, w - 2 * margin, titleHeight),
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
    painter->setPen(Qt::white);
    painter->drawRect(bar.adjusted(0, 0, -1, -1));

    const auto labelHeight = painter->fontMetrics().height();
    const auto labelLeft = bar.left() + bar.width() + labelGap;
    for (int label = 0; label < labelCount; ++label) {
        const auto fraction = static_cast<double>(label)
            / static_cast<double>(labelCount - 1);
        // In log mode the labels must be geometrically spaced to match the
        // gradient: the color at vertical position `fraction` (from the top)
        // corresponds to min*(max/min)^(1-fraction).
        const auto value = tickValue(m_minimum, m_maximum, m_logarithmic, fraction);
        const auto center = bar.top()
            + static_cast<int>(std::lround(fraction * static_cast<double>(rows)));
        const auto top = std::clamp(center - labelHeight / 2, bar.top(),
            std::max(bar.top(), bar.top() + bar.height() - labelHeight));
        painter->drawText(QRect(labelLeft, top, w - labelLeft - margin,
                labelHeight), Qt::AlignLeft | Qt::AlignVCenter,
            formatNumber(value, m_numberFormat));
    }
    painter->restore();
}

void ColorBarWidget::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    QPainter painter(this);
    paintBar(&painter, rect());
}

int ColorBarWidget::preferredWidth() const
{
    const QFontMetrics fm = fontMetrics();
    int labelWidth = 0;
    if (m_hasRange) {
        labelWidth = maxTickLabelWidth(
            fm, m_minimum, m_maximum, m_logarithmic, m_numberFormat);
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
