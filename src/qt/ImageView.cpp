#include "ImageView.hpp"
#include "ScaleBar.hpp"

#include "Theme.hpp"

#include <QEvent>
#include <QGraphicsLineItem>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QPen>

#include <algorithm>
#include <array>
#include <cmath>

namespace amrvis::qt {
namespace {

// Grid box outlines drawn crisp (anti-aliasing off), independently of the
// image's smooth scaling. Without this, adjacent boxes that share an edge
// draw their 1px outlines twice and the anti-alias fringe darkens on the
// shared interior lines, so a single level's boxes look like several colors.
// Axis-aligned 1px lines stay clean with anti-aliasing off.
class CrispRectItem : public QGraphicsRectItem {
public:
    using QGraphicsRectItem::QGraphicsRectItem;
    bool omitOuterEdges = false;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
        QWidget* widget = nullptr) override
    {
        painter->save();
        if (omitOuterEdges && parentItem() != nullptr) {
            // A cosmetic grid stroke on the outer data boundary paints the
            // first pixel row/column white, looking like a gap beside export
            // axes. Clip only grid ink there, not the raster or other overlays.
            const auto transform = painter->worldTransform();
            const QRectF deviceBounds = transform.mapRect(parentItem()->boundingRect());
            painter->setClipRect(
                transform.inverted().mapRect(deviceBounds.adjusted(1.0, 1.0, -1.0, -1.0)),
                Qt::IntersectClip);
        }
        const auto antialiasing = painter->testRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::Antialiasing, false);
        QGraphicsRectItem::paint(painter, option, widget);
        painter->setRenderHint(QPainter::Antialiasing, antialiasing);
        painter->restore();
    }
};

class PointCloudItem final : public QGraphicsItem {
public:
    PointCloudItem(std::vector<QPointF> points, const QRectF& bounds,
        QColor color, qreal size)
        : m_points(std::move(points))
        , m_bounds(bounds)
        , m_color(std::move(color))
        , m_size(size)
    {
    }

    [[nodiscard]] QRectF boundingRect() const override
    {
        // The pen is cosmetic, so its width is m_size *device* pixels however
        // far the view is zoomed out; in item coordinates that is m_size/scale,
        // which exceeds a bare m_size padding as soon as the scale drops below
        // one, and edge points then paint outside the declared bounds. Nothing
        // shows today because the view repaints its whole viewport, but the
        // contract holds regardless of who is asking.
        //
        // A constant multiple rather than the live view scale: boundingRect
        // must not change without prepareGeometryChange, or the scene's item
        // index goes stale, and the view scale changes on every zoom. This
        // covers zoom-out to 1/16, past which the whole raster occupies a
        // handful of pixels and a point's overspill is not a visible artifact.
        constexpr qreal smallestCoveredScale = 16.0;
        const auto padding = m_size * smallestCoveredScale;
        return m_bounds.adjusted(-padding, -padding, padding, padding);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        QPen pen(m_color);
        pen.setCosmetic(true);
        pen.setWidthF(m_size);
        pen.setCapStyle(Qt::RoundCap);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(pen);
        painter->drawPoints(m_points.data(), static_cast<int>(m_points.size()));
        painter->restore();
    }

private:
    std::vector<QPointF> m_points;
    QRectF m_bounds;
    QColor m_color;
    qreal m_size = 3.0;
};

void paintScaleBar(QPainter* painter, const QRectF& imageBounds, double codeUnitsPerImagePixel,
                   double pixelsPerImagePixel, std::optional<LengthUnit> lengthUnit,
                   const QFont* exportFont = nullptr) {
    if (!(codeUnitsPerImagePixel > 0.0) || imageBounds.width() < 40.0
        || imageBounds.height() < 36.0 || !(pixelsPerImagePixel > 0.0)
        || !std::isfinite(pixelsPerImagePixel)) {
        return;
    }

    const double codeUnitsPerOutputPixel
        = codeUnitsPerImagePixel / pixelsPerImagePixel;
    const double maximumPixels = imageBounds.width() * 0.25;
    const auto bar = chooseScaleBar(
        imageBounds.width() * codeUnitsPerOutputPixel,
        maximumPixels * codeUnitsPerOutputPixel, maximumPixels, lengthUnit);
    if (!bar) {
        return;
    }

    const double annotationScale = exportFont != nullptr ? exportFont->pixelSize() / 14.0 : 1.0;
    const double inset = 12.0 * annotationScale;
    const double tickHalfHeight = 4.0 * annotationScale;
    const double right = imageBounds.right() - inset;
    const double left = right - bar->lengthPixels;
    const double y = imageBounds.bottom() - inset;
    const QLineF horizontal(left, y, right, y);
    const QLineF leftTick(
        left, y - tickHalfHeight, left, y + tickHalfHeight);
    const QLineF rightTick(
        right, y - tickHalfHeight, right, y + tickHalfHeight);
    const QColor foreground(255, 255, 255, 230);
    const QColor outline(0, 0, 0, 190);

    painter->save();
    painter->resetTransform();
    painter->setRenderHint(QPainter::Antialiasing);
    QPen halo(outline);
    halo.setWidthF(5.0 * annotationScale);
    halo.setCapStyle(Qt::FlatCap);
    painter->setPen(halo);
    painter->drawLines({horizontal, leftTick, rightTick});
    QPen line(foreground);
    line.setWidthF(2.0 * annotationScale);
    line.setCapStyle(Qt::FlatCap);
    painter->setPen(line);
    painter->drawLines({horizontal, leftTick, rightTick});

    QFont font = exportFont != nullptr ? *exportFont : painter->font();
    if (exportFont == nullptr) {
        font.setPointSize(10);
    }
    font.setBold(true);
    painter->setFont(font);
    const QString label = QString::fromStdString(bar->label);
    const auto fm = painter->fontMetrics();
    const QRectF textRect(left - 20.0,
        y - tickHalfHeight - fm.height() - 2.0,
        bar->lengthPixels + 40.0, fm.height());
    painter->setPen(outline);
    painter->drawText(textRect.translated(1.0, 1.0),
        Qt::AlignHCenter | Qt::AlignVCenter, label);
    painter->setPen(foreground);
    painter->drawText(textRect,
        Qt::AlignHCenter | Qt::AlignVCenter, label);
    painter->restore();
}

// Deletes a tile-owned item and forgets it; children go with their parent.
template <typename Item>
void deleteItems(QGraphicsScene* scene, std::vector<Item*>& items)
{
    for (auto* item : items) {
        scene->removeItem(item);
        delete item;
    }
    items.clear();
}

} // namespace

ImageView::ImageView(QWidget* parent)
    : QGraphicsView(parent)
    , m_scene(new QGraphicsScene(this))
    , m_tiles(1)
{
    setScene(m_scene);
    setAlignment(Qt::AlignCenter);
    setBackgroundBrush(palette().window());
    setDragMode(QGraphicsView::RubberBandDrag);
    setMouseTracking(true);
    setRenderHint(QPainter::Antialiasing);
    setRenderHint(QPainter::SmoothPixmapTransform);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    // The axis indicator is painted in drawForeground with resetTransform,
    // outside the scene. FullViewportUpdate ensures it never ghosts when
    // the viewport scrolls partial-update.
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
}

ImageView::Tile& ImageView::tile(std::size_t index)
{
    if (index >= m_tiles.size()) {
        m_tiles.resize(index + 1);
    }
    return m_tiles[index];
}

