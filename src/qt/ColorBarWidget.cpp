#include "ColorBarWidget.hpp"
#include "NumberFormat.hpp"
#include "Theme.hpp"

#include <amrexplorer/render2d/Palette.hpp>

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>
#include <optional>
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

// Never below what a plain %g always showed, and never above what the range
// asked for: the digits are the point of the label, so width yields to them
// rather than the other way round.
QString boundedNumber(double value, const QString& format, const QFontMetrics& metrics, int width) {
    const auto digits = formatDigits(format);
    return fitNumber(value, format, digits,
        std::min(digits, minimumDisplayDigits),
        [&metrics](const QString& text) {
            return metrics.horizontalAdvance(text);
        },
        width);
}

// The common leading part of a narrow range, factored out of the tick labels
// the way matplotlib's ScalarFormatter does: the ticks then read as short
// residuals and one offset label carries the magnitude, instead of eight
// labels repeating the same twelve digits.
//
// Offered only when the ticks share a sign and the span is small next to the
// magnitude -- matplotlib's threshold is three orders of magnitude -- so an
// ordinary range is untouched. Not in log mode, where ticks are decades apart
// and share no leading part.
std::optional<double> tickOffset(double minimum, double maximum, bool logarithmic)
{
    if (logarithmic || !std::isfinite(minimum) || !std::isfinite(maximum)) {
        return std::nullopt;
    }
    const auto low = std::min(minimum, maximum);
    const auto high = std::max(minimum, maximum);
    if (low == high || (low <= 0.0 && high >= 0.0)) {
        return std::nullopt;
    }
    const auto span = high - low;
    const auto scale = std::max(std::abs(low), std::abs(high));
    if (!(span < scale / 1000.0)) {
        return std::nullopt;
    }
    // Round down to the decade just below the span, so the offset is a clean
    // number and every residual is positive.
    const auto step = std::pow(10.0, std::floor(std::log10(span)));
    if (!std::isfinite(step) || !(step > 0.0)) {
        return std::nullopt;
    }
    const auto offset = std::floor(low / step) * step;
    if (!std::isfinite(offset) || offset == 0.0) {
        return std::nullopt;
    }
    return offset;
}

// Pixel width of the widest tick label for the format/range. Because it
// measures the actual formatted strings, exponent forms (from %e / %g) are
// included at their full width.
int maxTickLabelWidth(const QFontMetrics& fm, double minimum, double maximum,
    bool logarithmic, const QString& format, double offset)
{
    int maxWidth = 0;
    for (int label = 0; label < labelCount; ++label) {
        const auto fraction = static_cast<double>(label)
            / static_cast<double>(labelCount - 1);
        maxWidth = std::max(maxWidth,
            fm.horizontalAdvance(formatNumber(
                ColorBarWidget::tickValue(minimum, maximum, logarithmic, fraction) - offset,
                format)));
    }
    return maxWidth;
}

// "+1.256637062e-06" under the title, matplotlib's placement: the sign says
// the ticks are added to it.
QString offsetLabel(double offset, const QString& format)
{
    const auto text = formatNumber(offset, format);
    return offset < 0.0 ? text : QStringLiteral("+") + text;
}

} // namespace

double ColorBarWidget::tickValue(
    double minimum, double maximum, bool logarithmic, double fraction)
{
    if (fraction == 0.0) {
        return maximum;
    }
    if (fraction == 1.0) {
        return minimum;
    }
    return logarithmic
        ? std::clamp(std::exp(std::lerp(std::log(maximum), std::log(minimum),
                         fraction)), minimum, maximum)
        : std::lerp(maximum, minimum, fraction);
}

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

