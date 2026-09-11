#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/OrthoProjection.hpp>

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QWidget>

#include <array>
#include <functional>
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
    // The map takes a dataset index and a physical point to the display
    // coordinates the wireframe is drawn in, so the view keeps the panels'
    // proportions rather than the physical ones; empty draws physical
    // coordinates.
    using DisplayMap = std::function<Real3(std::size_t dataset, const Real3& point)>;
    // One dataset, drawn through the map when one is given (the main window
    // passes its Axis Scaling factors; the volume window passes none so the
    // wireframe lines up with the ray-cast backdrop).
    void setGeometry(const DatasetMetadata& metadata, DisplayMap displayMap = {});
    // Two datasets sharing a plane: the outline and the projection span the
    // union of their domains, each domain is outlined on its own, and both
    // level box sets are drawn.
    void setPairedGeometry(const DatasetMetadata& primary,
        const DatasetMetadata& companion, DisplayMap displayMap);
    // The outlined domain in display coordinates (the union for two datasets).
    [[nodiscard]] const RealBox& displayDomain() const noexcept { return m_domain; }
    void setSlicePositions(double x, double y, double z);
    void setSlicePlanesVisible(bool visible);
    void setColorPalette(const Palette* palette);

    // The camera the widget draws with; drag, wheel and the preset buttons
    // change it and emit cameraChanged, a release after a drag emits
    // interactionEnded.
    [[nodiscard]] const OrthoCamera& camera() const noexcept { return m_camera; }
    void setCamera(const OrthoCamera& camera);

    // A rendered volume frame drawn under the wireframe, with the camera it
    // was rendered with: a premultiplied image produced at some viewport size
    // and some zoom. It is drawn about the viewport centre at backdropScale of
    // the two, so a frame rendered at another size (a half-size draft while
    // the camera moves) or another zoom (a wheel notch not yet re-rendered)
    // still lines up with the wireframe drawn over it -- the orientation
    // cannot be corrected that way, so a rotation still shows a stale frame
    // until the next one lands. A null image draws nothing -- the main
    // window's quadrant.
    void setBackdropImage(QImage image, const OrthoCamera& camera);
    // Overlay toggles for the volume view; the quadrant keeps both on.
    void setLevelBoxesVisible(bool visible);
    void setDomainOutlineVisible(bool visible);
    [[nodiscard]] bool levelBoxesVisible() const noexcept
    {
        return m_levelBoxesVisible;
    }
    [[nodiscard]] bool domainOutlineVisible() const noexcept
    {
        return m_domainOutlineVisible;
    }

signals:
    void cameraChanged();
    void interactionEnded();
    // The viewport changed size, so a backdrop rendered for the old one is
    // now being stretched: whoever renders it wants to render it again.
    void viewResized();
    // The display's scale changed -- the window was moved to a screen with a
    // different one. Separate from viewResized because no resize follows: the
    // logical size is unchanged, so a renderer that compares logical sizes to
    // spot layout churn would dismiss this as churn. Every device pixel the
    // view is made of has changed size, and a frame rendered for the old ratio
    // is now the wrong resolution.
    void viewScaleChanged();

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct LevelBoxes {
        int level = 0;
        std::size_t dataset = 0;
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

    void setGeometries(const std::vector<const DatasetMetadata*>& metadata,
        DisplayMap displayMap);
    // A dataset's physical point in the coordinates the wireframe uses:
    // the display map's when one is set, physical otherwise.
    [[nodiscard]] Real3 toDisplay(std::size_t dataset, const Real3& point) const;
    [[nodiscard]] RealBox toDisplay(std::size_t dataset, const RealBox& box) const;
    [[nodiscard]] std::size_t datasetHolding(int axis, double position) const;

    // In display coordinates (see toDisplay).
    RealBox m_domain{};
    // Each dataset's own domain when several share the widget; empty for one.
    std::vector<RealBox> m_datasetDomains;
    // The same in physical coordinates, to place the slice positions.
    std::vector<RealBox> m_physicalDomains;
    DisplayMap m_displayMap;
    std::vector<LevelBoxes> m_levels;
    std::array<double, 3> m_slicePositions{0.0, 0.0, 0.0};
    bool m_slicePlanesVisible = false;
    bool m_levelBoxesVisible = true;
    bool m_domainOutlineVisible = true;
    QImage m_backdrop;
    OrthoCamera m_backdropCamera;
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