const ImageView::Tile* ImageView::tileIfPresent(std::size_t index) const noexcept
{
    return index < m_tiles.size() ? &m_tiles[index] : nullptr;
}

ImageView::Tile* ImageView::tileIfPresent(std::size_t index) noexcept
{
    return index < m_tiles.size() ? &m_tiles[index] : nullptr;
}

void ImageView::clearTileItems(Tile& tile)
{
    // Deleting the pixmap item takes every child overlay with it; the lists
    // are then dangling and are dropped rather than deleted again.
    if (m_lineGuide != nullptr && m_lineGuide->parentItem() == tile.item) {
        m_lineGuide = nullptr;
    }
    if (tile.item != nullptr) {
        m_scene->removeItem(tile.item);
        delete tile.item;
        tile.item = nullptr;
    }
    tile.gridItems.clear();
    tile.overlayItems.clear();
    tile.pathItems.clear();
    tile.pointItems.clear();
    tile.pointOverlayColors.clear();
    tile.pointOverlayPointCount = 0;
    tile.crosshairVerticalItem = nullptr;
    tile.crosshairHorizontalItem = nullptr;
    tile.cellHighlightItem = nullptr;
}

void ImageView::applyTilePlacement(Tile& tile)
{
    if (tile.item == nullptr) {
        updateSceneRect();
        return;
    }
    const QRectF imageRect(QPointF(0.0, 0.0), QSizeF(tile.image.size()));
    if (tile.sceneRect == imageRect) {
        // The classic raster-at-origin scene: an identity, exactly.
        tile.item->setTransform(QTransform());
        tile.item->setPos(QPointF());
    } else {
        QTransform placement;
        if (tile.image.width() > 0 && tile.image.height() > 0) {
            placement.scale(tile.sceneRect.width() / tile.image.width(),
                tile.sceneRect.height() / tile.image.height());
        }
        tile.item->setTransform(placement);
        tile.item->setPos(tile.sceneRect.topLeft());
    }
    tile.item->setVisible(tile.visible);
    updateSceneRect();
}

QRectF ImageView::tilesRect() const
{
    QRectF union_;
    for (const auto& tile : m_tiles) {
        if (tile.item != nullptr && tile.visible) {
            union_ = union_.isNull() ? tile.sceneRect : union_.united(tile.sceneRect);
        }
    }
    return union_;
}

QRectF ImageView::exportSceneRect() const
{
    const auto tiles = tilesRect();
    if (m_canvasRect.has_value() && !m_placement.has_value()) {
        // A framed window with no tile under it (the layer on show lies
        // elsewhere) exports as the empty window, not as the tile outside it.
        const auto cut = tiles.intersected(*m_canvasRect);
        return cut.isEmpty() ? *m_canvasRect : cut;
    }
    return tiles;
}

void ImageView::updateSceneRect()
{
    m_scene->setSceneRect(m_canvasRect.value_or(tilesRect()));
}

std::size_t ImageView::tileAt(const QPointF& scenePoint) const
{
    for (std::size_t index = m_tiles.size(); index-- > 0;) {
        const auto& tile = m_tiles[index];
        if (tile.item != nullptr && tile.visible
            && tile.sceneRect.contains(scenePoint)) {
            return index;
        }
    }
    // Outside every tile: the first one on show, never a hidden raster.
    for (std::size_t index = 0; index < m_tiles.size(); ++index) {
        if (m_tiles[index].item != nullptr && m_tiles[index].visible) {
            return index;
        }
    }
    return 0;
}

QPointF ImageView::imagePositionIn(
    std::size_t index, const QPointF& scenePoint) const
{
    const auto* tile = tileIfPresent(index);
    return tile != nullptr && tile->item != nullptr
        ? tile->item->mapFromScene(scenePoint) : scenePoint;
}

void ImageView::setImage(
    const QImage& image, ImageTransformPolicy transformPolicy,
    QSize logicalSize, const std::optional<VirtualPlacement>& placement)
{
    // One tile again: every other tile goes with the scene.
    m_scene->clear();
    m_lineGuide = nullptr;
    m_lineDragButton = Qt::NoButton;
    auto& first = m_tiles.front();
    const auto crosshairVertical = first.crosshairVertical;
    const auto crosshairHorizontal = first.crosshairHorizontal;
    const auto verticalColor = first.crosshairVerticalColor;
    const auto horizontalColor = first.crosshairHorizontalColor;
    const bool sizeChanged = first.image.isNull() || first.image.size() != image.size();
    m_tiles.assign(1, Tile{});
    auto& tile0 = m_tiles.front();
    tile0.crosshairVertical = crosshairVertical;
    tile0.crosshairHorizontal = crosshairHorizontal;
    tile0.crosshairVerticalColor = verticalColor;
    tile0.crosshairHorizontalColor = horizontalColor;
    m_canvasRect.reset();
    // An explicitly incompatible raster resets Custom mode. Fixed scale is a
    // user-selected persistent mode and therefore survives every replacement,
    // including a delayed result whose policy was computed before the scale
    // selection. Fit likewise remains a mode and refits every replacement.
    if (transformPolicy == ImageTransformPolicy::Refit
        && m_transformMode == TransformMode::Custom) {
        m_transformMode = TransformMode::Fit;
    }
    tile0.image = image;
    m_placeholderText.clear();
    tile0.logicalSize = logicalSize.isValid() ? logicalSize : image.size();
    tile0.item = m_scene->addPixmap(QPixmap::fromImage(tile0.image));
    applyPixmapTransformationMode(tile0);
    m_placement = placement;
    applyPlacement();
    setBackgroundBrush(viewportBackground());
    if (m_transformMode == TransformMode::Fit) {
        fitImage();
    } else if (m_transformMode == TransformMode::FixedScale) {
        applyFixedScale();
    } else if (transformPolicy == ImageTransformPolicy::GeometryAware
        && sizeChanged && !m_placement.has_value()) {
        m_transformMode = TransformMode::Fit;
        fitImage();
    }
    applyCrosshairs(tile0);
}

void ImageView::setTileImage(std::size_t index, const QImage& image,
    const QRectF& sceneRect, const std::optional<QRectF>& canvasRect,
    ImageTransformPolicy transformPolicy)
{
    auto& tile = this->tile(index);
    const bool sizeChanged = tile.image.isNull() || tile.image.size() != image.size();
    const bool wasEmpty = !hasImage();
    clearTileItems(tile);
    if (transformPolicy == ImageTransformPolicy::Refit
        && m_transformMode == TransformMode::Custom) {
        m_transformMode = TransformMode::Fit;
    }
    tile.image = image;
    tile.logicalSize = image.size();
    tile.sceneRect = sceneRect;
    tile.visible = true;
    tile.item = m_scene->addPixmap(QPixmap::fromImage(tile.image));
    applyPixmapTransformationMode(tile);
    if (canvasRect.has_value()) {
        m_canvasRect = canvasRect;
    }
    if (!m_placeholderText.isEmpty()) {
        // A placeholder text item is still in the scene from setPlaceholder.
        for (auto* item : m_scene->items()) {
            if (item->parentItem() == nullptr
                && dynamic_cast<QGraphicsPixmapItem*>(item) == nullptr) {
                m_scene->removeItem(item);
                delete item;
            }
        }
        m_placeholderText.clear();
    }
    applyTilePlacement(tile);
    setBackgroundBrush(viewportBackground());
    if (m_transformMode == TransformMode::Fit) {
        fitImage();
    } else if (m_transformMode == TransformMode::FixedScale) {
        applyFixedScale();
    } else if (transformPolicy == ImageTransformPolicy::GeometryAware
        && (sizeChanged || wasEmpty)) {
        m_transformMode = TransformMode::Fit;
        fitImage();
    }
    applyCrosshairs(tile);
}