void ColorBarWidget::setLogarithmic(bool logarithmic)
{
    m_logarithmic = logarithmic;
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

QString ColorBarWidget::effectiveFormat() const
{
    return resolveNumberFormat(m_numberFormat, m_minimum, m_maximum);
}

double ColorBarWidget::labelOffset() const
{
    return tickOffset(m_minimum, m_maximum, m_logarithmic).value_or(0.0);
}

// With an offset in play the ticks carry only the residual, whose own span
// needs no extra digits; without one they carry the value and do.
QString ColorBarWidget::tickFormat() const
{
    const auto offset = labelOffset();
    if (offset == 0.0) {
        return effectiveFormat();
    }
    return resolveNumberFormat(
        m_numberFormat, m_minimum - offset, m_maximum - offset);
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
    const int titleBlock =
        boundedLabels ? (h >= 3 * labelHeight + 2 * paintMargin ? labelHeight + paintMargin : 0)
                      : titleHeight;
    // One more line under the title when the ticks carry a residual, so the
    // offset that completes them is beside them rather than implied.
    const auto offset = labelOffset();
    const int offsetHeight
        = (offset != 0.0 && titleBlock > 0 && h >= 4 * labelHeight + 2 * paintMargin)
        ? labelHeight
        : 0;
    const int paintTitleHeight = titleBlock + offsetHeight;
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
    painter->drawText(QRect(paintMargin, paintMargin, w - 2 * paintMargin, titleBlock),
                      Qt::AlignLeft | Qt::AlignVCenter, title);
    if (offsetHeight > 0) {
        painter->drawText(
            QRect(paintMargin, paintMargin + titleBlock, w - 2 * paintMargin,
                offsetHeight),
            Qt::AlignLeft | Qt::AlignVCenter,
            offsetLabel(offset, effectiveFormat()));
    }

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
    // Residuals when the offset is drawn; the values themselves when it is
    // not -- a panel too short for the offset line must not print ticks that
    // silently omit it.
    const auto drawnOffset = offsetHeight > 0 ? offset : 0.0;
    const auto format = drawnOffset == 0.0 ? effectiveFormat() : tickFormat();
    const int count =
        boundedLabels ? std::clamp(bar.height() / (labelHeight + 4), 0, labelCount) : labelCount;
    for (int label = 0; label < count; ++label) {
        const auto fraction =
            static_cast<double>(label) / static_cast<double>(std::max(1, count - 1));
        // In log mode the labels must be geometrically spaced to match the
        // gradient: the color at vertical position `fraction` (from the top)
        // corresponds to min*(max/min)^(1-fraction).
        const auto value
            = ColorBarWidget::tickValue(m_minimum, m_maximum, m_logarithmic, fraction)
            - drawnOffset;
        const auto center = bar.top()
            + static_cast<int>(std::lround(fraction * static_cast<double>(rows)));
        const auto top = std::clamp(center - labelHeight / 2, bar.top(),
            std::max(bar.top(), bar.top() + bar.height() - labelHeight));
        const auto text = boundedLabels
                              ? boundedNumber(value, format, painter->fontMetrics(),
                                              w - labelLeft - paintMargin)
                              : formatNumber(value, format);
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
                                    ColorBarWidget::tickValue(m_minimum, m_maximum, m_logarithmic, fraction),
                                    effectiveFormat(), metrics, maximumWidth)));
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
        const auto offset = labelOffset();
        labelWidth = maxTickLabelWidth(
            fm, m_minimum, m_maximum, m_logarithmic, tickFormat(), offset);
        if (offset != 0.0) {
            // The offset line runs the full panel width rather than sitting in
            // the label column, so it has to fit too.
            labelWidth = std::max(labelWidth,
                fm.horizontalAdvance(offsetLabel(offset, effectiveFormat()))
                    - barWidth - labelGap);
        }
    }
    // A 6-digit %g label is at most 13 characters (e.g. "-1.23456e-308"), so
    // reserving that width keeps the panel from twitching across ordinary
    // ranges. It is a floor, not a cap: a narrow range resolves to more digits
    // and a wide format (say %f on large magnitudes) is wider still, and both
    // grow past it via the max() above, so nothing clips.
    labelWidth = std::max(labelWidth,
        fm.horizontalAdvance(QStringLiteral("-1.23456e-308")));
    return 2 * margin + barWidth + labelGap + labelWidth;
}

} // namespace amrvis::qt
