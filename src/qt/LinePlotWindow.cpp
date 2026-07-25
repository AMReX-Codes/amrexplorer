#include "LinePlotWindow.hpp"
#include "NumberFormat.hpp"
#include "Theme.hpp"

#include <QCheckBox>
#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QIcon>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QRect>
#include <QRubberBand>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace amrvis::qt {
namespace {

const std::array<QColor, 8>& curveColors()
{
    static const std::array<QColor, 8> colors{
        QColor(Qt::red), QColor(Qt::green), QColor(Qt::cyan), QColor(Qt::yellow),
        QColor(Qt::magenta), QColor(Qt::white), QColor(Qt::gray),
        QColor(0x66, 0xB2, 0xFF)};
    return colors;
}

constexpr std::array<const char*, 3> axisLetters{"x", "y", "z"};

std::size_t sampleCount(const LinePlotCurve& curve)
{
    return std::min({curve.line.positions.size(), curve.line.values.size(),
        curve.line.valid.size()});
}

bool usesIndexPositions(const std::vector<LinePlotCurve>* curves)
{
    if (curves == nullptr) {
        return false;
    }
    auto foundVisible = false;
    for (const auto& curve : *curves) {
        if (!curve.visible) {
            continue;
        }
        foundVisible = true;
        if (!curve.line.positionsAreIndices) {
            return false;
        }
    }
    return foundVisible;
}

} // namespace

LinePlotWidget::LinePlotWidget(QWidget* parent)
    : QWidget(parent)
    , m_numberFormat(defaultNumberFormat())
{
    setMinimumSize(420, 300);
    setMouseTracking(true);
}

void LinePlotWidget::setCurves(const std::vector<LinePlotCurve>* curves)
{
    m_curves = curves;
    update();
}

void LinePlotWidget::setNumberFormat(QString format)
{
    m_numberFormat = std::move(format);
    update();
}

void LinePlotWidget::resetZoom()
{
    m_zoom.reset();
    update();
}

void LinePlotWidget::setShowMarkers(bool on)
{
    m_showMarkers = on;
    update();
}

QRect LinePlotWidget::plotRect() const
{
    constexpr int leftMargin = 92;
    constexpr int rightMargin = 32;
    constexpr int topMargin = 18;
    constexpr int bottomMargin = 36;
    return QRect(leftMargin, topMargin,
        std::max(width() - leftMargin - rightMargin, 16),
        std::max(height() - topMargin - bottomMargin, 16));
}

std::optional<QRectF> LinePlotWidget::automaticRange() const
{
    if (m_curves == nullptr) {
        return std::nullopt;
    }
    auto xMinimum = std::numeric_limits<double>::infinity();
    auto xMaximum = -std::numeric_limits<double>::infinity();
    auto yMinimum = std::numeric_limits<double>::infinity();
    auto yMaximum = -std::numeric_limits<double>::infinity();
    auto any = false;
    for (const auto& curve : *m_curves) {
        if (!curve.visible) {
            continue;
        }
        const auto count = sampleCount(curve);
        for (std::size_t sample = 0; sample < count; ++sample) {
            if (curve.line.valid[sample] == 0) {
                continue;
            }
            const auto position = curve.line.positions[sample];
            const auto value = static_cast<double>(curve.line.values[sample]);
            if (!std::isfinite(position) || !std::isfinite(value)) {
                continue;
            }
            any = true;
            xMinimum = std::min(xMinimum, position);
            xMaximum = std::max(xMaximum, position);
            yMinimum = std::min(yMinimum, value);
            yMaximum = std::max(yMaximum, value);
        }
    }
    if (!any) {
        return std::nullopt;
    }
    if (usesIndexPositions(m_curves)) {
        // Integer samples describe points, not cell edges. Half-index padding
        // centers the first and last points while leaving room for markers.
        xMinimum -= 0.5;
        xMaximum += 0.5;
    } else if (xMinimum == xMaximum) {
        const auto padding = std::max(std::abs(xMinimum), 1.0) * 1.0e-6;
        xMinimum -= padding;
        xMaximum += padding;
    }
    if (yMinimum == yMaximum) {
        const auto padding = std::max(std::abs(yMinimum), 1.0) * 1.0e-6;
        yMinimum -= padding;
        yMaximum += padding;
    }
    // Pad each physical/value axis so the data is not flush against the boundary.
    constexpr double padFraction = 0.05;
    const auto ySpan = yMaximum - yMinimum;
    if (!usesIndexPositions(m_curves)) {
        const auto xSpan = xMaximum - xMinimum;
        xMinimum -= padFraction * xSpan;
        xMaximum += padFraction * xSpan;
    }
    yMinimum -= padFraction * ySpan;
    yMaximum += padFraction * ySpan;
    return QRectF(QPointF(xMinimum, yMinimum), QPointF(xMaximum, yMaximum));
}

