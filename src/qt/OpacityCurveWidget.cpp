#include "OpacityCurveWidget.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPolygonF>
#include <QSizePolicy>

#include <algorithm>
#include <cmath>

namespace amrvis::qt {

namespace {

// Half the side of a control point's square, in pixels, and the distance a
// press may miss one by and still take it. The grab radius is the larger of
// the two: a 3-pixel square is hard to hit exactly with a mouse.
constexpr double handleHalf = 3.0;
constexpr double grabRadius = 6.0;

} // namespace

OpacityCurveWidget::OpacityCurveWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(96);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(tr("Drag a point to shape the opacity; click the curve to add "
                  "one, right-click a point to remove it"));
    // The plot is the whole widget, and every press inside it means something,
    // so it takes the mouse rather than passing clicks to the dock.
    setFocusPolicy(Qt::ClickFocus);
}

void OpacityCurveWidget::setColorPalette(const Palette* palette)
{
    m_palette = palette;
    update();
}

void OpacityCurveWidget::setCurve(std::vector<OpacityPoint> curve)
{
    // A curve with fewer than two points cannot span the range, and every
    // edit below assumes two ends it may not remove.
    m_curve = curve.size() >= 2 ? std::move(curve) : defaultOpacityCurve();
    m_dragging = -1;
    update();
}

QRectF OpacityCurveWidget::plotRect() const
{
    return QRectF(rect()).adjusted(
        handleHalf + 1.0, handleHalf + 1.0, -handleHalf - 1.0, -handleHalf - 1.0);
}

QPointF OpacityCurveWidget::widgetPosition(const OpacityPoint& point) const
{
    const auto plot = plotRect();
    // Opacity counts up, pixels count down.
    return QPointF(plot.left() + point.position * plot.width(),
        plot.bottom() - point.opacity * plot.height());
}

OpacityPoint OpacityCurveWidget::curvePosition(const QPointF& position) const
{
    const auto plot = plotRect();
    if (!(plot.width() > 0.0) || !(plot.height() > 0.0)) {
        return {};
    }
    return {std::clamp((position.x() - plot.left()) / plot.width(), 0.0, 1.0),
        std::clamp((plot.bottom() - position.y()) / plot.height(), 0.0, 1.0)};
}

int OpacityCurveWidget::pointAt(const QPointF& position) const
{
    int closest = -1;
    double best = grabRadius * grabRadius;
    for (std::size_t index = 0; index < m_curve.size(); ++index) {
        const auto at = widgetPosition(m_curve[index]);
        const auto dx = at.x() - position.x();
        const auto dy = at.y() - position.y();
        const auto distance = dx * dx + dy * dy;
        // Strictly nearer, so two points at one position give the lower index
        // and a drag is repeatable rather than depending on iteration order.
        if (distance < best) {
            best = distance;
            closest = static_cast<int>(index);
        }
    }
    return closest;
}

void OpacityCurveWidget::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), palette().color(QPalette::Base));
    const auto plot = plotRect();
    if (!(plot.width() > 0.0) || !(plot.height() > 0.0)) {
        return;
    }

    // The palette behind the curve, one band per data slot, so a point sits
    // over the colour it is shaping. Dimmed by the curve drawn over it: the
    // bands are the reference, not the subject.
    if (m_palette != nullptr) {
        constexpr int entries = Palette::colorSlots;
        const auto bandWidth = plot.width() / static_cast<double>(entries);
        for (int entry = 0; entry < entries; ++entry) {
            const auto argb
                = m_palette->slotArgb(Palette::paletteStart + entry);
            QColor band(QRgb(argb | 0xFF000000U));
            band.setAlpha(90);
            const auto left
                = plot.left() + static_cast<double>(entry) * bandWidth;
            // A whole pixel wider than the band, so rounding cannot leave a
            // background-coloured seam between two bands.
            painter.fillRect(
                QRectF(left, plot.top(), bandWidth + 1.0, plot.height()), band);
        }
    }

    painter.setRenderHint(QPainter::Antialiasing, true);
    // The area under the curve, so "more opaque" reads as "more filled" at a
    // glance rather than needing the axis to be worked out.
    QPolygonF under;
    under << QPointF(plot.left(), plot.bottom());
    for (const auto& point : m_curve) {
        under << widgetPosition(point);
    }
    under << QPointF(plot.right(), plot.bottom());
    auto fill = palette().color(QPalette::WindowText);
    fill.setAlpha(40);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawPolygon(under);

    QPolygonF line;
    for (const auto& point : m_curve) {
        line << widgetPosition(point);
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(palette().color(QPalette::WindowText), 2.0));
    painter.drawPolyline(line);

    // The control points last, over the curve they define.
    painter.setBrush(palette().color(QPalette::Base));
    for (std::size_t index = 0; index < m_curve.size(); ++index) {
        const auto at = widgetPosition(m_curve[index]);
        painter.setPen(QPen(palette().color(QPalette::WindowText),
            static_cast<int>(index) == m_dragging ? 2.5 : 1.5));
        painter.drawRect(QRectF(at.x() - handleHalf, at.y() - handleHalf,
            2.0 * handleHalf, 2.0 * handleHalf));
    }

    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawRect(plot);
}

void OpacityCurveWidget::mousePressEvent(QMouseEvent* event)
{
    const auto position = event->position();
    const auto hit = pointAt(position);
    if (event->button() == Qt::RightButton) {
        // Removal, and only of a point actually under the cursor: a
        // right-click on empty plot is not an attempt to delete the nearest
        // thing. removeOpacityPoint refuses the two ends on its own.
        if (hit >= 0
            && removeOpacityPoint(m_curve, static_cast<std::size_t>(hit))) {
            m_dragging = -1;
            update();
            emit curveChanged();
        }
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (hit >= 0) {
        m_dragging = hit;
    } else {
        // A press on empty plot adds a point there and drags it, so shaping a
        // curve is one gesture per point rather than click-then-find-it.
        const auto added = curvePosition(position);
        m_dragging = static_cast<int>(
            insertOpacityPoint(m_curve, added.position, added.opacity));
        emit curveChanged();
    }
    update();
    event->accept();
}

void OpacityCurveWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging < 0) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const auto moved = curvePosition(event->position());
    moveOpacityPoint(m_curve, static_cast<std::size_t>(m_dragging),
        moved.position, moved.opacity);
    update();
    emit curveChanged();
    event->accept();
}

void OpacityCurveWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragging < 0) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = -1;
    // No signal here: every step of the drag already emitted one, and the
    // controller's settle timer turns the last of them into the full frame.
    update();
    event->accept();
}

} // namespace amrvis::qt
