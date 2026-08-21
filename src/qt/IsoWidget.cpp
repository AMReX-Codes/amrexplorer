#include "IsoWidget.hpp"

#include "Theme.hpp"

#include <amrexplorer/render2d/Palette.hpp>

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPolygonF>
#include <QPushButton>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace amrvis::qt {
namespace {

constexpr double pi = 3.14159265358979323846;

// Cube corner indexing: bit 0 = x side, bit 1 = y side, bit 2 = z side.
constexpr std::array<std::array<int, 2>, 12> boxEdges{{
    {{0, 1}}, {{2, 3}}, {{4, 5}}, {{6, 7}},
    {{0, 2}}, {{1, 3}}, {{4, 6}}, {{5, 7}},
    {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}},
}};

} // namespace

IsoWidget::IsoWidget(QWidget* parent)
    : QWidget(parent)
    , m_camera{30.0 * pi / 180.0, 30.0 * pi / 180.0, 1.0}
{
    setMinimumSize(200, 150);
    setMouseTracking(true);

    const auto makeBtn = [this](const QString& label) {
        auto* btn = new QPushButton(label, this);
        btn->setFixedSize(32, 26);
        btn->setFocusPolicy(Qt::NoFocus);
        QFont f = btn->font();
        f.setPointSize(9);
        btn->setFont(f);
        btn->setStyleSheet(
            QStringLiteral("QPushButton { color: rgba(255,255,255,230);"
            " background: rgba(255,255,255,30); border: 1px solid"
            " rgba(255,255,255,80); border-radius: 2px; }"
            "QPushButton:hover { background: rgba(255,255,255,60); }"));
        return btn;
    };
    m_btnXY = makeBtn(QStringLiteral("XY"));
    m_btnXZ = makeBtn(QStringLiteral("XZ"));
    m_btnYZ = makeBtn(QStringLiteral("YZ"));

    connect(m_btnXY, &QPushButton::clicked, this, [this] {
        setViewAngles(orthoPresetXY.azimuth, orthoPresetXY.elevation);
    });
    connect(m_btnXZ, &QPushButton::clicked, this, [this] {
        setViewAngles(orthoPresetXZ.azimuth, orthoPresetXZ.elevation);
    });
    connect(m_btnYZ, &QPushButton::clicked, this, [this] {
        setViewAngles(orthoPresetYZ.azimuth, orthoPresetYZ.elevation);
    });
}

void IsoWidget::setGeometry(const DatasetMetadata& metadata)
{
    m_hasGeometry = metadata.dimension == 3;
    m_domain = datasetSampleBounds(metadata);
    m_levels.clear();
    if (m_hasGeometry) {
        m_levels.reserve(metadata.levels.size());
        for (const auto& level : metadata.levels) {
            m_levels.push_back({level.level, level.domain, level.cellSize,
                level.indexOrigin, level.boxes});
        }
        // Default to the domain center so a caller that sets geometry but
        // forgets setSlicePositions draws the planes mid-domain instead of
        // clamped to the lower face by the {0,0,0} default.
        m_slicePositions = m_domain.center().values;
    }
    update();
}

void IsoWidget::setSlicePositions(double x, double y, double z)
{
    m_slicePositions = {x, y, z};
    update();
}

void IsoWidget::setSlicePlanesVisible(bool visible)
{
    m_slicePlanesVisible = visible;
    update();
}

void IsoWidget::setColorPalette(const Palette* palette)
{
    m_palette = palette;
    update();
}

void IsoWidget::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), viewportBackground());
    if (!m_hasGeometry) {
        painter.setPen(viewportForeground());
        painter.drawText(rect(), Qt::AlignCenter, tr("3-D overview"));
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);

    const auto frame = viewportFrame(width(), height());
    for (const auto& level : m_levels) {
        const QPen pen(levelOutlineColor(level.level), 1);
        for (const auto& box : level.boxes) {
            drawBox(painter, frame, physicalBox(level, box), pen);
        }
    }
    drawBox(painter, frame, m_domain, QPen(Qt::white, 1));
    // Translucent slice planes overlay the wireframe so the user can see where
    // the XY/XZ/YZ slices sit in the domain.
    if (m_slicePlanesVisible) {
        for (int axis = 0; axis < 3; ++axis) {
            drawSlicePlane(painter, frame, axis);
        }
    }
    drawAxisIndicator(painter);
}