void ImageView::placeTile(std::size_t index, const QRectF& sceneRect,
    const std::optional<QRectF>& canvasRect)
{
    auto* tile = index < m_tiles.size() ? &m_tiles[index] : nullptr;
    if (tile == nullptr || tile->item == nullptr) {
        return;
    }
    tile->sceneRect = sceneRect;
    m_canvasRect = canvasRect;
    applyTilePlacement(*tile);
    if (m_transformMode == TransformMode::Fit) {
        fitImage();
    } else if (m_transformMode == TransformMode::FixedScale) {
        applyFixedScale();
    }
}

void ImageView::setTileVisible(std::size_t index, bool visible)
{
    auto* tile = index < m_tiles.size() ? &m_tiles[index] : nullptr;
    if (tile == nullptr || tile->visible == visible) {
        return;
    }
    tile->visible = visible;
    if (tile->item != nullptr) {
        tile->item->setVisible(visible);
    }
    updateSceneRect();
}

void ImageView::removeTile(std::size_t index)
{
    if (index == 0 || index >= m_tiles.size()) {
        return;
    }
    clearTileItems(m_tiles[index]);
    m_tiles[index] = Tile{};
    while (m_tiles.size() > 1 && m_tiles.back().item == nullptr) {
        m_tiles.pop_back();
    }
    updateSceneRect();
}

bool ImageView::hasTileImage(std::size_t index) const noexcept
{
    const auto* tile = tileIfPresent(index);
    return tile != nullptr && tile->item != nullptr && !tile->image.isNull();
}

bool ImageView::isTileVisible(std::size_t index) const noexcept
{
    const auto* tile = tileIfPresent(index);
    return tile != nullptr && tile->item != nullptr && tile->visible;
}

QRectF ImageView::tileSceneRect(std::size_t index) const
{
    return hasTileImage(index) ? m_tiles[index].sceneRect : QRectF();
}

void ImageView::setVirtualCanvas(
    const std::optional<VirtualPlacement>& placement)
{
    if (!hasImage()) {
        m_placement.reset();
        return;
    }
    if (!placement.has_value() && !m_placement.has_value()) {
        // Nothing to leave. applyPlacement would also drop an explicit
        // canvas from setTileImage and snap its tile back to the origin,
        // which is not what "no virtual canvas" asks for.
        return;
    }
    m_placement = placement;
    applyPlacement();
    if (m_transformMode == TransformMode::FixedScale) {
        applyFixedScale();
    }
    noteViewChanged();
}

void ImageView::setSmoothPixmapTransformation(bool smooth)
{
    if (m_smoothPixmaps == smooth) {
        return;
    }
    m_smoothPixmaps = smooth;
    for (auto& tile : m_tiles) {
        applyPixmapTransformationMode(tile);
    }
}

void ImageView::applyPixmapTransformationMode(Tile& tile)
{
    if (tile.item != nullptr) {
        tile.item->setTransformationMode(
            m_smoothPixmaps ? Qt::SmoothTransformation : Qt::FastTransformation);
    }
}

void ImageView::noteViewChanged()
{
    ViewSnapshot now;
    now.transform = transform();
    now.horizontalScroll = horizontalScrollBar()->value();
    now.verticalScroll = verticalScrollBar()->value();
    now.viewportSize = viewport() != nullptr ? viewport()->size() : QSize();
    if (now == m_notedView) {
        return;
    }
    m_notedView = now;
    emit viewChanged();
}

QRectF ImageView::imageSceneRect() const
{
    const auto& tile0 = m_tiles.front();
    return tile0.item == nullptr ? QRectF() : tile0.item->sceneBoundingRect();
}

QRectF ImageView::visibleImageRect(std::size_t index) const
{
    const auto* tile = tileIfPresent(index);
    if (tile == nullptr || tile->item == nullptr || viewport() == nullptr) {
        return {};
    }
    // mapRectFromScene undoes the item's placement -- identity in the classic
    // scene, the cell offset and pixel-to-cell scale on a virtual canvas -- so
    // the result is in raster pixels either way.
    return tile->item->mapRectFromScene(
        mapToScene(viewport()->rect()).boundingRect()).intersected(
            tile->item->boundingRect());
}

// Position tile 0 for the virtual canvas. On a virtual canvas the scene
// spans the whole domain in finest-cell units and the item maps its pixels
// onto its fetched cell window; the classic scene is the raster at the
// origin. Neither the view transform nor the scroll bars are touched here, so
// a raster replacement on an unchanged canvas never moves the view.
void ImageView::applyPlacement()
{
    auto& tile0 = m_tiles.front();
    if (m_placement.has_value()) {
        tile0.sceneRect = m_placement->itemCells;
        m_canvasRect = QRectF(QPointF(0.0, 0.0), m_placement->domainCells);
    } else {
        tile0.sceneRect = QRectF(QPointF(0.0, 0.0), QSizeF(tile0.image.size()));
        m_canvasRect.reset();
    }
    applyTilePlacement(tile0);
}

void ImageView::setGridBoxes(const std::vector<GridBoxOverlay>& boxes,
    std::size_t index)
{
    // A tile that was never placed has nothing to clear and nothing to draw
    // on; looking it up must not grow the tile list (see displaySize).
    auto* tile = tileIfPresent(index);
    if (tile == nullptr) {
        return;
    }
    deleteItems(m_scene, tile->gridItems);
    if (tile->item == nullptr) {
        return;
    }
    tile->gridItems.reserve(boxes.size());
    // All overlays are children of the raster item: their raster-pixel
    // coordinates then hold on a virtual canvas, where the item carries the
    // pixel-to-cell transform and its offset within the domain.
    for (const auto& box : boxes) {
        QPen pen(box.color);
        pen.setCosmetic(true);
        pen.setWidth(1);
        if (!box.path.isEmpty()) {
            // Spherical: a curved annular sector. Keep anti-aliasing on (the
            // view default) so the arcs stay smooth; the crisp-rect trick only
            // helps axis-aligned edges.
            auto* item = m_scene->addPath(box.path, pen);
            item->setParentItem(tile->item);
            item->setBrush(Qt::NoBrush);
            item->setZValue(1.0);
            tile->gridItems.push_back(item);
            continue;
        }
        auto* item = new CrispRectItem(box.rectangle, tile->item);
        item->setPen(pen);
        item->setBrush(Qt::NoBrush);
        item->setZValue(1.0);
        tile->gridItems.push_back(item);
    }
}

void ImageView::setOverlaySegments(const std::vector<OverlaySegment>& segments,
    std::size_t index)
{
    auto* tile = tileIfPresent(index);
    if (tile == nullptr) {
        return;
    }
    deleteItems(m_scene, tile->overlayItems);
    if (tile->item == nullptr) {
        return;
    }
    tile->overlayItems.reserve(segments.size());
    for (const auto& segment : segments) {
        QPen pen(segment.color);
        pen.setCosmetic(true);
        pen.setWidthF(segment.width);
        auto* item = m_scene->addLine(segment.line, pen);
        item->setParentItem(tile->item);
        item->setZValue(2.0);
        tile->overlayItems.push_back(item);
    }
}

