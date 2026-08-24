#pragma once

// The opacity curve, drawn over the palette it shapes and edited in place.
// Replaces the two-threshold window and the maximum: it says the same kind of
// thing with as many control points as the field needs, so a feature buried
// between two others can be given its own opacity without also revealing them.
//
// The editing rules are not here. They are pure functions on the point list in
// VolumePipeline.hpp -- insert, move, remove, evaluate -- because that is where
// the mistakes live (a point dragged past its neighbour, an end point pulled
// off the range) and they are worth pinning without a widget to drive. This
// class maps pixels to and from those points and paints the result.

#include <amrexplorer/pipeline/VolumePipeline.hpp>
#include <amrexplorer/render2d/Palette.hpp>

#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <cstddef>
#include <vector>

namespace amrvis::qt {

class OpacityCurveWidget final : public QWidget {
    Q_OBJECT

public:
    explicit OpacityCurveWidget(QWidget* parent = nullptr);

    // The palette painted behind the curve, so a control point sits over the
    // colour it makes transparent. Null draws the plot without it.
    void setColorPalette(const Palette* palette);
    [[nodiscard]] const std::vector<OpacityPoint>& curve() const noexcept
    {
        return m_curve;
    }
    // Replaces the curve without emitting curveChanged: for restoring one, not
    // for editing it.
    void setCurve(std::vector<OpacityPoint> curve);

signals:
    // One per edit, including every step of a drag -- the same shape the
    // sliders had, so the window relays it as rampChanged and the controller's
    // draft-then-settle path carries it with nothing new wired.
    void curveChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    // Only to repaint: the selected point is marked while this has the focus
    // the arrow keys follow, so gaining or losing it changes the picture.
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    // The plot inside its border, inset far enough that a control point on any
    // edge is drawn whole rather than clipped in half.
    [[nodiscard]] QRectF plotRect() const;
    [[nodiscard]] QPointF widgetPosition(const OpacityPoint& point) const;
    [[nodiscard]] OpacityPoint curvePosition(const QPointF& position) const;
    // The index of the control point under `position`, or -1.
    [[nodiscard]] int pointAt(const QPointF& position) const;

    const Palette* m_palette = nullptr;
    std::vector<OpacityPoint> m_curve = defaultOpacityCurve();
    // The point being dragged, or -1. Held as an index because the edit
    // functions take one, and a drag cannot reorder the list -- moveOpacityPoint
    // clamps an interior point between its neighbours.
    int m_dragging = -1;
    // The point the arrow keys nudge: the last one pressed, outliving the
    // drag so a coarse mouse placement can be fine-tuned afterwards.
    int m_selected = -1;
};

} // namespace amrvis::qt
