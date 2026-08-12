#include "ImageView.hpp"

#include "Theme.hpp"

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
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
        QWidget* widget = nullptr) override
    {
        const auto antialiasing = painter->testRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::Antialiasing, false);
        QGraphicsRectItem::paint(painter, option, widget);
        painter->setRenderHint(QPainter::Antialiasing, antialiasing);
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
        return m_bounds.adjusted(-m_size, -m_size, m_size, m_size);
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

} // namespace

ImageView::ImageView(QWidget* parent)
    : QGraphicsView(parent)
    , m_scene(new QGraphicsScene(this))
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

void ImageView::setImage(
    const QImage& image, ImageTransformPolicy transformPolicy,
    QSize logicalSize, const std::optional<VirtualPlacement>& placement)
{
    const bool sizeChanged = m_image.isNull() || m_image.size() != image.size();
    // An explicitly incompatible raster resets Custom mode. Fixed scale is a
    // user-selected persistent mode and therefore survives every replacement,
    // including a delayed result whose policy was computed before the scale
    // selection. Fit likewise remains a mode and refits every replacement.
    if (transformPolicy == ImageTransformPolicy::Refit
        && m_transformMode == TransformMode::Custom) {
        m_transformMode = TransformMode::Fit;
    }
    m_scene->clear();
    m_gridItems.clear();
    m_overlayItems.clear();
    m_pathItems.clear();
    m_pointItems.clear();
    m_pointOverlayColors.clear();
    m_crosshairVerticalItem = nullptr;
    m_crosshairHorizontalItem = nullptr;
    m_cellHighlightItem = nullptr;
    m_lineGuide = nullptr;
    m_lineDragButton = Qt::NoButton;
    m_image = image;
    m_placeholderText.clear();
    m_logicalSize = logicalSize.isValid() ? logicalSize : image.size();
    m_item = m_scene->addPixmap(QPixmap::fromImage(m_image));
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
    applyCrosshairs();
}

void ImageView::setVirtualCanvas(
    const std::optional<VirtualPlacement>& placement)
{
    if (!hasImage()) {
        m_placement.reset();
        return;
    }
    m_placement = placement;
    applyPlacement();
    if (m_transformMode == TransformMode::FixedScale) {
        applyFixedScale();
    }
}

QRectF ImageView::imageSceneRect() const
{
    return m_item == nullptr ? QRectF() : m_item->sceneBoundingRect();
}

QRectF ImageView::visibleImageRect() const
{
    if (m_item == nullptr || viewport() == nullptr) {
        return {};
    }
    // mapRectFromScene undoes the item's placement -- identity in the classic
    // scene, the cell offset and pixel-to-cell scale on a virtual canvas -- so
    // the result is in raster pixels either way.
    return m_item->mapRectFromScene(
        mapToScene(viewport()->rect()).boundingRect()).intersected(
            m_item->boundingRect());
}

// Position the raster item and size the scene. On a virtual canvas the scene
// spans the whole domain in finest-cell units and the item maps its pixels
// onto its fetched cell window; the classic scene is the raster at the
// origin. Neither the view transform nor the scroll bars are touched here, so
// a raster replacement on an unchanged canvas never moves the view.
void ImageView::applyPlacement()
{
    if (m_item == nullptr) {
        return;
    }
    if (m_placement.has_value()) {
        const auto& cells = m_placement->itemCells;
        QTransform toCells;
        if (m_image.width() > 0 && m_image.height() > 0) {
            toCells.scale(cells.width() / m_image.width(),
                cells.height() / m_image.height());
        }
        m_item->setTransform(toCells);
        m_item->setPos(cells.topLeft());
        m_scene->setSceneRect(
            QRectF(QPointF(0.0, 0.0), m_placement->domainCells));
    } else {
        m_item->setTransform(QTransform());
        m_item->setPos(QPointF());
        m_scene->setSceneRect(m_item->boundingRect());
    }
}

void ImageView::setGridBoxes(const std::vector<GridBoxOverlay>& boxes)
{
    for (auto* item : m_gridItems) {
        m_scene->removeItem(item);
        delete item;
    }
    m_gridItems.clear();
    if (!hasImage()) {
        return;
    }
    m_gridItems.reserve(boxes.size());
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
            item->setParentItem(m_item);
            item->setBrush(Qt::NoBrush);
            item->setZValue(1.0);
            m_gridItems.push_back(item);
            continue;
        }
        auto* item = new CrispRectItem(box.rectangle, m_item);
        item->setPen(pen);
        item->setBrush(Qt::NoBrush);
        item->setZValue(1.0);
        m_gridItems.push_back(item);
    }
}

