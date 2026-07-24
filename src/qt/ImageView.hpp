#pragma once

#include <QGraphicsView>
#include <QColor>
#include <QImage>
#include <QLineF>
#include <QPainterPath>
#include <QPoint>
#include <QRectF>

#include <optional>
#include <vector>

class QGraphicsLineItem;
class QGraphicsPathItem;
class QGraphicsPixmapItem;
class QGraphicsRectItem;
class QMouseEvent;
class QResizeEvent;
class QWheelEvent;

namespace amrvis::qt {

struct GridBoxOverlay {
    QRectF rectangle;
    QColor color;
};

struct OverlaySegment {
    QLineF line;
    QColor color;
    float width = 1.0F;
};

struct OverlayPath {
    QPainterPath path;
    QColor color;
    float width = 1.0F;
};

class ImageView final : public QGraphicsView {
    Q_OBJECT

public:
    explicit ImageView(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void setGridBoxes(const std::vector<GridBoxOverlay>& boxes);
    void setOverlaySegments(const std::vector<OverlaySegment>& segments);
    // Smooth contour polylines, rendered as cosmetic-pen path items at the
    // same z (2) as the overlay segments. Only replaces the path items; the
    // segment items are untouched, so callers that switch overlay kinds must
    // also clear the other setter. setImage/setPlaceholder drop both.
    void setOverlayPaths(const std::vector<OverlayPath>& paths);
    // Crosshair guides spanning the whole image, used by the 3-D slice views
    // to mark where the other two slice planes intersect this one. The lines
    // are in scene coordinates; a nullopt line hides that guide. They layer
    // at z 1.5, between the grid boxes (z 1) and the overlay segments (z 2).
    void setCrosshairs(const std::optional<QLineF>& vertical,
        const std::optional<QLineF>& horizontal, const QColor& verticalColor,
        const QColor& horizontalColor);
    // Small L-shaped axis indicator painted in the lower-left corner of the
    // viewport (not the scene), so it stays fixed regardless of zoom or pan.
    void setAxisIndicator(const QString& horizontal, const QString& vertical);
    // Cosmetic red rectangle marking the cell picked in the dataset window;
    // std::nullopt clears it, and setImage/setPlaceholder drop it too. It
    // layers at z 4, above the overlay segments.
    void setCellHighlight(const std::optional<QRectF>& sceneRect);
    void setPlaceholder(const QString& text);
    [[nodiscard]] bool hasImage() const noexcept;
    [[nodiscard]] const QImage& image() const noexcept;
    // Renders the scene (base image plus grid boxes and any other overlays)
    // to a fresh QImage for export. scaleFactor multiplies the raster's native
    // resolution so the export reflects the on-screen zoom (WYSIWYG); an
    // aspect-preserving cap keeps extreme zooms from allocating gigabytes.
    [[nodiscard]] QImage composedImage(qreal scaleFactor = 1.0) const;
    void fitToWindow();
    void setFixedScale(int factor);
    void zoomToRect(const QRectF& sceneRect);
    // Shift+left-drag pans the viewport (scroll bars, or the view transform
    // when the scene fits the window). When zoomed into a subregion, the
    // drag shifts the visible data window (see panDrag* signals).
    void panViewport(const QPoint& delta);
    // When enabled (the 3-D slice views): a plain right click emits
    // sliceMoveRequested; a Shift+middle/right click or drag, or a right
    // drag, arms a line-plot request. A plain middle click or drag is a no-op.
    // (Only Shift is honored; Control is not.)
    void setSliceMoveEnabled(bool enabled) noexcept;
    // Highlight (or clear) a coloured border indicating the active panel.
    void setActiveBorder(bool active);
    // Remove any temporary line-plot preview guide from the scene.
    void clearLineGuide();

signals:
    void probeMoved(int x, int y);
    void probeClicked(int x, int y);
    void rubberBandSelected(const QRectF& sceneRect);
    void panDragBegan();
    // Total scene-coordinate offset since the drag began, plus the latest
    // viewport-pixel step (for view-only panning).
    void panDragMoved(const QPointF& totalSceneDelta, const QPoint& viewportDelta);
    // Final total scene-coordinate offset when the drag ends.
    void panDragEnded(const QPointF& totalSceneDelta);
    void linePlotRequested(int imageX, int imageY, Qt::MouseButton button);
    void sliceMoveRequested(int imageX, int imageY, Qt::MouseButton button);
    void fitRequested();

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;

private:
    void fitImage();
    void showLineGuide(const QPoint& viewPosition);
    void updateLineGuide(const QPoint& viewPosition);
    void applyCrosshairs();

    QGraphicsScene* m_scene = nullptr;
    QGraphicsPixmapItem* m_item = nullptr;
    QImage m_image;
    std::vector<QGraphicsRectItem*> m_gridItems;
    std::vector<QGraphicsLineItem*> m_overlayItems;
    std::vector<QGraphicsPathItem*> m_pathItems;
    std::optional<QLineF> m_crosshairVertical;
    std::optional<QLineF> m_crosshairHorizontal;
    QColor m_crosshairVerticalColor;
    QColor m_crosshairHorizontalColor;
    QGraphicsLineItem* m_crosshairVerticalItem = nullptr;
    QGraphicsLineItem* m_crosshairHorizontalItem = nullptr;
    QGraphicsRectItem* m_cellHighlightItem = nullptr;
    QString m_indicatorH;
    QString m_indicatorV;
    QPoint m_pressPosition;
    QPoint m_lastPanPosition;
    QPointF m_panAccumulated;
    Qt::MouseButton m_lineDragButton = Qt::NoButton;
    QPoint m_linePressPosition;
    bool m_lineDragShiftHeld = false;
    bool m_panActive = false;
    QGraphicsLineItem* m_lineGuide = nullptr;
    bool m_sliceMoveEnabled = false;
    bool m_fitOnResize = true;
};

} // namespace amrvis::qt
