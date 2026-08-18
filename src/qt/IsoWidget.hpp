#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/OrthoProjection.hpp>

#include <QColor>
#include <QPoint>
#include <QWidget>

#include <array>
#include <vector>

class QMouseEvent;
class QPainter;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QWheelEvent;

namespace amrvis {
class Palette;
}

namespace amrvis::qt {

// The bottom-right quadrant of the 3-D layout: an orthographic wireframe
// of the physical domain and the per-level grid boxes, with the three
// current slice planes drawn as translucent quads.  Drag to rotate, wheel
// to zoom, matching the legacy Amrvis iso view interaction. The projection
// is the shared OrthoCamera (core/OrthoProjection.hpp), the one the volume
// renderer uses, so a rendered volume placed under this wireframe lines up.
class IsoWidget final : public QWidget {
    Q_OBJECT

public:
    explicit IsoWidget(QWidget* parent = nullptr);

    using QWidget::setGeometry;
    void setGeometry(const DatasetMetadata& metadata);
    void setSlicePositions(double x, double y, double z);
    void setSlicePlanesVisible(bool visible);
    void setColorPalette(const Palette* palette);

    // The camera the widget draws with; drag, wheel and the preset buttons
    // change it and emit cameraChanged, a release after a drag emits
    // interactionEnded.
    [[nodiscard]] const OrthoCamera& camera() const noexcept { return m_camera; }
    void setCamera(const OrthoCamera& camera);

signals:
    void cameraChanged();
    void interactionEnded();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct LevelBoxes {
        int level = 0;
        IntBox domain;
        Real3 cellSize;
        Real3 indexOrigin;
        std::vector<IntBox> boxes;
    };
    [[nodiscard]] QPointF project(const ViewportFrame& frame,
        double x, double y, double z) const;
    void drawBox(QPainter& painter, const ViewportFrame& frame,
        const RealBox& box, const QPen& pen) const;
    void drawSlicePlane(QPainter& painter, const ViewportFrame& frame,
        int axis) const;
    void drawAxisIndicator(QPainter& painter) const;
    [[nodiscard]] RealBox physicalBox(const LevelBoxes& level,
        const IntBox& box) const;
    [[nodiscard]] QColor levelOutlineColor(int level) const;
    [[nodiscard]] QColor slicePlaneColor(int axis) const;
    void setViewAngles(double azimuth, double elevation);
    void layoutButtons();

    RealBox m_domain{};
    std::vector<LevelBoxes> m_levels;
    std::array<double, 3> m_slicePositions{0.0, 0.0, 0.0};
    bool m_slicePlanesVisible = false;
    const Palette* m_palette = nullptr;
    bool m_hasGeometry = false;

    OrthoCamera m_camera;
    QPoint m_lastMousePos;
    bool m_dragging = false;

    QPushButton* m_btnXY = nullptr;
    QPushButton* m_btnXZ = nullptr;
    QPushButton* m_btnYZ = nullptr;
};

} // namespace amrvis::qt