void ImageView::setOverlaySegments(const std::vector<OverlaySegment>& segments)
{
    for (auto* item : m_overlayItems) {
        m_scene->removeItem(item);
        delete item;
    }
    m_overlayItems.clear();
    if (!hasImage()) {
        return;
    }
    m_overlayItems.reserve(segments.size());
    for (const auto& segment : segments) {
        QPen pen(segment.color);
        pen.setCosmetic(true);
        pen.setWidthF(segment.width);
        auto* item = m_scene->addLine(segment.line, pen);
        item->setParentItem(m_item);
        item->setZValue(2.0);
        m_overlayItems.push_back(item);
    }
}

void ImageView::setOverlayPaths(const std::vector<OverlayPath>& paths)
{
    for (auto* item : m_pathItems) {
        m_scene->removeItem(item);
        delete item;
    }
    m_pathItems.clear();
    if (!hasImage()) {
        return;
    }
    m_pathItems.reserve(paths.size());
    for (const auto& overlay : paths) {
        QPen pen(overlay.color);
        pen.setCosmetic(true);
        pen.setWidthF(overlay.width);
        auto* item = m_scene->addPath(overlay.path, pen);
        item->setParentItem(m_item);
        item->setZValue(2.0);
        m_pathItems.push_back(item);
    }
}

void ImageView::setPointOverlays(const std::vector<PointOverlay>& overlays)
{
    for (auto* item : m_pointItems) {
        m_scene->removeItem(item);
        delete item;
    }
    m_pointItems.clear();
    m_pointOverlayColors.clear();
    if (!hasImage()) {
        return;
    }
    m_pointItems.reserve(overlays.size());
    for (const auto& overlay : overlays) {
        if (overlay.points.empty()) {
            continue;
        }
        auto* item = new PointCloudItem(
            overlay.points, m_item->boundingRect(), overlay.color,
            overlay.size);
        item->setParentItem(m_item);
        item->setZValue(3.0);
        m_pointItems.push_back(item);
        m_pointOverlayColors.push_back(overlay.color);
    }
}

std::size_t ImageView::pointOverlayCount() const noexcept
{
    return m_pointItems.size();
}

const std::vector<QColor>& ImageView::pointOverlayColors() const noexcept
{
    return m_pointOverlayColors;
}

void ImageView::setCrosshairs(const std::optional<QLineF>& vertical,
    const std::optional<QLineF>& horizontal, const QColor& verticalColor,
    const QColor& horizontalColor)
{
    m_crosshairVertical = vertical;
    m_crosshairHorizontal = horizontal;
    m_crosshairVerticalColor = verticalColor;
    m_crosshairHorizontalColor = horizontalColor;
    applyCrosshairs();
}

void ImageView::applyCrosshairs()
{
    for (auto* item : {m_crosshairVerticalItem, m_crosshairHorizontalItem}) {
        if (item != nullptr) {
            m_scene->removeItem(item);
            delete item;
        }
    }
    m_crosshairVerticalItem = nullptr;
    m_crosshairHorizontalItem = nullptr;
    if (!hasImage()) {
        return;
    }
    const auto addGuide = [this](const QLineF& line, const QColor& color) {
        QPen pen(color);
        pen.setCosmetic(true);
        pen.setWidth(2);
        auto* item = m_scene->addLine(line, pen);
        item->setParentItem(m_item);
        item->setZValue(1.5);
        return item;
    };
    if (m_crosshairVertical.has_value()) {
        m_crosshairVerticalItem = addGuide(*m_crosshairVertical,
            m_crosshairVerticalColor);
    }
    if (m_crosshairHorizontal.has_value()) {
        m_crosshairHorizontalItem = addGuide(*m_crosshairHorizontal,
            m_crosshairHorizontalColor);
    }
}

void ImageView::setCellHighlight(const std::optional<QRectF>& sceneRect)
{
    if (m_cellHighlightItem != nullptr) {
        m_scene->removeItem(m_cellHighlightItem);
        delete m_cellHighlightItem;
        m_cellHighlightItem = nullptr;
    }
    if (!sceneRect.has_value() || !hasImage()) {
        return;
    }
    QPen pen(Qt::red);
    pen.setCosmetic(true);
    pen.setWidth(2);
    auto* item = m_scene->addRect(*sceneRect, pen, Qt::NoBrush);
    item->setParentItem(m_item);
    item->setZValue(4.0);
    m_cellHighlightItem = item;
}