std::optional<QRectF> LinePlotWidget::displayedRange() const
{
    if (m_zoom.has_value()) {
        return m_zoom;
    }
    return automaticRange();
}

QString LinePlotWidget::hoverTextAt(const QPointF& position) const
{
    constexpr double hoverRadius = 10.0;
    const auto plot = plotRect();
    if (m_curves == nullptr || !m_paintedRange
        || !plot.contains(position.toPoint())) {
        return {};
    }
    const auto& range = *m_paintedRange;

    const auto mapX = [&](double value) {
        return plot.left()
            + (value - range.left()) / range.width() * plot.width();
    };
    const auto mapY = [&](double value) {
        return plot.bottom()
            - (value - range.top()) / range.height() * plot.height();
    };
    const auto cursorX = range.left()
        + (position.x() - plot.left()) / plot.width() * range.width();
    const auto dataRadius = hoverRadius * range.width() / plot.width();

    const LinePlotCurve* nearestCurve = nullptr;
    std::size_t nearestSample = 0;
    auto nearestDistanceSquared = hoverRadius * hoverRadius;
    constexpr std::size_t candidateRadius = 8;
    for (const auto& curve : *m_curves) {
        if (!curve.visible) {
            continue;
        }
        const auto count = sampleCount(curve);
        const auto begin = curve.line.positions.begin();
        const auto end = begin + static_cast<std::ptrdiff_t>(count);
        const auto insertion = static_cast<std::size_t>(std::distance(
            begin, std::lower_bound(begin, end, cursorX)));
        const auto first = insertion > candidateRadius
            ? insertion - candidateRadius : std::size_t{0};
        const auto last = insertion
            + std::min(count - insertion, candidateRadius + 1);
        for (auto sample = first; sample < last; ++sample) {
            const auto samplePosition = curve.line.positions[sample];
            const auto value = static_cast<double>(curve.line.values[sample]);
            if (curve.line.valid[sample] == 0
                || !std::isfinite(samplePosition) || !std::isfinite(value)) {
                continue;
            }
            const auto dx = mapX(samplePosition) - position.x();
            if (std::abs(dx) > hoverRadius
                || std::abs(samplePosition - cursorX) > dataRadius) {
                continue;
            }
            const auto dy = mapY(value) - position.y();
            const auto distanceSquared = dx * dx + dy * dy;
            if (distanceSquared <= nearestDistanceSquared) {
                nearestCurve = &curve;
                nearestSample = sample;
                nearestDistanceSquared = distanceSquared;
            }
        }
    }
    if (nearestCurve == nullptr) {
        return {};
    }

    const auto coordinate = nearestCurve->line.positions[nearestSample];
    const auto value = static_cast<double>(
        nearestCurve->line.values[nearestSample]);
    const auto coordinateText = nearestCurve->line.positionsAreIndices
        ? QString::number(static_cast<long long>(std::llround(coordinate)))
        : formatNumber(coordinate, m_numberFormat);
    const auto axis = nearestCurve->lineAxis >= 0
            && nearestCurve->lineAxis < static_cast<int>(axisLetters.size())
        ? QLatin1String(
              axisLetters[static_cast<std::size_t>(nearestCurve->lineAxis)])
        : QStringLiteral("x");
    return tr("%1\n%2 = %3\nvalue = %4")
        .arg(QString::fromStdString(nearestCurve->fieldName))
        .arg(axis)
        .arg(coordinateText)
        .arg(formatNumber(value, m_numberFormat));
}

void LinePlotWidget::hideHover()
{
    QToolTip::hideText();
}

void LinePlotWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), viewportBackground());
    const auto range = displayedRange();
    m_paintedRange = range;
    if (!range.has_value()) {
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter,
            tr("Shift+middle click or horizontal right drag for X, "
               "Shift+right click or vertical right drag for Y"));
        return;
    }
    const auto plot = plotRect();
    // Normalized data rect: left/right are x min/max, top/bottom y min/max.
    const auto xMinimum = range->left();
    const auto xMaximum = range->right();
    const auto yMinimum = range->top();
    const auto yMaximum = range->bottom();
    const auto mapX = [&](double value) {
        return plot.left() + (value - xMinimum) / (xMaximum - xMinimum) * plot.width();
    };
    const auto mapY = [&](double value) {
        return plot.bottom() - (value - yMinimum) / (yMaximum - yMinimum) * plot.height();
    };

    const QPen gridPen(QColor(96, 96, 96));
    constexpr int tickCount = 5;
    const auto drawXTick = [&](double value, const QString& label) {
        const auto x = mapX(value);
        painter.setPen(gridPen);
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(Qt::white);
        painter.drawText(QRectF(x - 40.0, plot.bottom() + 4.0, 80.0, 16.0),
            Qt::AlignHCenter | Qt::AlignTop, label);
    };
    if (usesIndexPositions(m_curves)) {
        const auto first = static_cast<long long>(std::ceil(xMinimum));
        const auto last = static_cast<long long>(std::floor(xMaximum));
        const auto span = std::max(last - first, 0LL);
        const auto step = std::max((span + tickCount - 2) / (tickCount - 1), 1LL);
        auto tick = first;
        while (tick <= last) {
            drawXTick(static_cast<double>(tick), QString::number(tick));
            if (tick > last - step) {
                break;
            }
            tick += step;
        }
    } else {
        for (int tick = 0; tick < tickCount; ++tick) {
            const auto fraction = static_cast<double>(tick) / (tickCount - 1);
            const auto xValue = xMinimum + fraction * (xMaximum - xMinimum);
            drawXTick(xValue, formatNumber(xValue, m_numberFormat));
        }
    }
    for (int tick = 0; tick < tickCount; ++tick) {
        const auto fraction = static_cast<double>(tick) / (tickCount - 1);
        const auto yValue = yMinimum + fraction * (yMaximum - yMinimum);
        const auto y = mapY(yValue);
        painter.setPen(gridPen);
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(Qt::white);
        painter.drawText(QRectF(0.0, y - 8.0, plot.left() - 6.0, 16.0),
            Qt::AlignRight | Qt::AlignVCenter,
            formatNumber(yValue, m_numberFormat));
    }
    painter.setPen(Qt::white);
    painter.drawRect(plot);

    if (m_curves == nullptr) {
        return;
    }
    painter.save();
    painter.setClipRect(plot);
    for (const auto& curve : *m_curves) {
        if (!curve.visible) {
            continue;
        }
        painter.setPen(QPen(curve.color));
        const auto count = sampleCount(curve);
        QPolygonF run;
        const auto flushRun = [&] {
            if (run.size() == 1) {
                painter.drawPoint(run.first());
            } else if (run.size() > 1) {
                painter.drawPolyline(run);
            }
            run.clear();
        };
        for (std::size_t sample = 0; sample < count; ++sample) {
            const auto position = curve.line.positions[sample];
            const auto value = static_cast<double>(curve.line.values[sample]);
            if (curve.line.valid[sample] == 0
                || !std::isfinite(position) || !std::isfinite(value)) {
                flushRun();
                continue;
            }
            run.append(QPointF(mapX(position), mapY(value)));
        }
        flushRun();
        if (m_showMarkers) {
            // One marker per original sample at a fixed pixel size, raised just
            // enough that the dot sits on the line (its lower edge touching),
            // so it reads against the line without floating above it. The lift
            // is in screen pixels, independent of the axis scaling/zoom.
            constexpr qreal markerDiameter = 3.5;
            constexpr qreal markerLift = markerDiameter / 2.0;
            QPen markerPen(curve.color);
            markerPen.setWidthF(markerDiameter);
            markerPen.setCapStyle(Qt::RoundCap);
            painter.setPen(markerPen);
            for (std::size_t sample = 0; sample < count; ++sample) {
                const auto position = curve.line.positions[sample];
                const auto value
                    = static_cast<double>(curve.line.values[sample]);
                if (curve.line.valid[sample] == 0
                    || !std::isfinite(position) || !std::isfinite(value)) {
                    continue;
                }
                painter.drawPoint(
                    QPointF(mapX(position), mapY(value) - markerLift));
            }
        }
    }
    painter.restore();
}

void LinePlotWidget::mousePressEvent(QMouseEvent* event)
{
    hideHover();
    if (event->button() == Qt::LeftButton
        && plotRect().contains(event->position().toPoint())) {
        m_pressPosition = event->position().toPoint();
        m_dragging = true;
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        resetZoom();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void LinePlotWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging) {
        hideHover();
        if (m_rubberBand == nullptr) {
            m_rubberBand = new QRubberBand(QRubberBand::Rectangle, this);
        }
        m_rubberBand->setGeometry(
            QRect(m_pressPosition, event->position().toPoint()).normalized());
        m_rubberBand->show();
        event->accept();
        return;
    }
    const auto text = hoverTextAt(event->position());
    if (text.isEmpty()) {
        hideHover();
    } else {
        QToolTip::showText(event->globalPosition().toPoint()
                + QPoint(12, 16),
            text, this, plotRect());
    }
    QWidget::mouseMoveEvent(event);
}

void LinePlotWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        if (m_rubberBand != nullptr) {
            m_rubberBand->hide();
            m_rubberBand->deleteLater();
            m_rubberBand = nullptr;
        }
        const auto dragged = QRect(m_pressPosition, event->position().toPoint())
            .normalized().intersected(plotRect());
        const auto base = displayedRange();
        if (base.has_value() && dragged.width() >= 4 && dragged.height() >= 4) {
            const auto plot = plotRect();
            const auto xMinimum = base->left()
                + static_cast<double>(dragged.left() - plot.left()) / plot.width()
                    * base->width();
            const auto xMaximum = base->left()
                + static_cast<double>(dragged.right() - plot.left()) / plot.width()
                    * base->width();
            const auto yMaximum = base->top()
                + static_cast<double>(plot.bottom() - dragged.top()) / plot.height()
                    * base->height();
            const auto yMinimum = base->top()
                + static_cast<double>(plot.bottom() - dragged.bottom()) / plot.height()
                    * base->height();
            m_zoom = QRectF(QPointF(xMinimum, yMinimum), QPointF(xMaximum, yMaximum))
                .normalized();
            update();
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void LinePlotWidget::leaveEvent(QEvent* event)
{
    hideHover();
    QWidget::leaveEvent(event);
}

LinePlotWindow::LinePlotWindow(const QString& datasetName, QWidget* parent)
    : QWidget(parent)
{
    setWindowTitle(tr("Line Plot — %1").arg(datasetName));
    resize(780, 480);

    m_plot = new LinePlotWidget(this);
    m_plot->setCurves(&m_curves);

    m_legend = new QListWidget(this);
    m_legend->setMaximumWidth(260);
    m_legend->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    auto* clearButton = new QPushButton(tr("Clear"), this);
    auto* zoomButton = new QPushButton(tr("Reset Zoom"), this);
    auto* markersBox = new QCheckBox(tr("Data Markers"), this);
    markersBox->setChecked(false);
    auto* closeButton = new QPushButton(tr("Close"), this);

    auto* sideLayout = new QVBoxLayout;
    sideLayout->addWidget(m_legend);
    sideLayout->addWidget(clearButton);
    sideLayout->addWidget(zoomButton);
    sideLayout->addWidget(markersBox);
    sideLayout->addStretch();
    sideLayout->addWidget(closeButton);

    auto* layout = new QHBoxLayout(this);
    layout->addWidget(m_plot, 1);
    layout->addLayout(sideLayout);

    connect(m_legend, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        const auto row = m_legend->row(item);
        if (row < 0 || static_cast<std::size_t>(row) >= m_curves.size()) {
            return;
        }
        m_curves[static_cast<std::size_t>(row)].visible
            = item->checkState() == Qt::Checked;
        m_plot->update();
    });
    connect(clearButton, &QPushButton::clicked, this, [this] { clearCurves(); });
    connect(zoomButton, &QPushButton::clicked, m_plot, &LinePlotWidget::resetZoom);
    connect(markersBox, &QCheckBox::toggled, m_plot, &LinePlotWidget::setShowMarkers);
    connect(closeButton, &QPushButton::clicked, this, &QWidget::close);
}

void LinePlotWindow::setNumberFormat(QString format)
{
    m_plot->setNumberFormat(std::move(format));
}

void LinePlotWindow::addCurve(LinePlotCurve curve)
{
    curve.color = curveColors()[m_addedCurves % curveColors().size()];
    ++m_addedCurves;
    curve.visible = true;
    m_curves.push_back(std::move(curve));
    const auto& added = m_curves.back();
    QPixmap swatch(14, 14);
    swatch.fill(added.color);
    auto* item = new QListWidgetItem(QIcon(swatch), curveDescription(added), m_legend);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Checked);
    if (added.dimension == 3) {
        item->setSizeHint(QSize(0, m_legend->fontMetrics().height() * 2 + 6));
    }
    m_plot->update();
}

QString LinePlotWindow::curveDescription(const LinePlotCurve& curve) const
{
    auto result = QString::fromStdString(curve.fieldName);
    if (curve.dimension == 3) {
        const auto indent = QString(result.size() + 1, QLatin1Char(' '));
        bool first = true;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis == curve.lineAxis) {
                continue;
            }
            result += (first ? QStringLiteral(" ") : QChar('\n') + indent)
                + tr("%1=%2")
                    .arg(QLatin1String(
                        axisLetters[static_cast<std::size_t>(axis)]))
                    .arg(curve.fixedCoordinates[static_cast<std::size_t>(axis)],
                        0, 'g', 6);
            first = false;
        }
    } else {
        const auto fixed = static_cast<std::size_t>(curve.primaryFixedAxis);
        result += tr(" %1=%2")
            .arg(QLatin1String(axisLetters[fixed]))
            .arg(curve.fixedCoordinates[fixed], 0, 'g', 6);
    }
    return result;
}

void LinePlotWindow::clearCurves()
{
    m_curves.clear();
    m_legend->clear();
    m_plot->resetZoom();
    m_plot->update();
}

} // namespace amrvis::qt