QPointF IsoWidget::project(const ViewportFrame& frame,
    double x, double y, double z) const
{
    Real3 point;
    point[0] = x;
    point[1] = y;
    point[2] = z;
    const auto projected = projectPoint(m_camera, frame, m_domain, point);
    return QPointF(projected.x, projected.y);
}

void IsoWidget::drawBox(QPainter& painter, const ViewportFrame& frame,
    const RealBox& box, const QPen& pen) const
{
    std::array<QPointF, 8> corners;
    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
        const auto x = (corner & 1U) != 0U ? box.upper[0] : box.lower[0];
        const auto y = (corner & 2U) != 0U ? box.upper[1] : box.lower[1];
        const auto z = (corner & 4U) != 0U ? box.upper[2] : box.lower[2];
        corners[corner] = project(frame, x, y, z);
    }
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    for (const auto& edge : boxEdges) {
        painter.drawLine(corners[static_cast<std::size_t>(edge[0])],
            corners[static_cast<std::size_t>(edge[1])]);
    }
}

void IsoWidget::drawSlicePlane(QPainter& painter, const ViewportFrame& frame,
    int axis) const
{
    const auto index = static_cast<std::size_t>(axis);
    const auto a = static_cast<std::size_t>((axis + 1) % 3);
    const auto b = static_cast<std::size_t>((axis + 2) % 3);
    const auto position = std::clamp(m_slicePositions[index],
        m_domain.lower[index], m_domain.upper[index]);
    QPolygonF polygon;
    // Visit corners in perimeter order (0,1,3,2); the natural 0,1,2,3 bit
    // order crosses the diagonals and fills a self-intersecting bowtie.
    for (const unsigned int corner : {0U, 1U, 3U, 2U}) {
        std::array<double, 3> point{};
        point[index] = position;
        point[a] = (corner & 1U) != 0U ? m_domain.upper[a] : m_domain.lower[a];
        point[b] = (corner & 2U) != 0U ? m_domain.upper[b] : m_domain.lower[b];
        polygon << project(frame, point[0], point[1], point[2]);
    }
    auto fill = slicePlaneColor(axis);
    fill.setAlpha(96);
    painter.setPen(QPen(slicePlaneColor(axis), 1));
    painter.setBrush(fill);
    painter.drawPolygon(polygon);
    painter.setBrush(Qt::NoBrush);
}

RealBox IsoWidget::physicalBox(const LevelBoxes& level, const IntBox& box) const
{
    RealBox physical;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto nodal = box.centering[axis] != 0;
        physical.lower[axis] = level.indexOrigin[axis]
            + (static_cast<double>(box.lower[axis]) - (nodal ? 0.5 : 0.0))
                * level.cellSize[axis];
        physical.upper[axis] = level.indexOrigin[axis]
            + (static_cast<double>(box.upper[axis]) + (nodal ? 0.5 : 1.0))
                * level.cellSize[axis];
    }
    return physical;
}

QColor IsoWidget::levelOutlineColor(int level) const
{
    // Same rule as the 2-D grid-box overlays: coarse white, finer levels
    // spread across the palette.
    if (level <= 0 || m_palette == nullptr) {
        return QColor(Qt::white);
    }
    const auto finest = std::max(static_cast<int>(m_levels.size()) - 1, level);
    return QColor::fromRgb(static_cast<QRgb>(
        m_palette->levelColor(level, finest)));
}

QColor IsoWidget::slicePlaneColor(int axis) const
{
    // The same palette slots the 2-D views use for their crosshair guides:
    // x -> 65, y -> 220, z -> 255.
    constexpr std::array<int, 3> paletteSlots{65, 220, 255};
    if (m_palette == nullptr) {
        return QColor(Qt::white);
    }
    return QColor::fromRgba(static_cast<QRgb>(
        m_palette->slotArgb(paletteSlots[static_cast<std::size_t>(axis)])));
}

void IsoWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_hasGeometry) {
        m_lastMousePos = event->pos();
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void IsoWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging) {
        const auto delta = event->pos() - m_lastMousePos;
        m_lastMousePos = event->pos();
        constexpr double sensitivity = 0.008;
        m_camera.azimuth -= static_cast<double>(delta.x()) * sensitivity;
        m_camera.elevation += static_cast<double>(delta.y()) * sensitivity;
        m_camera.elevation = std::clamp(
            m_camera.elevation, -pi / 2.0 + 0.01, pi / 2.0 - 0.01);
        update();
        emit cameraChanged();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void IsoWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        setCursor(Qt::ArrowCursor);
        emit interactionEnded();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void IsoWidget::wheelEvent(QWheelEvent* event)
{
    if (!m_hasGeometry) {
        QWidget::wheelEvent(event);
        return;
    }
    const auto vertical = event->angleDelta().y();
    if (vertical == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    constexpr double zoomStep = 1.15;
    const auto factor = vertical > 0 ? zoomStep : 1.0 / zoomStep;
    m_camera.zoom = std::clamp(m_camera.zoom * factor, 0.1, 10.0);
    update();
    emit cameraChanged();
    event->accept();
}

void IsoWidget::drawAxisIndicator(QPainter& painter) const
{
    constexpr int originX = 38;
    constexpr int originY = 28;
    constexpr int armLen = 24;

    const int h = height();
    const QPointF origin(static_cast<qreal>(originX),
        static_cast<qreal>(h - originY));

    const auto cosAz = std::cos(m_camera.azimuth);
    const auto sinAz = std::sin(m_camera.azimuth);
    const auto cosEl = std::cos(m_camera.elevation);
    const auto sinEl = std::sin(m_camera.elevation);

    auto projectDir = [&](double dx, double dy, double dz) -> QPointF {
        const auto x1 = dx * cosAz - dy * sinAz;
        const auto y1 = dx * sinAz + dy * cosAz;
        const auto y2 = y1 * cosEl - dz * sinEl;
        return QPointF(static_cast<double>(armLen) * x1,
            -static_cast<double>(armLen) * y2);
    };

    const auto xTip = origin + projectDir(1.0, 0.0, 0.0);
    const auto yTip = origin + projectDir(0.0, 1.0, 0.0);
    const auto zTip = origin + projectDir(0.0, 0.0, 1.0);

    const QColor xColor = slicePlaneColor(0);
    const QColor yColor = slicePlaneColor(1);
    const QColor zColor = slicePlaneColor(2);

    QFont font;
    font.setPointSize(11);
    font.setBold(true);

    const auto drawArm = [&](const QPointF& tip, const QColor& color,
            const QString& label) {
        QPen pen(color, 2);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.drawLine(origin.toPoint(), tip.toPoint());

        const auto dir = tip - origin;
        const auto len = std::hypot(dir.x(), dir.y());
        if (len < 1.0) return;
        const auto ux = dir.x() / len;
        const auto uy = dir.y() / len;

        painter.setFont(font);
        const auto fm = painter.fontMetrics();
        const QRectF labelRect(tip.x() + ux * 6.0 - 16.0,
            tip.y() + uy * 6.0 - fm.height() / 2.0, 32.0, fm.height());
        painter.drawText(labelRect, Qt::AlignCenter, label);
    };

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    drawArm(xTip, xColor, QStringLiteral("X"));
    drawArm(yTip, yColor, QStringLiteral("Y"));
    drawArm(zTip, zColor, QStringLiteral("Z"));
    painter.restore();
}

void IsoWidget::setViewAngles(double azimuth, double elevation)
{
    m_camera.azimuth = azimuth;
    m_camera.elevation = elevation;
    update();
    emit cameraChanged();
}

void IsoWidget::setCamera(const OrthoCamera& camera)
{
    if (camera == m_camera) {
        return;
    }
    m_camera = camera;
    update();
    emit cameraChanged();
}

void IsoWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutButtons();
}

void IsoWidget::layoutButtons()
{
    if (!m_btnXY) return;
    constexpr int btnW = 32;
    constexpr int btnH = 26;
    constexpr int gap = 4;
    const int y = height() - btnH - 6;
    const int totalW = btnW * 3 + gap * 2;
    int x = (width() - totalW) / 2;
    m_btnXY->move(x, y);        x += btnW + gap;
    m_btnXZ->move(x, y);        x += btnW + gap;
    m_btnYZ->move(x, y);
}

} // namespace amrvis::qt
