#pragma once

#include <amrexplorer/core/Metadata.hpp>

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
// to zoom, matching the legacy Amrvis iso view interaction.
class IsoWidget final : public QWidget {
    Q_OBJECT

public:
    explicit IsoWidget(QWidget* parent = nullptr);

    using QWidget::setGeometry;
    void setGeometry(const DatasetMetadata& metadata);
    void setSlicePositions(double x, double y, double z);
    void setColorPalette(const Palette* palette);

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
    struct Projection {
        double centerX = 0.0;
        double centerY = 0.0;
        double scale = 1.0;
    };

    [[nodiscard]] QPointF project(const Projection& projection,
        double x, double y, double z) const;
    void drawBox(QPainter& painter, const Projection& projection,
        const RealBox& box, const QPen& pen) const;
    void drawSlicePlane(QPainter& painter, const Projection& projection,
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
    const Palette* m_palette = nullptr;
    bool m_hasGeometry = false;

    double m_azimuth = 0.0;
    double m_elevation = 0.0;
    double m_zoom = 1.0;
    QPoint m_lastMousePos;
    bool m_dragging = false;

    QPushButton* m_btnXY = nullptr;
    QPushButton* m_btnXZ = nullptr;
    QPushButton* m_btnYZ = nullptr;
};

} // namespace amrvis::qt
