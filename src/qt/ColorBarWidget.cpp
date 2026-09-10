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

// Preserve the requested notation when it fits. A fixed export rectangle
// falls back to compact notation rather than clipping part of a number.
QString boundedNumber(double value, const QString& format, const QFontMetrics& metrics, int width) {
    const auto digits = std::max(minimumDisplayDigits, formatDigits(format));
    return fitNumber(value, format, digits,
        1,
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

// "+1.256637062e-06" under the title, matplotlib's placement: the sign says
// the ticks are added to it.
QString offsetLabel(double offset, const QString& format)
{
    const auto text = formatNumber(offset, format);
    return offset < 0.0 || text.trimmed().startsWith('+')
        ? text : QStringLiteral("+") + text;
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
    return offsetForFormat(effectiveFormat());
}

double ColorBarWidget::offsetForFormat(const QString& format) const
{
    const auto candidate = tickOffset(m_minimum, m_maximum, m_logarithmic);
    if (!candidate) {
        return 0.0;
    }
    // The printed offset is part of every value. Never subtract digits that
    // the chosen format drops: that would misstate the entire range.
    bool ok = false;
    const auto printed = formatNumber(*candidate,
        conversionSpecifier(format)).toDouble(&ok);
    const auto tolerance = (m_maximum - m_minimum) / 100.0;
    return ok && std::isfinite(printed) && printed <= m_minimum
            && std::abs(printed - *candidate) <= tolerance
        ? printed : 0.0;
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

struct ColorBarWidget::LabelLayout {
    int margin;
    int titleBlock;
    int offsetHeight;
    int barWidth;
    int gap;
    int barHeight;
    int count;
    double offset;
    QString format;
    QString offsetText;
};

ColorBarWidget::LabelLayout ColorBarWidget::labelLayout(
    const QFontMetrics& metrics, int height, bool bounded, int width,
    const NumberPresentation* presentation) const
{
    const int labelHeight = metrics.height();
    LabelLayout result{};
    result.margin = bounded
        ? std::min(std::max(margin, labelHeight / 4), std::max(0, (height - 1) / 2))
        : margin;
    result.titleBlock = bounded
        ? (height >= 3 * labelHeight + 2 * result.margin ? labelHeight + result.margin : 0)
        : titleHeight;
    result.barWidth = bounded ? std::max(barWidth, labelHeight) : barWidth;
    result.gap = bounded ? std::max(margin, labelHeight / 4) : labelGap;
    const auto valueFormat = presentation != nullptr
        ? presentation->valueFormat : effectiveFormat();
    result.offset = presentation != nullptr
        ? (presentation->offsetLine ? offsetForFormat(valueFormat) : 0.0) : labelOffset();
    result.offsetText = offsetLabel(result.offset, valueFormat);
    bool offsetLine = presentation != nullptr ? presentation->offsetLine : result.offset != 0.0;
    if (!offsetLine || result.titleBlock == 0
        || height < 4 * labelHeight + 2 * result.margin
        || (width > 0 && metrics.horizontalAdvance(result.offsetText)
                > width - 2 * result.margin)) {
        result.offset = 0.0;
        result.offsetText.clear();
        offsetLine = false;
    }
    result.offsetHeight = offsetLine ? labelHeight : 0;
    result.barHeight = std::max(1,
        height - 2 * result.margin - result.titleBlock - result.offsetHeight);
    result.count = bounded
        ? std::clamp(result.barHeight / (labelHeight + 4), 0, labelCount) : labelCount;
    result.format = !offsetLine ? valueFormat
        : (presentation != nullptr ? presentation->tickFormat : tickFormat());
    // A reserved offset line can print +0 when the current offset is unusable.
    // Narrow ranges then need full-value precision; ordinary ranges retain
    // the frozen residual format.
    if (presentation != nullptr && result.offset == 0.0
        && displayDigits(m_minimum, m_maximum) > formatDigits(result.format)) {
        result.format = valueFormat;
    }
    return result;
}

ColorBarWidget::NumberPresentation ColorBarWidget::exportPresentation(
    const QFontMetrics& metrics, const QRect& target) const
{
    const auto labels = labelLayout(metrics, target.height(), true, target.width());
    return {effectiveFormat(), labels.format, labels.offsetHeight > 0};
}

void ColorBarWidget::paintBar(QPainter* painter, const QRect& target, bool transparentBackground,
                              bool boundedLabels, const NumberPresentation* presentation) const {
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
    const auto labels = labelLayout(painter->fontMetrics(), h, boundedLabels, w, presentation);
    const int paintMargin = labels.margin;
    const int titleBlock = labels.titleBlock;
    const int offsetHeight = labels.offsetHeight;
    const int paintTitleHeight = titleBlock + offsetHeight;
    const int paintBarWidth = labels.barWidth;
    const int paintLabelGap = labels.gap;

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
            labels.offsetText);
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
    const auto drawnOffset = labels.offset;
    const auto& format = labels.format;
    const int count = labels.count;
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
        if (painter->fontMetrics().horizontalAdvance(text) <= w - labelLeft - paintMargin) {
            painter->drawText(QRect(labelLeft, top, w - labelLeft - paintMargin, labelHeight),
                              Qt::AlignLeft | Qt::AlignVCenter, text);
        }
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
    const auto labels = labelLayout(metrics, height, true, 0);
    for (int label = 0; label < labels.count; ++label) {
        const double fraction = static_cast<double>(label) / std::max(1, labels.count - 1);
        const auto value = tickValue(m_minimum, m_maximum, m_logarithmic, fraction)
            - labels.offset;
        width = std::max(width, metrics.horizontalAdvance(
            boundedNumber(value, labels.format, metrics, maximumWidth)));
    }
    if (!labels.offsetText.isEmpty()) {
        width = std::max(width, metrics.horizontalAdvance(labels.offsetText)
            - labels.barWidth - labels.gap);
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
        const auto labels = labelLayout(fm, height(), false, 0);
        for (int label = 0; label < labels.count; ++label) {
            const auto fraction = static_cast<double>(label) / std::max(1, labels.count - 1);
            labelWidth = std::max(labelWidth, fm.horizontalAdvance(formatNumber(
                tickValue(m_minimum, m_maximum, m_logarithmic, fraction) - labels.offset,
                labels.format)));
        }
        if (!labels.offsetText.isEmpty()) {
            labelWidth = std::max(labelWidth,
                fm.horizontalAdvance(labels.offsetText) - barWidth - labelGap);
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
