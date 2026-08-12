#pragma once

#include <amrexplorer/pipeline/ImageTransformPolicy.hpp>

#include <QGraphicsView>
#include <QColor>
#include <QImage>
#include <QLineF>
#include <QPainterPath>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QSize>

#include <optional>
#include <vector>

class QGraphicsLineItem;
class QGraphicsItem;
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
    // When non-empty (2-D spherical), the box is an annular sector: draw this
    // scene-space path instead of the axis-aligned rectangle above.
    QPainterPath path;
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

struct PointOverlay {
    std::vector<QPointF> points;
    QColor color;
    float size = 3.0F;
};

// ImageTransformPolicy lives in the Qt-free pipeline layer (the
// DisplayCoordinator decides it from pure request data); re-export it here
// so amrvis::qt::ImageTransformPolicy keeps resolving for the GUI.
using amrvis::ImageTransformPolicy;

class ImageView final : public QGraphicsView {
    Q_OBJECT

public:
    enum class TransformMode {
        Fit,
        FixedScale,
        Custom
    };

    // Virtual-canvas placement for the demand-driven remote fixed scale: the
    // scene spans the whole dataset domain in finest-cell units while the
    // pixmap item covers only the fetched window at its cell offset. The
    // scroll bars then represent the full domain — the same look and reach a
    // local whole-domain raster gives — and scrolling emits canvasScrolled so
    // the owner can fetch the newly visible window.
    struct VirtualPlacement {
        QRectF itemCells;   // fetched window in finest cells within the domain
        QSizeF domainCells; // whole domain in finest cells
    };

    explicit ImageView(QWidget* parent = nullptr);

