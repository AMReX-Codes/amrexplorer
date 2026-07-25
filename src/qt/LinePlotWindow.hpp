#pragma once

#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Result.hpp>

#include <QColor>
#include <QPoint>
#include <QRectF>
#include <QWidget>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class QListWidget;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QRubberBand;

namespace amrvis::qt {

// One curve in the line plot window: a snapshot of a LineQuery result plus
// the request context needed for the legend text.
struct LinePlotCurve {
    LineResult line;
    std::string fieldName;
    int primaryFixedAxis = 1;  // in-plane fixed axis (from the cursor)
    int lineAxis = 0;          // axis the line varies along
    std::array<double, 3> fixedCoordinates{};
    int dimension = 2;
    int maximumLevel = 0;
    CompositionPolicy composition = CompositionPolicy::FinestAvailable;
    QColor color;
    bool visible = true;
};

// Custom paint widget drawing every visible curve white-on-black with axes,
// tick labels, and a light grid. Left-drag zooms to the dragged rect;
// right-click or resetZoom() restores the automatic range.
class LinePlotWidget final : public QWidget {
    Q_OBJECT

public:
    explicit LinePlotWidget(QWidget* parent = nullptr);

    void setCurves(const std::vector<LinePlotCurve>* curves);
    void setNumberFormat(QString format);
    void resetZoom();
    // Toggles per-sample data markers over each curve (legacy Amrvis style).
    void setShowMarkers(bool on);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    [[nodiscard]] QRect plotRect() const;
    [[nodiscard]] std::optional<QRectF> automaticRange() const;
    [[nodiscard]] std::optional<QRectF> displayedRange() const;
    [[nodiscard]] QString hoverTextAt(const QPointF& position) const;
    void hideHover();

    const std::vector<LinePlotCurve>* m_curves = nullptr;
    QString m_numberFormat;
    bool m_showMarkers = false;
    std::optional<QRectF> m_zoom;
    std::optional<QRectF> m_paintedRange;
    QPoint m_pressPosition;
    QRubberBand* m_rubberBand = nullptr;
    bool m_dragging = false;
};

// Legacy-style XY line plot window: plot area plus a side panel with a
// checkable legend (colored swatch + description per curve), a Data Markers
// toggle, and the Clear / Reset Zoom / Close buttons.
class LinePlotWindow final : public QWidget {
    Q_OBJECT

public:
    explicit LinePlotWindow(const QString& datasetName, QWidget* parent = nullptr);

    void addCurve(LinePlotCurve curve);
    // The printf-style format for the axis tick labels; the legend text and
    // the ASCII export keep their full-precision 'g' formatting.
    void setNumberFormat(QString format);

private:
    void clearCurves();
    [[nodiscard]] QString curveDescription(const LinePlotCurve& curve) const;

    LinePlotWidget* m_plot = nullptr;
    QListWidget* m_legend = nullptr;
    std::vector<LinePlotCurve> m_curves;
    std::size_t m_addedCurves = 0;
};

} // namespace amrvis::qt
