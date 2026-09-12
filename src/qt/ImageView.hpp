#pragma once

#include "ScaleBar.hpp"

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

#include <cstddef>
#include <optional>
#include <vector>

class QEvent;
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

// A raster and its overlays, placed in the scene. A view shows one tile for
// an ordinary dataset; a companion dataset adds a second, placed beside the
// first in a shared canvas. Every overlay is a child of the tile's pixmap
// item and is expressed in that raster's pixel coordinates, so the item's
// placement is the one transform that maps a tile onto the canvas.
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
    // replacing the raster never moves the scroll position. This installs
    // tile 0 and drops every other tile.
    void setImage(const QImage& image,
        ImageTransformPolicy transformPolicy =
            ImageTransformPolicy::GeometryAware,
        QSize logicalSize = {},
        const std::optional<VirtualPlacement>& placement = std::nullopt);
    // Install or replace one tile. sceneRect is the raster's footprint in
    // scene units (its item is scaled and offset to cover it); canvasRect, when
    // given, becomes the scene rect -- the reach of the scroll bars -- and is
    // kept until setImage or setPlaceholder; otherwise the scene rect is the
    // union of the tiles. Only this tile's item and overlays are replaced.
    // Scene units become display units (a fixed scale N is N screen pixels
    // per scene unit), so callers bake any per-tile stretch into sceneRect.
    void setTileImage(std::size_t tile, const QImage& image,
        const QRectF& sceneRect,
        const std::optional<QRectF>& canvasRect = std::nullopt,
        ImageTransformPolicy transformPolicy = ImageTransformPolicy::Preserve);
    // Move a tile already on display to a new footprint, keeping its raster
    // and overlays; the canvas is set, or cleared, as given. This is how the
    // owner switches between the classic raster-at-origin scene and a shared
    // canvas without re-rendering anything.
    void placeTile(std::size_t tile, const QRectF& sceneRect,
        const std::optional<QRectF>& canvasRect);
    // Hide or show a tile's item and overlays without discarding them.
    void setTileVisible(std::size_t tile, bool visible);
    // Drop a tile's item and overlays. Tile 0 is never removed (setPlaceholder
    // clears it); removing the last extra tile shrinks the tile list.
    void removeTile(std::size_t tile);
    [[nodiscard]] std::size_t tileCount() const noexcept
    {
        return m_tiles.size();
    }
    [[nodiscard]] bool hasTileImage(std::size_t tile) const noexcept;
    [[nodiscard]] bool isTileVisible(std::size_t tile) const noexcept;
    // The tile's footprint in scene units (empty when it has no image).
    [[nodiscard]] QRectF tileSceneRect(std::size_t tile) const;
    // Enter or leave the virtual canvas for the raster already on display,
    // repositioning it without waiting for the next render. Leaving when no
    // virtual canvas is active is a no-op: an explicit canvas from
    // setTileImage is not the virtual one and stays.
    void setVirtualCanvas(const std::optional<VirtualPlacement>& placement);
    // How pixmaps are resampled when the view scales them: nearest-neighbour
    // (the default, one crisp raster pixel per cell) or smooth. A raster
    // drawn at the screen's own resolution is only ever scaled for the
    // moment between a view change and its re-render, and smooth resampling
    // keeps that moment from flickering into a staircase. Applies to every
    // tile on display and to those installed afterwards.
    void setSmoothPixmapTransformation(bool smooth);
    [[nodiscard]] bool virtualCanvasActive() const noexcept
    {
        return m_placement.has_value();
    }
    // The raster item's footprint in scene coordinates (the image rect in the
    // classic raster-at-origin scene; the fetched cell window when virtual).
    // Tile 0; see tileSceneRect for the others.
    [[nodiscard]] QRectF imageSceneRect() const;
    // The visible part of the raster in raster-pixel coordinates, clamped to
    // the raster. Scene coordinates are raster pixels only in the classic
    // scene; on a virtual canvas they are whole-domain finest cells and the
    // item carries both the pixel-to-cell scale and its offset within the
    // domain, so callers that want pixels must come through here rather than
    // intersect a scene rect with the image rect.
    [[nodiscard]] QRectF visibleImageRect(std::size_t tile = 0) const;
    void setGridBoxes(const std::vector<GridBoxOverlay>& boxes,
        std::size_t tile = 0);
    void setOverlaySegments(const std::vector<OverlaySegment>& segments,
        std::size_t tile = 0);
    // Smooth contour polylines, rendered as cosmetic-pen path items at the
    // same z (2) as the overlay segments. Only replaces the path items; the
    // segment items are untouched, so callers that switch overlay kinds must
    // also clear the other setter. Replacing the tile's image drops both.
    void setOverlayPaths(const std::vector<OverlayPath>& paths,
        std::size_t tile = 0);
    void setPointOverlays(const std::vector<PointOverlay>& overlays,
        std::size_t tile = 0);
    // Crosshair guides spanning the whole tile, used by the 3-D slice views
    // to mark where the other two slice planes intersect this one. The lines
    // are in the tile's raster coordinates; a nullopt line hides that guide.
    // They layer at z 1.5, between the grid boxes (z 1) and the overlay
    // segments (z 2).
    void setCrosshairs(const std::optional<QLineF>& vertical,
        const std::optional<QLineF>& horizontal, const QColor& verticalColor,
        const QColor& horizontalColor, std::size_t tile = 0);
    // Small L-shaped axis indicator painted in the lower-left corner of the
    // viewport (not the scene), so it stays fixed regardless of zoom or pan.
    void setAxisIndicator(const QString& horizontal, const QString& vertical);
    // Add a scale bar in the lower-right of the displayed raster. The width is
    // in native plotfile coordinates; an absent unit labels that native value
    // as code units, while an explicit unit permits physical-unit conversion.
    // A non-positive or non-finite width clears the bar.
    void setScaleBarWidth(double widthCodeUnits,
        std::optional<LengthUnit> lengthUnit = std::nullopt);
    [[nodiscard]] bool hasScaleBar() const noexcept
    {
        return m_scaleBarCodeUnitsPerImagePixel > 0.0;
    }
    // Cosmetic red rectangle marking the cell picked in the dataset window;
    // std::nullopt clears it, and replacing the tile's image drops it too. It
    // layers at z 4, above the overlay segments.
    void setCellHighlight(const std::optional<QRectF>& sceneRect,
        std::size_t tile = 0);
    // Spherical companion of setCellHighlight: the picked cell is an annular
    // sector, so mark it with a scene-space path. std::nullopt clears it; both
    // setters share the tile's single highlight item, so calling either
    // replaces the other's.
    void setCellHighlightPath(const std::optional<QPainterPath>& scenePath,
        std::size_t tile = 0);
    void setPlaceholder(const QString& text);
    // The text a placeholder is currently showing, empty once an image
    // replaces it. Tests use this to tell a settled failure state from the
    // "Loading dataset..." one that outlived its load.
    [[nodiscard]] const QString& placeholderText() const noexcept
    {
        return m_placeholderText;
    }
    // Any tile shows a raster.
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
    // Per-axis display stretch, applied in the view transform on top of the
    // isotropic zoom. The raster and the scene are untouched: one raster pixel
    // is still one finest cell and overlays keep their raster coordinates;
    // the screen simply shows each scene unit sx wide and sy tall. Callers
    // normalize so the smaller factor is one, which makes a fixed scale N
    // mean N screen pixels per cell along the less stretched axis. Fit and
    // fixed scale are re-applied; a custom zoom is rescaled about the viewport
    // centre. Non-positive or non-finite factors are treated as one.
    void setDisplayStretch(qreal sx, qreal sy);
    [[nodiscard]] QPointF displayStretch() const noexcept
    {
        return m_stretch;
    }
    // The isotropic part of the view transform: screen pixels per scene unit
    // along the less stretched axis. Equals m11 when the stretch is unity.
    [[nodiscard]] qreal isotropicScale() const;
    // The tiles' on-screen footprint at unit zoom: raster size times the
    // stretch for a single tile, the union of the tiles' scene rects
    // otherwise. This is the aspect an export reproduces.
    [[nodiscard]] QSizeF displaySize() const;
    // Tile 0's raster; image(tile) for the others (null when absent).
    [[nodiscard]] const QImage& image() const noexcept;
    [[nodiscard]] const QImage& image(std::size_t tile) const noexcept;
    [[nodiscard]] std::size_t gridBoxCount() const noexcept;
    // The crosshair guides drawn, over every tile (at most two per tile).
    [[nodiscard]] std::size_t crosshairCount() const noexcept;
    [[nodiscard]] std::size_t pointOverlayCount() const noexcept;
    // The points across those batches: a filter that thins a batch without
    // emptying it leaves pointOverlayCount unchanged.
    [[nodiscard]] std::size_t pointOverlayPointCount() const noexcept;
    [[nodiscard]] const std::vector<QColor>& pointOverlayColors() const noexcept;
    // Renders the scene (tiles plus grid boxes and any other overlays) to a
    // fresh QImage for export. scaleFactor multiplies the raster's native
    // resolution so the export reflects the on-screen zoom (WYSIWYG); an
    // aspect-preserving cap keeps extreme zooms from allocating gigabytes.
    [[nodiscard]] QImage composedImage(qreal scaleFactor = 1.0) const;
    [[nodiscard]] QSize composedImageSize(qreal scaleFactor) const;
    // Axes-enabled exports suppress outer grid strokes, not data pixels.
    [[nodiscard]] QImage composedImage(QSize outputSize, const QFont* exportFont = nullptr,
                                       bool omitOuterGridEdges = false) const;
    void fitToWindow();
    void setFixedScale(int factor);
    void zoomBy(qreal factor);
    // The rect is in image (raster-pixel) coordinates of tile 0 — identical
    // to scene coordinates in the classic scene, offset-corrected on a
    // virtual canvas. confineScene additionally shrinks the scene rect to the
    // target, which keeps the scroll ranges empty: for a zoom whose
    // surroundings are about to be discarded (a rubber-band selection
    // awaiting its re-slice), the domain-spanning scroll bars must not appear
    // even transiently — on a remote session they steal viewport pixels, so
    // the requested raster is undersized and a second fetch fires when they
    // vanish. The next setImage restores the scene rect.
    void zoomToRect(const QRectF& imageRect, bool confineScene = false);
    // The same in scene coordinates, for a selection spanning several tiles.
    void zoomToSceneRect(const QRectF& sceneRect, bool confineScene = false);
    // Confines the scene to `sceneRect` and centres the view on it without
    // touching the transform or its mode: a pan of a zoomed pair, whose
    // window keeps its size and whose scale -- fixed or not -- must stay.
    void showSceneWindow(const QRectF& sceneRect);
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
    // Raster-pixel coordinates of tile 0. The tile* forms name the tile the
    // pointer is over and fire for every tile; these fire for tile 0 only.
    void probeMoved(int x, int y);
    void probeClicked(int x, int y);
    void tileProbeMoved(int tile, int x, int y);
    void tileProbeClicked(int tile, int x, int y);
    // In image (raster-pixel) coordinates of tile 0; see zoomToRect. The
    // scene form carries the same selection in scene coordinates, whichever
    // tiles it spans.
    void rubberBandSelected(const QRectF& imageRect);
    void rubberBandSelectedScene(const QRectF& sceneRect);
    // The viewport scrolled over a virtual canvas — the owner should check
    // whether newly visible cells need fetching.
    void canvasScrolled();
    // What visibleImageRect() returns may have moved: the viewport scrolled or
    // was resized. Separate from canvasScrolled, which is specific to a
    // virtual canvas and fires only over one, and from zoomChanged, which
    // covers a wheel zoom. Between the three, every way the visible part of
    // the raster can change without the raster itself changing is reported.
    void viewportMoved();
    void panDragBegan();
    // Total scene-coordinate offset since the drag began, plus the latest
    // viewport-pixel step (for view-only panning).
    void panDragMoved(const QPointF& totalSceneDelta, const QPoint& viewportDelta);
    // Final total scene-coordinate offset when the drag ends.
    void panDragEnded(const QPointF& totalSceneDelta);
    void linePlotRequested(int imageX, int imageY, Qt::MouseButton button);
    void sliceMoveRequested(int imageX, int imageY, Qt::MouseButton button);
    void tileLinePlotRequested(
        int tile, int imageX, int imageY, Qt::MouseButton button);
    void tileSliceMoveRequested(
        int tile, int imageX, int imageY, Qt::MouseButton button);
    void fitRequested();
    // The view zoomed itself, leaving the display mode at Custom. The owner
    // reports the scale, and a wheel zoom is the one path that changes it
    // without going through the owner first.
    void zoomChanged();
    void viewportResized(const QSize& size);
    // The mapping from scene to screen changed in any way -- a zoom, fit,
    // fixed scale, stretch, scroll or resize -- so a raster drawn for what the
    // viewport showed is now drawn for something else. One signal for the
    // owner that re-renders on demand (the mapped-grid warp), where the
    // three above each cover only their own path. Emitted only when the
    // transform, scroll position or viewport size actually differ from the
    // last emission: replacing a tile on an unchanged canvas emits nothing,
    // so a demand render cannot re-trigger itself.
    void viewChanged();
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
    // Repaints the placeholder in the new skin's colours; it is drawn from
    // palette roles, but a QGraphicsTextItem holds the colour it was given.
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct Tile {
        QGraphicsPixmapItem* item = nullptr;
        QImage image;
        // Raster dimensions at local/native density. Remote Fit rasters may
        // have a different sampled size; fixed scales use this logical size
        // so 1x remains one native output pixel per screen pixel.
        QSize logicalSize;
        QRectF sceneRect;
        bool visible = true;
        // Rect items (Cartesian) or path items (spherical sectors);
        // QGraphicsItem* so both kinds share the list.
        std::vector<QGraphicsItem*> gridItems;
        std::vector<QGraphicsLineItem*> overlayItems;
        std::vector<QGraphicsPathItem*> pathItems;
        std::vector<QGraphicsItem*> pointItems;
        std::vector<QColor> pointOverlayColors;
        std::size_t pointOverlayPointCount = 0;
        std::optional<QLineF> crosshairVertical;
        std::optional<QLineF> crosshairHorizontal;
        QColor crosshairVerticalColor;
        QColor crosshairHorizontalColor;
        QGraphicsLineItem* crosshairVerticalItem = nullptr;
        QGraphicsLineItem* crosshairHorizontalItem = nullptr;
        QGraphicsItem* cellHighlightItem = nullptr;
    };

    // The tile list always holds tile 0; tile(index) grows it on demand.
    [[nodiscard]] Tile& tile(std::size_t index);
    [[nodiscard]] const Tile* tileIfPresent(std::size_t index) const noexcept;
    [[nodiscard]] Tile* tileIfPresent(std::size_t index) noexcept;
    // Drop a tile's item and everything parented to it.
    void clearTileItems(Tile& tile);
    // Place a tile's item over its scene rect and refresh the scene rect.
    void applyTilePlacement(Tile& tile);
    // The tile whose footprint contains the scene point, else tile 0.
    [[nodiscard]] std::size_t tileAt(const QPointF& scenePoint) const;
    // Raster-pixel position within that tile.
    [[nodiscard]] QPointF imagePositionIn(
        std::size_t tile, const QPointF& scenePoint) const;
    // Union of the visible tiles' footprints; the scene rect when no canvas is
    // set, and what Fit and export frame.
    [[nodiscard]] QRectF tilesRect() const;
    // What an export renders: the visible tiles' footprint, cut to a pair
    // canvas confined by a zoom (the other layer's whole tile lies outside
    // the framed window then). A virtual canvas is not a cut: the tile sits
    // at its cell offset and the export follows it.
    [[nodiscard]] QRectF exportSceneRect() const;
    void updateSceneRect();
    void fitImage();
    // Fit a scene rect into the viewport with the current stretch: the
    // isotropic scale is the smaller of the two per-axis ratios, then each
    // axis is multiplied by its stretch. QGraphicsView::fitInView cannot be
    // used because it resets the transform to 1:1 per axis and so flattens
    // any stretch already in force. Mirrors fitInView's 2-pixel margin and
    // final centerOn. Callers pass the tiles' bounding rect, where fitInView
    // fitted the pixmap's alpha mask: a raster with transparent pixels (an
    // alpha-ramp palette, the spherical R-Z warp outside its sector) now
    // fits its whole rect, consistent with fixed scale and export.
    void fitSceneRect(const QRectF& rect);
    void applyFixedScale();
    void applyPlacement();
    // Compare the view's scene-to-screen state with the last emission of
    // viewChanged and emit it when anything differs. Called at the end of
    // every path that can change the transform, the scroll position or the
    // viewport size.
    void noteViewChanged();
    void applyPixmapTransformationMode(Tile& tile);
    void showLineGuide(const QPoint& viewPosition);
    void updateLineGuide(const QPoint& viewPosition);
    void applyCrosshairs(Tile& tile);

    QGraphicsScene* m_scene = nullptr;
    std::vector<Tile> m_tiles;
    QString m_placeholderText;
    std::optional<VirtualPlacement> m_placement;
    // An explicit canvas from setTileImage: the scene rect, independent of
    // where the tiles sit within it. Scene units are display units while set.
    std::optional<QRectF> m_canvasRect;
    QString m_indicatorH;
    QString m_indicatorV;
    double m_scaleBarCodeUnitsPerImagePixel = 0.0;
    std::optional<LengthUnit> m_scaleBarLengthUnit;
    QPoint m_pressPosition;
    QPoint m_lastPanPosition;
    QPointF m_panAccumulated;
    Qt::MouseButton m_lineDragButton = Qt::NoButton;
    QPoint m_linePressPosition;
    bool m_lineDragShiftHeld = false;
    bool m_panActive = false;
    QGraphicsLineItem* m_lineGuide = nullptr;
    std::size_t m_lineGuideTile = 0;
    bool m_sliceMoveEnabled = false;
    bool m_lineToolEnabled = true;
    TransformMode m_transformMode = TransformMode::Fit;
    int m_fixedScaleFactor = 1;
    QPointF m_stretch{1.0, 1.0};
    bool m_smoothPixmaps = false;
    // The scene-to-screen state viewChanged last reported.
    struct ViewSnapshot {
        QTransform transform;
        int horizontalScroll = 0;
        int verticalScroll = 0;
        QSize viewportSize;
        friend bool operator==(const ViewSnapshot&, const ViewSnapshot&) = default;
    };
    ViewSnapshot m_notedView;
};

} // namespace amrvis::qt