    // Preserve keeps the current panel-local transform when replacing a raster
    // after rubber-band zoom or pan. GeometryAware refits only when the raster
    // dimensions change. Refit discards the transform even for equal-size
    // rasters whose data regions are incompatible. A placement keeps the
    // virtual canvas: the scene rect and view transform are left untouched so
    // replacing the raster never moves the scroll position.
    void setImage(const QImage& image,
        ImageTransformPolicy transformPolicy =
            ImageTransformPolicy::GeometryAware,
        QSize logicalSize = {},
        const std::optional<VirtualPlacement>& placement = std::nullopt);
    // Enter or leave the virtual canvas for the raster already on display,
    // repositioning it without waiting for the next render.
    void setVirtualCanvas(const std::optional<VirtualPlacement>& placement);
    [[nodiscard]] bool virtualCanvasActive() const noexcept
    {
        return m_placement.has_value();
    }
    // The raster item's footprint in scene coordinates (the image rect in the
    // classic raster-at-origin scene; the fetched cell window when virtual).
    [[nodiscard]] QRectF imageSceneRect() const;
    // The visible part of the raster in raster-pixel coordinates, clamped to
    // the raster. Scene coordinates are raster pixels only in the classic
    // scene; on a virtual canvas they are whole-domain finest cells and the
    // item carries both the pixel-to-cell scale and its offset within the
    // domain, so callers that want pixels must come through here rather than
    // intersect a scene rect with the image rect.
    [[nodiscard]] QRectF visibleImageRect() const;
    void setGridBoxes(const std::vector<GridBoxOverlay>& boxes);
    void setOverlaySegments(const std::vector<OverlaySegment>& segments);
    // Smooth contour polylines, rendered as cosmetic-pen path items at the
    // same z (2) as the overlay segments. Only replaces the path items; the
    // segment items are untouched, so callers that switch overlay kinds must
    // also clear the other setter. setImage/setPlaceholder drop both.
    void setOverlayPaths(const std::vector<OverlayPath>& paths);
    void setPointOverlays(const std::vector<PointOverlay>& overlays);
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
    // Spherical companion of setCellHighlight: the picked cell is an annular
    // sector, so mark it with a scene-space path. std::nullopt clears it; both
    // setters share the single highlight item, so calling either replaces the
    // other's.
    void setCellHighlightPath(const std::optional<QPainterPath>& scenePath);
    void setPlaceholder(const QString& text);
    // The text a placeholder is currently showing, empty once an image
    // replaces it. Tests use this to tell a settled failure state from the
    // "Loading dataset..." one that outlived its load.
    [[nodiscard]] const QString& placeholderText() const noexcept
    {
        return m_placeholderText;
    }
    [[nodiscard]] bool hasImage() const noexcept;
    // Fit, fixed integer scale, and custom zoom/pan are durable display modes,
    // not incidental properties of the current QTransform.
    [[nodiscard]] TransformMode transformMode() const noexcept
    {
        return m_transformMode;
    }
    [[nodiscard]] bool isFitToWindow() const noexcept
    {
        return m_transformMode == TransformMode::Fit;
    }
    [[nodiscard]] int fixedScaleFactor() const noexcept
    {
        return m_fixedScaleFactor;
    }
    [[nodiscard]] const QImage& image() const noexcept;
    [[nodiscard]] std::size_t gridBoxCount() const noexcept
    {
        return m_gridItems.size();
    }
    [[nodiscard]] std::size_t pointOverlayCount() const noexcept;
    [[nodiscard]] const std::vector<QColor>& pointOverlayColors() const noexcept;
    // Renders the scene (base image plus grid boxes and any other overlays)
    // to a fresh QImage for export. scaleFactor multiplies the raster's native
    // resolution so the export reflects the on-screen zoom (WYSIWYG); an
    // aspect-preserving cap keeps extreme zooms from allocating gigabytes.
    [[nodiscard]] QImage composedImage(qreal scaleFactor = 1.0) const;
    void fitToWindow();
    void setFixedScale(int factor);
    void zoomBy(qreal factor);
    // The rect is in image (raster-pixel) coordinates — identical to scene
    // coordinates in the classic scene, offset-corrected on a virtual canvas.
    void zoomToRect(const QRectF& imageRect);
    // Scrolls the viewport for Shift+left-drag and the arrow keys. The delta
    // is how far the content moves, matching the sense MainWindow's
    // shiftedPanRegion uses, and is a no-op when the scene already fits the
    // window. When zoomed into a subregion, the drag shifts the visible data
    // window instead (see panDrag* signals).
    void panViewport(const QPoint& delta);
    // When enabled (the 3-D slice views): a plain right click emits
    // sliceMoveRequested; a Shift+middle/right click or drag, or a right
    // drag, arms a line-plot request. A plain middle click or drag is a no-op.
    // (Only Shift is honored; Control is not.)
    void setSliceMoveEnabled(bool enabled) noexcept;
    // Disables the line-plot drag and its preview guide, used for 2-D spherical
    // display where the straight-line profile tool is not yet meaningful.
    // Probing and rubber-band zoom still work.
    void setLineToolEnabled(bool enabled) noexcept;
    // Highlight (or clear) a coloured border indicating the active panel.
    void setActiveBorder(bool active);
    // Remove any temporary line-plot preview guide from the scene.
    void clearLineGuide();

signals:
    void probeMoved(int x, int y);
    void probeClicked(int x, int y);
    // In image (raster-pixel) coordinates; see zoomToRect.
    void rubberBandSelected(const QRectF& imageRect);
    // The viewport scrolled over a virtual canvas — the owner should check
    // whether newly visible cells need fetching.
    void canvasScrolled();
    void panDragBegan();
    // Total scene-coordinate offset since the drag began, plus the latest
    // viewport-pixel step (for view-only panning).
    void panDragMoved(const QPointF& totalSceneDelta, const QPoint& viewportDelta);
    // Final total scene-coordinate offset when the drag ends.
    void panDragEnded(const QPointF& totalSceneDelta);
    void linePlotRequested(int imageX, int imageY, Qt::MouseButton button);
    void sliceMoveRequested(int imageX, int imageY, Qt::MouseButton button);
    void fitRequested();
    // The view zoomed itself, leaving the display mode at Custom. The owner
    // reports the scale, and a wheel zoom is the one path that changes it
    // without going through the owner first.
    void zoomChanged();
    void viewportResized(const QSize& size);
    // An arrow key pressed while this view has focus, as a unit direction in
    // pan terms (+x scrolls the data right, +y scrolls it up). Panning is a
    // view action, so it belongs to the focused view rather than to the window:
    // a window-wide shortcut takes Up/Down away from every spin box and combo
    // in the toolbars, which do not claim those keys through ShortcutOverride.
    void panStepRequested(const QPointF& direction);

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    void fitImage();
    void applyFixedScale();
    void applyPlacement();
    void showLineGuide(const QPoint& viewPosition);
    void updateLineGuide(const QPoint& viewPosition);
    void applyCrosshairs();

    QGraphicsScene* m_scene = nullptr;
    QGraphicsPixmapItem* m_item = nullptr;
    QImage m_image;
    QString m_placeholderText;
    // Raster dimensions at local/native density. Remote Fit rasters may have
    // a different sampled size; fixed scales use this logical size so 1x
    // remains one native output pixel per screen pixel.
    QSize m_logicalSize;
    std::optional<VirtualPlacement> m_placement;
    // Rect items (Cartesian) or path items (spherical sectors); QGraphicsItem*
    // so both kinds share the list.
    std::vector<QGraphicsItem*> m_gridItems;
    std::vector<QGraphicsLineItem*> m_overlayItems;
    std::vector<QGraphicsPathItem*> m_pathItems;
    std::vector<QGraphicsItem*> m_pointItems;
    std::vector<QColor> m_pointOverlayColors;
    std::optional<QLineF> m_crosshairVertical;
    std::optional<QLineF> m_crosshairHorizontal;
    QColor m_crosshairVerticalColor;
    QColor m_crosshairHorizontalColor;
    QGraphicsLineItem* m_crosshairVerticalItem = nullptr;
    QGraphicsLineItem* m_crosshairHorizontalItem = nullptr;
    QGraphicsItem* m_cellHighlightItem = nullptr;
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
    bool m_lineToolEnabled = true;
    TransformMode m_transformMode = TransformMode::Fit;
    int m_fixedScaleFactor = 1;
};

} // namespace amrvis::qt