void ImageView::setOverlayPaths(const std::vector<OverlayPath>& paths,
    std::size_t index)
{
    auto* tile = tileIfPresent(index);
    if (tile == nullptr) {
        return;
    }
    deleteItems(m_scene, tile->pathItems);
    if (tile->item == nullptr) {
        return;
    }
    tile->pathItems.reserve(paths.size());
    for (const auto& overlay : paths) {
        QPen pen(overlay.color);
        pen.setCosmetic(true);
        pen.setWidthF(overlay.width);
        auto* item = m_scene->addPath(overlay.path, pen);
        item->setParentItem(tile->item);
        item->setZValue(2.0);
        tile->pathItems.push_back(item);
    }
}

void ImageView::setPointOverlays(const std::vector<PointOverlay>& overlays,
    std::size_t index)
{
    auto* tile = tileIfPresent(index);
    if (tile == nullptr) {
        return;
    }
    deleteItems(m_scene, tile->pointItems);
    tile->pointOverlayColors.clear();
    tile->pointOverlayPointCount = 0;
    if (tile->item == nullptr) {
        return;
    }
    tile->pointItems.reserve(overlays.size());
    for (const auto& overlay : overlays) {
        if (overlay.points.empty()) {
            continue;
        }
        auto* item = new PointCloudItem(
            overlay.points, tile->item->boundingRect(), overlay.color,
            overlay.size);
        item->setParentItem(tile->item);
        item->setZValue(3.0);
        tile->pointItems.push_back(item);
        tile->pointOverlayColors.push_back(overlay.color);
        tile->pointOverlayPointCount += overlay.points.size();
    }
}

std::size_t ImageView::gridBoxCount() const noexcept
{
    std::size_t count = 0;
    for (const auto& tile : m_tiles) {
        count += tile.gridItems.size();
    }
    return count;
}

std::size_t ImageView::crosshairCount() const noexcept
{
    std::size_t count = 0;
    for (const auto& tile : m_tiles) {
        count += tile.crosshairVertical.has_value() ? 1U : 0U;
        count += tile.crosshairHorizontal.has_value() ? 1U : 0U;
    }
    return count;
}

std::size_t ImageView::pointOverlayCount() const noexcept
{
    std::size_t count = 0;
    for (const auto& tile : m_tiles) {
        count += tile.pointItems.size();
    }
    return count;
}

std::size_t ImageView::pointOverlayPointCount() const noexcept
{
    std::size_t count = 0;
    for (const auto& tile : m_tiles) {
        count += tile.pointOverlayPointCount;
    }
    return count;
}

const std::vector<QColor>& ImageView::pointOverlayColors() const noexcept
{
    return m_tiles.front().pointOverlayColors;
}

void ImageView::setCrosshairs(const std::optional<QLineF>& vertical,
    const std::optional<QLineF>& horizontal, const QColor& verticalColor,
    const QColor& horizontalColor, std::size_t index)
{
    auto& tile = this->tile(index);
    tile.crosshairVertical = vertical;
    tile.crosshairHorizontal = horizontal;
    tile.crosshairVerticalColor = verticalColor;
    tile.crosshairHorizontalColor = horizontalColor;
    applyCrosshairs(tile);
}

void ImageView::applyCrosshairs(Tile& tile)
{
    for (auto* item : {tile.crosshairVerticalItem, tile.crosshairHorizontalItem}) {
        if (item != nullptr) {
            m_scene->removeItem(item);
            delete item;
        }
    }
    tile.crosshairVerticalItem = nullptr;
    tile.crosshairHorizontalItem = nullptr;
    if (tile.item == nullptr || tile.image.isNull()) {
        return;
    }
    const auto addGuide = [this, &tile](const QLineF& line, const QColor& color) {
        QPen pen(color);
        pen.setCosmetic(true);
        pen.setWidth(2);
        auto* item = m_scene->addLine(line, pen);
        item->setParentItem(tile.item);
        item->setZValue(1.5);
        return item;
    };
    if (tile.crosshairVertical.has_value()) {
        tile.crosshairVerticalItem = addGuide(*tile.crosshairVertical,
            tile.crosshairVerticalColor);
    }
    if (tile.crosshairHorizontal.has_value()) {
        tile.crosshairHorizontalItem = addGuide(*tile.crosshairHorizontal,
            tile.crosshairHorizontalColor);
    }
}

void ImageView::setCellHighlight(const std::optional<QRectF>& sceneRect,
    std::size_t index)
{
    auto* tile = tileIfPresent(index);
    if (tile == nullptr) {
        return;
    }
    if (tile->cellHighlightItem != nullptr) {
        m_scene->removeItem(tile->cellHighlightItem);
        delete tile->cellHighlightItem;
        tile->cellHighlightItem = nullptr;
    }
    if (!sceneRect.has_value() || tile->item == nullptr) {
        return;
    }
    QPen pen(Qt::red);
    pen.setCosmetic(true);
    pen.setWidth(2);
    auto* item = m_scene->addRect(*sceneRect, pen, Qt::NoBrush);
    item->setParentItem(tile->item);
    item->setZValue(4.0);
    tile->cellHighlightItem = item;
}

void ImageView::setCellHighlightPath(const std::optional<QPainterPath>& scenePath,
    std::size_t index)
{
    auto* tile = tileIfPresent(index);
    if (tile == nullptr) {
        return;
    }
    if (tile->cellHighlightItem != nullptr) {
        m_scene->removeItem(tile->cellHighlightItem);
        delete tile->cellHighlightItem;
        tile->cellHighlightItem = nullptr;
    }
    if (!scenePath.has_value() || tile->item == nullptr) {
        return;
    }
    QPen pen(Qt::red);
    pen.setCosmetic(true);
    pen.setWidth(2);
    auto* item = m_scene->addPath(*scenePath, pen, Qt::NoBrush);
    item->setParentItem(tile->item);
    item->setZValue(4.0);
    tile->cellHighlightItem = item;
}