void ImageView::setCellHighlightPath(const std::optional<QPainterPath>& scenePath)
{
    if (m_cellHighlightItem != nullptr) {
        m_scene->removeItem(m_cellHighlightItem);
        delete m_cellHighlightItem;
        m_cellHighlightItem = nullptr;
    }
    if (!scenePath.has_value() || !hasImage()) {
        return;
    }
    QPen pen(Qt::red);
    pen.setCosmetic(true);
    pen.setWidth(2);
    auto* item = m_scene->addPath(*scenePath, pen, Qt::NoBrush);
    item->setParentItem(m_item);
    item->setZValue(4.0);
    m_cellHighlightItem = item;
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

void ImageView::drawForeground(QPainter* painter, const QRectF& /*rect*/)
{
    if (!hasImage() || (m_indicatorH.isEmpty() && m_indicatorV.isEmpty())) {
        return;
    }

    painter->save();
    painter->resetTransform();

    constexpr int armLen = 26;
    constexpr int headLen = 6;
    constexpr int headHalf = 3;
    constexpr int margin = 8;
    const QColor fg(255, 255, 255, 200);

    const auto* vp = viewport();
    if (vp == nullptr) {
        painter->restore();
        return;
    }
    const int vh = vp->height();

    const QPoint origin(margin, vh - margin);
    const QPoint vTip(origin.x(), origin.y() - armLen);
    const QPoint hTip(origin.x() + armLen, origin.y());

    QPen pen(fg);
    pen.setWidth(2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(fg);
    painter->setRenderHint(QPainter::Antialiasing);

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
    painter->setPen(fg);

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
    m_gridItems.clear();
    m_overlayItems.clear();
    m_pathItems.clear();
    m_pointItems.clear();
    m_crosshairVertical.reset();
    m_crosshairHorizontal.reset();
    m_crosshairVerticalItem = nullptr;
    m_crosshairHorizontalItem = nullptr;
    m_cellHighlightItem = nullptr;
    m_lineGuide = nullptr;
    m_lineDragButton = Qt::NoButton;
    m_item = nullptr;
    m_image = {};
    m_logicalSize = {};
    m_placement.reset();
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
    return m_item != nullptr && !m_image.isNull();
}

const QImage& ImageView::image() const noexcept
{
    return m_image;
}

QImage ImageView::composedImage(qreal scaleFactor) const
{
    if (m_image.isNull()) {
        return {};
    }
    const auto baseWidth = m_image.width();
    const auto baseHeight = m_image.height();
    // Cap the longer output axis so a large zoom on big data can't allocate a
    // gigabyte image; reduce the factor (preserving aspect) when it would.
    constexpr int maxAxis = 8192;
    const auto cap = static_cast<qreal>(maxAxis)
        / static_cast<qreal>(std::max(baseWidth, baseHeight));
    const auto effective = std::clamp(scaleFactor, 1.0, std::max(1.0, cap));
    const auto outWidth = std::max(1,
        static_cast<int>(std::round(baseWidth * effective)));
    const auto outHeight = std::max(1,
        static_cast<int>(std::round(baseHeight * effective)));
    QImage out(outWidth, outHeight, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    // Smooth upscaling of the raster plus crisp vector overlays (grid boxes,
    // contours, vector arrows) at the higher pixel count.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Render the whole scene (pixmap + grid boxes + overlays) from the image's
    // own pixel rect, so the export matches the on-screen composition, scaled.
    // Transient interaction overlays (line-plot guide, cell highlight) must not
    // be baked into the export, so hide them for this render and restore them.
    const bool guideVisible =
        m_lineGuide != nullptr && m_lineGuide->isVisible();
    const bool cellVisible =
        m_cellHighlightItem != nullptr && m_cellHighlightItem->isVisible();
    if (m_lineGuide != nullptr) {
        m_lineGuide->setVisible(false);
    }
    if (m_cellHighlightItem != nullptr) {
        m_cellHighlightItem->setVisible(false);
    }
    // The raster's scene footprint, not the image rect: on a virtual canvas
    // the item sits at its cell offset, and the export must follow it.
    m_scene->render(&painter, QRectF(0.0, 0.0, outWidth, outHeight),
        m_item->sceneBoundingRect());
    if (m_lineGuide != nullptr) {
        m_lineGuide->setVisible(guideVisible);
    }
    if (m_cellHighlightItem != nullptr) {
        m_cellHighlightItem->setVisible(cellVisible);
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
}

void ImageView::zoomToRect(const QRectF& imageRect)
{
    if (!hasImage() || imageRect.isEmpty()) {
        return;
    }
    m_transformMode = TransformMode::Custom;
    fitInView(m_item->mapRectToScene(imageRect), Qt::KeepAspectRatio);
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
    if ((event->button() == Qt::MiddleButton || event->button() == Qt::RightButton)
        && hasImage() && m_lineToolEnabled) {
        if ((event->modifiers() & Qt::ShiftModifier)
            || event->button() == Qt::RightButton) {
            m_lineDragButton = event->button();
            m_linePressPosition = event->position().toPoint();
            m_lineDragShiftHeld = event->modifiers() & Qt::ShiftModifier;
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
        const auto imagePosition = m_item->mapFromScene(mapToScene(releasePosition));
        const auto x = std::clamp(static_cast<int>(std::floor(imagePosition.x())),
            0, m_image.width() - 1);
        const auto y = std::clamp(static_cast<int>(std::floor(imagePosition.y())),
            0, m_image.height() - 1);
        const auto drag = releasePosition - m_linePressPosition;
        constexpr int lineDragThreshold = 6;
        const bool wasDrag = std::abs(drag.x()) > lineDragThreshold
            || std::abs(drag.y()) > lineDragThreshold;
        if (m_sliceMoveEnabled && !shiftHeld && !wasDrag) {
            clearLineGuide();
            emit sliceMoveRequested(x, y, button);
        } else if (button == Qt::MiddleButton && wasDrag) {
            // Middle drag does nothing — only shift+middle clicks produce
            // line plots. Right drags are unaffected.
            clearLineGuide();
        } else if (shiftHeld || wasDrag) {
            // Leave the guide visible as a temporary preview while the line
            // plot is computed asynchronously.
            const auto effectiveButton = wasDrag
                ? (std::abs(drag.x()) > std::abs(drag.y())
                    ? Qt::MiddleButton : Qt::RightButton)
                : button;
            emit linePlotRequested(x, y, effectiveButton);
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
        emit rubberBandSelected(QRectF(
            m_item->mapFromScene(mapToScene(m_pressPosition)),
            m_item->mapFromScene(mapToScene(releasePosition))).normalized());
        return;
    }
    const auto imagePosition = m_item->mapFromScene(mapToScene(releasePosition));
    const auto x = std::clamp(static_cast<int>(std::floor(imagePosition.x())),
        0, m_image.width() - 1);
    const auto y = std::clamp(static_cast<int>(std::floor(imagePosition.y())),
        0, m_image.height() - 1);
    emit probeClicked(x, y);
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
    const auto imagePosition = m_item->mapFromScene(scenePosition);
    const auto x = static_cast<int>(std::floor(imagePosition.x()));
    const auto y = static_cast<int>(std::floor(imagePosition.y()));
    if (x >= 0 && y >= 0 && x < m_image.width() && y < m_image.height()) {
        emit probeMoved(x, y);
    }
}

void ImageView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    if (m_transformMode == TransformMode::Fit) {
        fitImage();
    }
    emit viewportResized(viewport()->size());
}

void ImageView::scrollContentsBy(int dx, int dy)
{
    QGraphicsView::scrollContentsBy(dx, dy);
    if (m_placement.has_value()) {
        emit canvasScrolled();
    }
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
    // The guide is a child of the raster item, so it is built in image
    // (raster-pixel) coordinates like every other overlay.
    const auto imagePosition = m_item->mapFromScene(mapToScene(viewPosition));
    const auto width = static_cast<double>(m_image.width());
    const auto height = static_cast<double>(m_image.height());

    const auto drag = viewPosition - m_linePressPosition;
    constexpr int orientThreshold = 8;
    const bool significantDrag = std::abs(drag.x()) > orientThreshold
        || std::abs(drag.y()) > orientThreshold;

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
    if (m_lineGuide == nullptr) {
        QPen pen(Qt::white);
        pen.setStyle(Qt::DashLine);
        pen.setCosmetic(true);
        m_lineGuide = m_scene->addLine(line, pen);
        m_lineGuide->setParentItem(m_item);
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
    if (m_item != nullptr) {
        resetTransform();
        fitInView(m_item, Qt::KeepAspectRatio);
    }
}

void ImageView::applyFixedScale()
{
    if (m_item == nullptr) {
        return;
    }
    const auto factor = static_cast<qreal>(m_fixedScaleFactor);
    QTransform desired;
    if (m_placement.has_value()) {
        // Virtual canvas: scene units are finest cells and the item transform
        // already maps raster pixels onto cells, so the view scales cells to
        // screen pixels directly.
        desired = QTransform::fromScale(factor, factor);
    } else {
        const auto logicalWidth = std::max(1, m_logicalSize.width());
        const auto logicalHeight = std::max(1, m_logicalSize.height());
        desired = QTransform::fromScale(
            factor * logicalWidth / std::max(1, m_image.width()),
            factor * logicalHeight / std::max(1, m_image.height()));
    }
    // Equal-transform replacements must not touch the view: re-setting the
    // same matrix would recenter the scroll position on a virtual canvas.
    if (transform() != desired) {
        setTransform(desired);
    }
}

} // namespace amrvis::qt
