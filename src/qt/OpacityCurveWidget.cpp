#include "OpacityCurveWidget.hpp"

#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
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

// What one arrow key press moves the selected point by. Sideways it is one
// palette slot, the finest step that can still change the sampled transfer
// table; upwards one percent of opacity. Both are around a pixel on a plot
// the width of the dock, which is the point: the mouse cannot do better than
// a pixel, and this is what the keys are for. Shift covers ground instead.
constexpr double positionStep
    = 1.0 / static_cast<double>(Palette::colorSlots - 1);
constexpr double opacityStep = 0.01;
// A multiplier on either step above, not a step itself.
constexpr double coarseMultiplier = 10.0;

} // namespace

OpacityCurveWidget::OpacityCurveWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(96);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(tr("Drag a point to shape the opacity; click the curve to add "
                  "one, right-click a point to remove it. Arrow keys nudge "
                  "the selected point, with Shift for a bigger step"));
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
    // These points are not the ones that were selected, whatever the indices.
    m_selected = -1;
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

    // The palette behind the curve, one image column per data slot scaled to
    // the plot, so a point sits over the colour it is shaping. A single
    // drawImage paints every pixel once; translucent per-slot rects compound
    // wherever they overlap and stripe the strip. Dimmed by painter opacity:
    // the bands are the reference, not the subject.
    if (m_palette != nullptr) {
        constexpr int entries = Palette::colorSlots;
        QImage strip(entries, 1, QImage::Format_RGB32);
        for (int entry = 0; entry < entries; ++entry) {
            strip.setPixel(entry, 0,
                m_palette->slotArgb(Palette::paletteStart + entry)
                    | 0xFF000000U);
        }
        // Well back when the control is inert. Everything drawn over the strip
        // greys out on its own, through QPalette::Disabled, but these are the
        // palette's own colours and would keep full strength -- leaving the
        // boldest thing on a disabled control the one part that does not say
        // so.
        painter.setOpacity(isEnabled() ? 200.0 / 255.0 : 60.0 / 255.0);
        painter.drawImage(plot, strip);
        painter.setOpacity(1.0);
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

    // The control points last, over the curve they define. The selected one is
    // marked in the highlight colour, so which point the arrow keys will move
    // is visible rather than remembered -- but only while the keys would
    // actually come here, or the mark would advertise an edit that is going to
    // whichever control took the focus instead.
    const auto marked = hasFocus() ? m_selected : -1;
    for (std::size_t index = 0; index < m_curve.size(); ++index) {
        const auto at = widgetPosition(m_curve[index]);
        painter.setBrush(palette().color(static_cast<int>(index) == marked
                ? QPalette::Highlight
                : QPalette::Base));
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

void OpacityCurveWidget::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    // The selection is kept across the focus change rather than dropped, so
    // coming back from another window finds the same point marked again.
    update();
}

void OpacityCurveWidget::focusOutEvent(QFocusEvent* event)
{
    QWidget::focusOutEvent(event);
    update();
}

void OpacityCurveWidget::keyPressEvent(QKeyEvent* event)
{
    // Arrows move the selected point, which is the last one pressed. With
    // nothing selected they are not ours: the dock's focus chain gets them
    // rather than a nudge landing on a point the user did not choose. Nor
    // during a drag, where the next mouse move would overwrite the nudge with
    // the cursor's own position and waste the render it asked for.
    if (m_dragging >= 0 || m_selected < 0
        || static_cast<std::size_t>(m_selected) >= m_curve.size()) {
        QWidget::keyPressEvent(event);
        return;
    }
    // Shift is the coarse step, and an arrow carrying anything else belongs to
    // whatever else may want it rather than being swallowed here. Keypad is
    // masked out rather than counted as a modifier because macOS stamps it on
    // the arrow keys -- the same reason, and the same handling, as
    // ImageView::keyPressEvent, which documents it at length.
    const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
    if (modifiers != Qt::NoModifier && modifiers != Qt::ShiftModifier) {
        QWidget::keyPressEvent(event);
        return;
    }
    const auto step = modifiers == Qt::ShiftModifier ? coarseMultiplier : 1.0;
    double dx = 0.0;
    double dy = 0.0;
    switch (event->key()) {
    case Qt::Key_Left:
        dx = -positionStep * step;
        break;
    case Qt::Key_Right:
        dx = positionStep * step;
        break;
    case Qt::Key_Down:
        dy = -opacityStep * step;
        break;
    case Qt::Key_Up:
        dy = opacityStep * step;
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    // Through moveOpacityPoint like a drag, so a nudge obeys the same rules:
    // the ends keep their positions, an interior point stops at its
    // neighbours, and opacity stays on [0, 1].
    const auto index = static_cast<std::size_t>(m_selected);
    const auto before = m_curve[index];
    moveOpacityPoint(m_curve, index, before.position + dx, before.opacity + dy);
    // Only a real move is worth a signal. Those rules mean a key held against
    // a limit -- an end asked to change position, a point already up against
    // its neighbour or the top of the plot -- moves nothing, and every repeat
    // would otherwise restart the settle timer and re-render an identical
    // volume, so the full frame would never arrive until the key came up. The
    // key is still ours: it is accepted either way rather than escaping to pan
    // a panel while the curve is being edited.
    if (m_curve[index].position == before.position
        && m_curve[index].opacity == before.opacity) {
        event->accept();
        return;
    }
    update();
    emit curveChanged();
    event->accept();
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
            // The list closed up over the gap, so an index past the removed
            // point now names its neighbour: follow the point, and drop the
            // selection entirely if it was the point that just went.
            if (m_selected == hit) {
                m_selected = -1;
            } else if (m_selected > hit) {
                --m_selected;
            }
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
    // Whichever point the press ended up on is the one the arrow keys take,
    // and it stays selected after the button is released: placing a point
    // roughly and then nudging it is the whole gesture.
    m_selected = m_dragging;
    update();
    event->accept();
}

void OpacityCurveWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging < 0) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const auto index = static_cast<std::size_t>(m_dragging);
    const auto before = m_curve[index];
    const auto moved = curvePosition(event->position());
    moveOpacityPoint(m_curve, index, moved.position, moved.opacity);
    // Only a real move is worth a signal, for the reason keyPressEvent gives:
    // an end dragged sideways, or an interior point dragged into a neighbour,
    // moves nothing, and every mouse-move event would still restart the settle
    // timer and re-render a volume that had not changed.
    if (m_curve[index].position != before.position
        || m_curve[index].opacity != before.opacity) {
        update();
        emit curveChanged();
    }
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