void ImageView::setAxisIndicator(const QString& horizontal,
    const QString& vertical)
{
    m_indicatorH = horizontal;
    m_indicatorV = vertical;
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void ImageView::setScaleBarWidth(double widthCodeUnits,
    std::optional<LengthUnit> lengthUnit)
{
    const auto& tile0 = m_tiles.front();
    m_scaleBarCodeUnitsPerImagePixel = hasImage() && widthCodeUnits > 0.0
            && std::isfinite(widthCodeUnits) && tile0.image.width() > 0
        ? widthCodeUnits / static_cast<double>(tile0.image.width())
        : 0.0;
    m_scaleBarLengthUnit = lengthUnit;
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void ImageView::drawForeground(QPainter* painter, const QRectF& /*rect*/)
{
    if (!hasImage()) {
        return;
    }

    painter->save();
    painter->resetTransform();

    const auto* vp = viewport();
    if (vp == nullptr) {
        painter->restore();
        return;
    }
    constexpr int margin = 8;
    const QColor foreground(255, 255, 255, 230);
    painter->setRenderHint(QPainter::Antialiasing);

    if (!m_indicatorH.isEmpty() || !m_indicatorV.isEmpty()) {
        constexpr int armLen = 26;
        constexpr int headLen = 6;
        constexpr int headHalf = 3;
        const QPoint origin(margin, vp->height() - margin);
        const QPoint vTip(origin.x(), origin.y() - armLen);
        const QPoint hTip(origin.x() + armLen, origin.y());

        QPen pen(foreground);
        pen.setWidth(2);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter->setPen(pen);
        painter->setBrush(foreground);

        painter->drawLine(origin, vTip);
        QPolygon vHead;
        vHead << QPoint(vTip.x(), vTip.y())
              << QPoint(vTip.x() - headHalf, vTip.y() + headLen)
              << QPoint(vTip.x() + headHalf, vTip.y() + headLen);
        painter->drawPolygon(vHead);

        painter->drawLine(origin, hTip);
        QPolygon hHead;
        hHead << QPoint(hTip.x(), hTip.y())
              << QPoint(hTip.x() - headLen, hTip.y() - headHalf)
              << QPoint(hTip.x() - headLen, hTip.y() + headHalf);
        painter->drawPolygon(hHead);

        QFont font;
        font.setPointSize(11);
        font.setBold(true);
        painter->setFont(font);
        painter->setPen(foreground);

        if (!m_indicatorH.isEmpty()) {
            const auto fm = painter->fontMetrics();
            const QRectF rect(hTip.x() + 4,
                hTip.y() - fm.height() / 2.0, 40, fm.height());
            painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter,
                m_indicatorH);
        }
        if (!m_indicatorV.isEmpty()) {
            const auto fm = painter->fontMetrics();
            const QRectF rect(vTip.x() - 20,
                vTip.y() - headLen - fm.height() - 2, 40, fm.height());
            painter->drawText(rect, Qt::AlignHCenter | Qt::AlignVCenter,
                m_indicatorV);
        }
    }

    const auto& tile0 = m_tiles.front();
    if (m_scaleBarCodeUnitsPerImagePixel > 0.0 && tile0.item != nullptr) {
        const auto itemToViewport = tile0.item->deviceTransform(viewportTransform());
        const auto imageBounds = itemToViewport.mapRect(tile0.item->boundingRect())
                                     .intersected(QRectF(vp->rect()));
        const auto p0 = itemToViewport.map(QPointF(0.0, 0.0));
        const auto p1 = itemToViewport.map(QPointF(1.0, 0.0));
        const double pixelsPerImagePixel = QLineF(p0, p1).length();
        paintScaleBar(painter, imageBounds,
            m_scaleBarCodeUnitsPerImagePixel, pixelsPerImagePixel,
            m_scaleBarLengthUnit);
    }

    painter->restore();
}

void ImageView::setSliceMoveEnabled(bool enabled) noexcept
{
    m_sliceMoveEnabled = enabled;
}

void ImageView::setLineToolEnabled(bool enabled) noexcept
{
    m_lineToolEnabled = enabled;
}

void ImageView::setPlaceholder(const QString& text)
{
    m_scene->clear();
    m_tiles.assign(1, Tile{});
    m_lineGuide = nullptr;
    m_lineDragButton = Qt::NoButton;
    m_placement.reset();
    m_canvasRect.reset();
    m_scaleBarCodeUnitsPerImagePixel = 0.0;
    m_scaleBarLengthUnit.reset();
    m_placeholderText = text;
    setBackgroundBrush(palette().window());
    auto* label = m_scene->addText(text);
    label->setDefaultTextColor(palette().windowText().color());
    m_scene->setSceneRect(label->boundingRect());
    resetTransform();
    m_transformMode = TransformMode::Fit;
    m_fixedScaleFactor = 1;
}

bool ImageView::hasImage() const noexcept
{
    return std::any_of(m_tiles.begin(), m_tiles.end(), [](const Tile& tile) {
        return tile.item != nullptr && !tile.image.isNull();
    });
}

const QImage& ImageView::image() const noexcept
{
    return m_tiles.front().image;
}

const QImage& ImageView::image(std::size_t index) const noexcept
{
    static const QImage none;
    const auto* tile = tileIfPresent(index);
    return tile != nullptr ? tile->image : none;
}

QSize ImageView::composedImageSize(qreal scaleFactor) const {
    if (!hasImage()) {
        return {};
    }
    // The on-screen footprint, not the raster: a stretched display exports
    // with the aspect it shows.
    const auto base = displaySize();
    const auto baseWidth = base.width();
    const auto baseHeight = base.height();
    // Cap the longer output axis so a large zoom on big data can't allocate a
    // gigabyte image; reduce the factor (preserving aspect) when it would.
    // The factor may drop below one: a stretched footprint can exceed the
    // cap on its own, and the cap must still hold.
    constexpr int maxAxis = 8192;
    const auto cap = static_cast<qreal>(maxAxis)
        / std::max(baseWidth, baseHeight);
    const auto effective = std::min(std::max(scaleFactor, 1.0), cap);
    const auto outWidth = std::max(1,
        static_cast<int>(std::round(baseWidth * effective)));
    const auto outHeight = std::max(1,
        static_cast<int>(std::round(baseHeight * effective)));
    return {outWidth, outHeight};
}

QImage ImageView::composedImage(qreal scaleFactor) const {
    return composedImage(composedImageSize(scaleFactor));
}

QImage ImageView::composedImage(QSize outputSize, const QFont* exportFont,
                                bool omitOuterGridEdges) const {
    if (!hasImage() || outputSize.isEmpty()) {
        return {};
    }
    const int outWidth = outputSize.width();
    const int outHeight = outputSize.height();
    QImage out(outWidth, outHeight, QImage::Format_ARGB32_Premultiplied);
    if (out.isNull()) {
        return {};
    }
    out.fill(Qt::transparent);
    QPainter painter(&out);
    // Smooth upscaling of the raster plus crisp vector overlays (grid boxes,
    // contours, vector arrows) at the higher pixel count.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Render the whole scene (tiles + grid boxes + overlays) from the tiles'
    // own footprint, so the export matches the on-screen composition, scaled.
    // Navigation and interaction guides belong only in the display windows.
    std::vector<std::pair<QGraphicsItem*, bool>> guides;
    const auto hideGuide = [&guides](QGraphicsItem* item) {
        if (item != nullptr) {
            guides.emplace_back(item, item->isVisible());
            item->setVisible(false);
        }
    };
    hideGuide(m_lineGuide);
    for (const auto& tile : m_tiles) {
        hideGuide(tile.cellHighlightItem);
        hideGuide(tile.crosshairVerticalItem);
        hideGuide(tile.crosshairHorizontalItem);
        for (auto* item : tile.gridItems) {
            if (auto* rectangle = dynamic_cast<CrispRectItem*>(item)) {
                rectangle->omitOuterEdges = omitOuterGridEdges;
            }
        }
    }
    // The tiles' scene footprint, not the image rect: on a virtual canvas
    // the item sits at its cell offset, and the export must follow it; over
    // a zoomed pair only the framed window is exported.
    const auto source = exportSceneRect();
    m_scene->render(&painter, QRectF(0.0, 0.0, outWidth, outHeight), source,
                    Qt::IgnoreAspectRatio);
    for (const auto& tile : m_tiles) {
        for (auto* item : tile.gridItems) {
            if (auto* rectangle = dynamic_cast<CrispRectItem*>(item)) {
                rectangle->omitOuterEdges = false;
            }
        }
    }
    // Output pixels per raster pixel of tile 0: the scene-to-output scale
    // times the tile's own placement scale. The classic scene and the
    // virtual canvas both reduce this to outWidth over the raster width.
    const auto& tile0 = m_tiles.front();
    const double pixelsPerImagePixel = source.width() > 0.0 && tile0.image.width() > 0
        ? static_cast<double>(outWidth) / source.width()
            * tile0.sceneRect.width() / tile0.image.width()
        : 0.0;
    paintScaleBar(&painter, QRectF(0.0, 0.0, outWidth, outHeight), m_scaleBarCodeUnitsPerImagePixel,
                  pixelsPerImagePixel, m_scaleBarLengthUnit, exportFont);
    for (const auto& [item, visible] : guides) {
        item->setVisible(visible);
    }
    return out;
}

void ImageView::fitToWindow()
{
    if (hasImage()) {
        m_transformMode = TransformMode::Fit;
        fitImage();
    }
}

void ImageView::setFixedScale(int factor)
{
    if (!hasImage() || factor < 1) {
        return;
    }
    m_transformMode = TransformMode::FixedScale;
    m_fixedScaleFactor = factor;
    applyFixedScale();
}

void ImageView::zoomBy(qreal factor)
{
    if (!hasImage() || !(factor > 0.0)) {
        return;
    }
    scale(factor, factor);
    m_transformMode = TransformMode::Custom;
    emit zoomChanged();
    noteViewChanged();
}

void ImageView::zoomToRect(const QRectF& imageRect, bool confineScene)
{
    const auto& tile0 = m_tiles.front();
    if (!hasImage() || tile0.item == nullptr || imageRect.isEmpty()) {
        return;
    }
    zoomToSceneRect(tile0.item->mapRectToScene(imageRect), confineScene);
}

void ImageView::zoomToSceneRect(const QRectF& sceneTarget, bool confineScene)
{
    if (!hasImage() || sceneTarget.isEmpty()) {
        return;
    }
    m_transformMode = TransformMode::Custom;
    if (confineScene) {
        m_scene->setSceneRect(sceneTarget);
    }
    fitSceneRect(sceneTarget);
}

void ImageView::showSceneWindow(const QRectF& sceneRect)
{
    if (!hasImage() || sceneRect.isEmpty()) {
        return;
    }
    m_scene->setSceneRect(sceneRect);
    centerOn(sceneRect.center());
    noteViewChanged();
}

void ImageView::panViewport(const QPoint& delta)
{
    if (!hasImage() || (delta.x() == 0 && delta.y() == 0)) {
        return;
    }
    auto* const hBar = horizontalScrollBar();
    auto* const vBar = verticalScrollBar();
    if (hBar->maximum() == hBar->minimum()
        && vBar->maximum() == vBar->minimum()) {
        // Neither axis has anywhere to scroll: the scene already fits the
        // viewport. Translating instead would slide a fully visible image
        // off-centre and demote the display mode to Custom — a change
        // MainWindow never hears about, since panning raises no mode signal
        // the way an explicit zoom does, leaving the Scale button stale.
        return;
    }
    // Scrolling leaves the transform — and therefore the display mode —
    // untouched: a fixed scale panned by its scroll bars is still that fixed
    // scale, locally and on a virtual canvas alike.
    hBar->setValue(hBar->value() - delta.x());
    vBar->setValue(vBar->value() - delta.y());
}

void ImageView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (hasImage()) {
        fitToWindow();
        emit fitRequested();
        event->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void ImageView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressPosition = event->position().toPoint();
        if (hasImage() && (event->modifiers() & Qt::ShiftModifier)) {
            m_panActive = true;
            m_lastPanPosition = event->position().toPoint();
            m_panAccumulated = QPointF();
            setCursor(Qt::ClosedHandCursor);
            emit panDragBegan();
            event->accept();
            return;
        }
        m_panActive = false;
    }
    bool handled = false;
    // The line tool takes a Shift+middle/right press or a right press; with
    // it off (a warped view) a plain right press still starts a slice move.
    const bool shiftPressed = event->modifiers() & Qt::ShiftModifier;
    const bool linePress = m_lineToolEnabled
        && (shiftPressed || event->button() == Qt::RightButton);
    const bool movePress = !m_lineToolEnabled && m_sliceMoveEnabled
        && event->button() == Qt::RightButton && !shiftPressed;
    if ((event->button() == Qt::MiddleButton || event->button() == Qt::RightButton)
        && hasImage() && (linePress || movePress)) {
        if (shiftPressed || event->button() == Qt::RightButton) {
            m_lineDragButton = event->button();
            m_linePressPosition = event->position().toPoint();
            m_lineDragShiftHeld = event->modifiers() & Qt::ShiftModifier;
            // The guide and the eventual request belong to the tile pressed.
            m_lineGuideTile = tileAt(mapToScene(m_linePressPosition));
            // Show the guide immediately for Shift+clicks (explicit line-plot
            // request); for plain right-clicks in 3-D, defer until the drag
            // exceeds the threshold so the guide doesn't flash on a slice move.
            if (m_lineDragShiftHeld) {
                showLineGuide(event->position().toPoint());
            }
            handled = true;
        }
    }
    if (!handled) {
        QGraphicsView::mousePressEvent(event);
    }
}

void ImageView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_panActive && event->button() == Qt::LeftButton) {
        m_panActive = false;
        unsetCursor();
        emit panDragEnded(m_panAccumulated);
        m_panAccumulated = QPointF();
        event->accept();
        return;
    }
    const bool wasLineDrag = event->button() == m_lineDragButton
        && m_lineDragButton != Qt::NoButton;
    if (!wasLineDrag) {
        QGraphicsView::mouseReleaseEvent(event);
    }
    // Clamp a raster position to the tile's image.
    const auto clampedPixel = [](const Tile& tile, const QPointF& position) {
        return QPoint(
            std::clamp(static_cast<int>(std::floor(position.x())),
                0, std::max(0, tile.image.width() - 1)),
            std::clamp(static_cast<int>(std::floor(position.y())),
                0, std::max(0, tile.image.height() - 1)));
    };
    if (wasLineDrag) {
        const auto button = m_lineDragButton;
        const auto shiftHeld = m_lineDragShiftHeld;
        m_lineDragButton = Qt::NoButton;
        m_lineDragShiftHeld = false;
        if (!hasImage()) {
            clearLineGuide();
            return;
        }
        const auto releasePosition = event->position().toPoint();
        const auto tileIndex = m_lineGuideTile < m_tiles.size()
            && m_tiles[m_lineGuideTile].item != nullptr ? m_lineGuideTile : std::size_t{0};
        const auto& tile = m_tiles[tileIndex];
        const auto pixel = clampedPixel(tile,
            imagePositionIn(tileIndex, mapToScene(releasePosition)));
        const auto drag = releasePosition - m_linePressPosition;
        constexpr int lineDragThreshold = 6;
        const bool wasDrag = std::abs(drag.x()) > lineDragThreshold
            || std::abs(drag.y()) > lineDragThreshold;
        if (m_sliceMoveEnabled && !shiftHeld && !wasDrag) {
            clearLineGuide();
            emit tileSliceMoveRequested(
                static_cast<int>(tileIndex), pixel.x(), pixel.y(), button);
            if (tileIndex == 0) {
                emit sliceMoveRequested(pixel.x(), pixel.y(), button);
            }
        } else if (button == Qt::MiddleButton && wasDrag) {
            // Middle drag does nothing — only shift+middle clicks produce
            // line plots. Right drags are unaffected.
            clearLineGuide();
        } else if (m_lineToolEnabled && (shiftHeld || wasDrag)) {
            // Leave the guide visible as a temporary preview while the line
            // plot is computed asynchronously.
            const auto effectiveButton = wasDrag
                ? (std::abs(drag.x()) > std::abs(drag.y())
                    ? Qt::MiddleButton : Qt::RightButton)
                : button;
            emit tileLinePlotRequested(static_cast<int>(tileIndex),
                pixel.x(), pixel.y(), effectiveButton);
            if (tileIndex == 0) {
                emit linePlotRequested(pixel.x(), pixel.y(), effectiveButton);
            }
        } else {
            clearLineGuide();
        }
        return;
    }
    if (event->button() != Qt::LeftButton || !hasImage()) {
        return;
    }
    const auto releasePosition = event->position().toPoint();
    const auto drag = releasePosition - m_pressPosition;
    constexpr int minimumDrag = 4;
    if (std::abs(drag.x()) > minimumDrag && std::abs(drag.y()) > minimumDrag) {
        const auto pressScene = mapToScene(m_pressPosition);
        const auto releaseScene = mapToScene(releasePosition);
        emit rubberBandSelectedScene(QRectF(pressScene, releaseScene).normalized());
        // Tile 0's raster pixels, as the single-tile owner reads them.
        emit rubberBandSelected(QRectF(
            imagePositionIn(0, pressScene),
            imagePositionIn(0, releaseScene)).normalized());
        return;
    }
    const auto releaseScene = mapToScene(releasePosition);
    const auto tileIndex = tileAt(releaseScene);
    const auto pixel = clampedPixel(m_tiles[tileIndex],
        imagePositionIn(tileIndex, releaseScene));
    emit tileProbeClicked(static_cast<int>(tileIndex), pixel.x(), pixel.y());
    if (tileIndex == 0) {
        emit probeClicked(pixel.x(), pixel.y());
    }
}

void ImageView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_panActive && (event->buttons() & Qt::LeftButton)) {
        const QPoint current = event->position().toPoint();
        const QPoint delta = current - m_lastPanPosition;
        m_panAccumulated += mapToScene(current) - mapToScene(m_lastPanPosition);
        m_lastPanPosition = current;
        emit panDragMoved(m_panAccumulated, delta);
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
    if (!hasImage()) {
        return;
    }
    if (m_lineDragButton != Qt::NoButton) {
        const auto drag = event->position().toPoint() - m_linePressPosition;
        constexpr int guideThreshold = 6;
        if (m_lineDragShiftHeld
            || std::abs(drag.x()) > guideThreshold
            || std::abs(drag.y()) > guideThreshold) {
            updateLineGuide(event->position().toPoint());
        }
    }
    const auto scenePosition = mapToScene(event->position().toPoint());
    const auto tileIndex = tileAt(scenePosition);
    const auto& tile = m_tiles[tileIndex];
    const auto imagePosition = imagePositionIn(tileIndex, scenePosition);
    const auto x = static_cast<int>(std::floor(imagePosition.x()));
    const auto y = static_cast<int>(std::floor(imagePosition.y()));
    if (x >= 0 && y >= 0 && x < tile.image.width() && y < tile.image.height()) {
        emit tileProbeMoved(static_cast<int>(tileIndex), x, y);
        if (tileIndex == 0) {
            emit probeMoved(x, y);
        }
    }
}

void ImageView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    if (m_transformMode == TransformMode::Fit) {
        fitImage();
    }
    emit viewportResized(viewport()->size());
    // A resize changes how much of the raster is on screen even when nothing
    // moved, and in Fit mode fitImage above has just changed the transform.
    emit viewportMoved();
    noteViewChanged();
}

void ImageView::changeEvent(QEvent* event)
{
    QGraphicsView::changeEvent(event);
    // The placeholder is drawn from palette roles, but both the background
    // brush and the text item's colour are copies taken when it was set, so a
    // skin changed while a placeholder is up would leave it in the old one.
    // A view showing a raster keeps the fixed viewport background instead.
    if (event->type() == QEvent::PaletteChange && !m_placeholderText.isEmpty()) {
        setPlaceholder(m_placeholderText);
    }
}

void ImageView::scrollContentsBy(int dx, int dy)
{
    QGraphicsView::scrollContentsBy(dx, dy);
    if (m_placement.has_value()) {
        emit canvasScrolled();
    }
    // Unconditionally, unlike canvasScrolled: a local view scrolls its own
    // raster under the viewport with no placement set, which moves what is
    // visible just as much.
    emit viewportMoved();
    noteViewChanged();
}

void ImageView::keyPressEvent(QKeyEvent* event)
{
    // Only while this view holds focus and has something to pan. Anything else
    // -- including an arrow with a modifier, which belongs to whatever else may
    // want it -- falls through to the base class.
    //
    // KeypadModifier is masked out rather than treated as a modifier: macOS
    // stamps it on the arrow keys, which Qt documents ("the arrow keys are
    // considered part of the keypad"), so testing against NoModifier alone
    // would leave arrow panning dead there. The QShortcut binding this
    // replaced normalized that away for us.
    const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
    if (hasImage() && modifiers == Qt::NoModifier) {
        switch (event->key()) {
        case Qt::Key_Left:
            emit panStepRequested(QPointF(1.0, 0.0));
            event->accept();
            return;
        case Qt::Key_Right:
            emit panStepRequested(QPointF(-1.0, 0.0));
            event->accept();
            return;
        case Qt::Key_Up:
            emit panStepRequested(QPointF(0.0, 1.0));
            event->accept();
            return;
        case Qt::Key_Down:
            emit panStepRequested(QPointF(0.0, -1.0));
            event->accept();
            return;
        default:
            break;
        }
    }
    QGraphicsView::keyPressEvent(event);
}

void ImageView::wheelEvent(QWheelEvent* event)
{
    if (!hasImage()) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    const auto vertical = event->angleDelta().y();
    if (vertical == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    constexpr double zoomStep = 1.15;
    const auto factor = vertical > 0 ? zoomStep : 1.0 / zoomStep;
    zoomBy(factor);
    event->accept();
}

void ImageView::showLineGuide(const QPoint& viewPosition)
{
    // The guide is a child of the pressed tile's raster item, so it is built
    // in that raster's pixel coordinates like every other overlay.
    const auto tileIndex = m_lineGuideTile < m_tiles.size()
        && m_tiles[m_lineGuideTile].item != nullptr ? m_lineGuideTile : std::size_t{0};
    auto& tile = m_tiles[tileIndex];
    if (tile.item == nullptr) {
        return;
    }
    const auto imagePosition = imagePositionIn(tileIndex, mapToScene(viewPosition));
    const auto width = static_cast<double>(tile.image.width());
    const auto height = static_cast<double>(tile.image.height());

    const auto drag = viewPosition - m_linePressPosition;
    constexpr int orientThreshold = 8;
    const bool significantDrag = std::abs(drag.x()) > orientThreshold
        || std::abs(drag.y()) > orientThreshold;
    if (!m_lineToolEnabled) {
        // With the line tool off a right press is a slice move alone, and
        // a drag plots nothing: no guide at any distance.
        clearLineGuide();
        return;
    }

    // On press (no drag yet), show a slice-move guide (perpendicular).
    // Once the user drags significantly, the action switches to a line
    // plot so the guide follows the drag direction instead.
    bool horizontal;
    if (m_sliceMoveEnabled && !m_lineDragShiftHeld && !significantDrag) {
        horizontal = (m_lineDragButton != Qt::MiddleButton);
    } else if (significantDrag) {
        horizontal = std::abs(drag.x()) > std::abs(drag.y());
    } else {
        horizontal = (m_lineDragButton == Qt::MiddleButton);
    }

    QLineF line;
    if (horizontal) {
        const auto y = std::clamp(imagePosition.y(), 0.0, height);
        line = QLineF(0.0, y, width, y);
    } else {
        const auto x = std::clamp(imagePosition.x(), 0.0, width);
        line = QLineF(x, 0.0, x, height);
    }
    if (m_lineGuide != nullptr && m_lineGuide->parentItem() != tile.item) {
        clearLineGuide();
    }
    if (m_lineGuide == nullptr) {
        QPen pen(Qt::white);
        pen.setStyle(Qt::DashLine);
        pen.setCosmetic(true);
        m_lineGuide = m_scene->addLine(line, pen);
        m_lineGuide->setParentItem(tile.item);
        m_lineGuide->setZValue(3.0);
    } else {
        m_lineGuide->setLine(line);
    }
}

void ImageView::updateLineGuide(const QPoint& viewPosition)
{
    const auto drag = viewPosition - m_linePressPosition;
    constexpr int minimumDrag = 4;
    if (std::max(std::abs(drag.x()), std::abs(drag.y())) < minimumDrag) {
        return;
    }
    showLineGuide(viewPosition);
}

void ImageView::setActiveBorder(bool active)
{
    if (active) {
        setStyleSheet(QStringLiteral(
            "QGraphicsView { border: 2px solid #ff8800; }"));
    } else {
        setStyleSheet(QString());
    }
}

void ImageView::clearLineGuide()
{
    if (m_lineGuide != nullptr) {
        m_scene->removeItem(m_lineGuide);
        delete m_lineGuide;
        m_lineGuide = nullptr;
    }
}

void ImageView::fitImage()
{
    if (!hasImage()) {
        return;
    }
    resetTransform();
    // A pair layout's canvas is what Fit frames, so the panel showing one
    // layer at a time keeps one zoom as the shown layer changes. A remote
    // virtual canvas keeps framing the fetched window, as it always has.
    if (m_canvasRect.has_value() && !m_placement.has_value()) {
        fitSceneRect(*m_canvasRect);
    } else {
        fitSceneRect(tilesRect());
    }
}

void ImageView::fitSceneRect(const QRectF& rect)
{
    if (viewport() == nullptr) {
        return;
    }
    if (rect.isEmpty()) {
        return;
    }
    // A pane collapsed below the margin still gets a transform, so the view
    // always carries the stretch it was given (isotropicScale, export size);
    // the next resize refits it properly.
    constexpr int margin = 2;
    const QRectF viewRect
        = viewport()->rect().adjusted(margin, margin, -margin, -margin);
    const auto viewWidth = std::max(1.0, viewRect.width());
    const auto viewHeight = std::max(1.0, viewRect.height());
    const auto scale = std::min(
        viewWidth / (rect.width() * m_stretch.x()),
        viewHeight / (rect.height() * m_stretch.y()));
    setTransform(QTransform::fromScale(
        scale * m_stretch.x(), scale * m_stretch.y()));
    centerOn(rect.center());
    noteViewChanged();
}

void ImageView::setDisplayStretch(qreal sx, qreal sy)
{
    const auto sane = [](qreal value) {
        return std::isfinite(value) && value > 0.0 ? value : 1.0;
    };
    const QPointF stretch(sane(sx), sane(sy));
    if (stretch == m_stretch) {
        // Re-applying an equal transform would recenter a virtual canvas.
        return;
    }
    const auto previous = m_stretch;
    m_stretch = stretch;
    if (!hasImage()) {
        return;
    }
    switch (m_transformMode) {
    case TransformMode::Fit:
        fitImage();
        break;
    case TransformMode::FixedScale:
        applyFixedScale();
        break;
    case TransformMode::Custom: {
        // Keep the zoom and what is under the viewport centre; only the
        // ratio of the axes changes.
        const auto centre
            = mapToScene(viewport()->rect()).boundingRect().center();
        setTransform(QTransform::fromScale(
            m_stretch.x() / previous.x(), m_stretch.y() / previous.y()), true);
        centerOn(centre);
        break;
    }
    }
    emit viewportMoved();
    noteViewChanged();
}

qreal ImageView::isotropicScale() const
{
    return transform().m11() / m_stretch.x();
}

QSizeF ImageView::displaySize() const
{
    if (!hasImage()) {
        return {};
    }
    // Counted by rasters on show, not list length: a cleared overlay slot
    // must not turn a single raster into a footprint measurement. A lone
    // raster on a layout canvas (a warp drawn for the screen) is not its own
    // footprint: its pixels cover the scene rect it was placed over. On a
    // virtual canvas the pixels are the cells, so the raster is.
    const auto placed = std::count_if(m_tiles.begin(), m_tiles.end(),
        [](const Tile& tile) { return tile.item != nullptr && !tile.image.isNull(); });
    if (placed == 1 && (!m_canvasRect.has_value() || m_placement.has_value())) {
        const auto& tile0 = *std::find_if(m_tiles.begin(), m_tiles.end(),
            [](const Tile& tile) { return tile.item != nullptr && !tile.image.isNull(); });
        return {tile0.image.width() * m_stretch.x(),
            tile0.image.height() * m_stretch.y()};
    }
    // Several tiles, or a layout canvas: the footprint is in display units.
    const auto footprint = exportSceneRect();
    return {footprint.width() * m_stretch.x(), footprint.height() * m_stretch.y()};
}

void ImageView::applyFixedScale()
{
    if (!hasImage()) {
        return;
    }
    const auto factor = static_cast<qreal>(m_fixedScaleFactor);
    QTransform desired;
    if (m_placement.has_value() || m_canvasRect.has_value()) {
        // Placed tiles: scene units are display units (finest cells on the
        // virtual canvas) and the item transforms already map raster pixels
        // onto them, so the view scales scene units to screen pixels directly.
        desired = QTransform::fromScale(
            factor * m_stretch.x(), factor * m_stretch.y());
    } else {
        const auto& tile0 = m_tiles.front();
        const auto logicalWidth = std::max(1, tile0.logicalSize.width());
        const auto logicalHeight = std::max(1, tile0.logicalSize.height());
        desired = QTransform::fromScale(
            factor * m_stretch.x() * logicalWidth / std::max(1, tile0.image.width()),
            factor * m_stretch.y() * logicalHeight
                / std::max(1, tile0.image.height()));
    }
    // Equal-transform replacements must not touch the view: re-setting the
    // same matrix would recenter the scroll position on a virtual canvas.
    if (transform() != desired) {
        setTransform(desired);
    }
    noteViewChanged();
}

} // namespace amrvis::qt
