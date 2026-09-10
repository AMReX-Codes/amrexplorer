#include "LinePlotWindow.hpp"
#include "RecordingPaintDevice.hpp"

#include <QAction>
#include <QApplication>
#include <QTest>
#include <QEvent>
#include <QFontMetrics>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPointF>
#include <QImage>
#include <QToolTip>

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <limits>
#include <utility>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    amrvis::qt::LinePlotWindow window(QStringLiteral("hover-test"));

    amrvis::qt::LinePlotCurve curve;
    curve.fieldName = "density";
    curve.lineAxis = 0;
    curve.line.positions = {0.0, 1.0, 2.0};
    curve.line.positionsAreIndices = true;
    curve.line.values = {10.0F, 20.0F, 30.0F};
    curve.line.valid = {1, 1, 1};
    window.addCurve(std::move(curve));
    window.show();
    application.processEvents();

    auto* plot = window.findChild<amrvis::qt::LinePlotWidget*>();
    require(plot != nullptr, "line plot canvas was not created");

    // The plot insets follow the tick labels the last paint measured, so ask
    // the widget where its data area is rather than assuming fixed margins.
    constexpr int bottomMargin = 36;
    const QRect dataRect = plot->plotRect();
    const QPoint hoverPosition(
        dataRect.left() + dataRect.width() / 2,
        dataRect.bottom() - dataRect.height() / 2);
    const auto globalPosition = plot->mapToGlobal(hoverPosition);
    QMouseEvent move(QEvent::MouseMove,
        QPointF(hoverPosition), QPointF(globalPosition),
        Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(plot, &move);
    application.processEvents();

    const auto text = QToolTip::text();
    require(text.contains(QStringLiteral("density")),
        "hover readout omitted the field name");
    require(text.contains(QStringLiteral("x = 1")),
        "hover readout did not format the integer position");
    require(text.contains(QStringLiteral("value = 20")),
        "hover readout omitted the sample value");

    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(plot, &leave);
    application.processEvents();
    // QToolTip fades asynchronously and the offscreen platform keeps reporting
    // it as visible during that fade; sending Leave still exercises cleanup.

    // Ctrl+W closes this window, as it does the main, volume and Dataset ones.
    // The window carries no menu bar, so the action has to be on the window
    // itself for the key to reach it; the sequence is spelled out here rather
    // than read back from the action, so moving the key fails this.
    auto* const closeAction = window.findChild<QAction*>(
        QStringLiteral("linePlotCloseAction"));
    require(closeAction != nullptr, "the line plot has no close action");
    require(window.actions().contains(closeAction),
        "the close action is not on the window, so Ctrl+W cannot reach it");
    require(closeAction->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_W),
        "the line plot close shortcut is not Ctrl+W");
    // Pressed, not triggered: QTest::keyClick goes through the platform's key
    // path and Qt's shortcut map, which is what a trigger() call skips -- and
    // skipping it hides a wrong shortcut context or a missing window
    // association, both of which leave the key dead in the app.
    window.activateWindow();
    QTest::keyClick(&window, Qt::Key_W, Qt::ControlModifier);
    require(!window.isVisible(), "Ctrl+W left the line plot window open");
    // Large finite samples must still paint and remain hoverable after zoom.
    // Test both signs, plus constants whose ordinary padding would overflow.
    amrvis::qt::LinePlotWidget extremePlot;
    extremePlot.resize(640, 480);
    extremePlot.setNumberFormat("%g");
    std::vector<amrvis::qt::LinePlotCurve> curves(1);
    curves[0].fieldName = "extreme";
    curves[0].color = Qt::red;
    curves[0].line.positions = {0.0, 1.0, 2.0};
    curves[0].line.valid = {1, 1, 1};
    extremePlot.setCurves(&curves);
    extremePlot.show();
    const auto requirePainted = [&] {
        const auto image = extremePlot.grab().toImage();
        const QRect painted = extremePlot.plotRect();
        int colored = 0;
        for (int y = painted.top(); y <= painted.bottom(); ++y) {
            for (int x = painted.left(); x <= painted.right(); ++x) {
                const auto pixel = image.pixelColor(x, y);
                colored += pixel.red() > 150 && pixel.green() < 80 && pixel.blue() < 80;
            }
        }
        require(colored > 100, "extreme finite values erased the plotted curve");
    };
    const auto largest = std::numeric_limits<double>::max();
    for (const auto& values : {std::vector{-1.0e308, 0.0, 1.0e308},
             std::vector{-largest, 0.0, largest}, std::vector{largest, largest, largest},
             std::vector{-largest, -largest, -largest}}) {
        curves[0].line.values = values;
        extremePlot.resetZoom();
        requirePainted();
    }
    curves[0].line.values = {-1.0e308, 0.0, 1.0e308};
    extremePlot.resetZoom();
    requirePainted();
    // Taken from the painted geometry each time: the same insets the hover
    // maps through, so the cursor lands on the midpoint sample itself.
    const auto centreOf = [](const QRect& area) {
        return QPoint(area.left() + area.width() / 2,
            area.bottom() - area.height() / 2);
    };
    const auto requireHover = [&] {
        const auto centre = centreOf(extremePlot.plotRect());
        QEvent clear(QEvent::Leave);
        QApplication::sendEvent(&extremePlot, &clear);
        QMouseEvent moveEvent(QEvent::MouseMove, QPointF(centre),
            QPointF(extremePlot.mapToGlobal(centre)),
            Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&extremePlot, &moveEvent);
        application.processEvents();
        require(QToolTip::text().contains("extreme")
                && QToolTip::text().contains("value = 0"),
            "extreme line range lost the midpoint hover readout");
    };
    requireHover();

    // Hover must use the range that was painted, including its label margins.
    // Change distant samples before the next repaint: a fresh extrema scan
    // would widen the margins and move the midpoint away from the cursor.
    extremePlot.resetZoom();
    const auto originalFont = extremePlot.font();
    auto largerFont = originalFont;
    largerFont.setPixelSize(24);
    extremePlot.setFont(largerFont);
    curves[0].line.values = {-1.0, 0.0, 1.0};
    requirePainted();
    QToolTip::hideText();
    curves[0].fieldName = "pending-range";
    curves[0].line.values = {-1.0e308, 0.0, 1.0e308};
    const auto pendingCentre = centreOf(extremePlot.plotRect());
    QMouseEvent pendingMove(QEvent::MouseMove, QPointF(pendingCentre),
        QPointF(extremePlot.mapToGlobal(pendingCentre)),
        Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&extremePlot, &pendingMove);
    require(QToolTip::text().contains("pending-range")
            && QToolTip::text().contains("value = 0"),
        "hover rescanned pending data instead of using the painted geometry");
    curves[0].fieldName = "extreme";
    extremePlot.setFont(originalFont);
    requirePainted();
    const QRect extremeRect = extremePlot.plotRect();
    const auto beforeZoom = extremePlot.grab().toImage().copy(extremeRect);
    const QPoint zoomCentre = centreOf(extremeRect);
    const QPoint inset(extremeRect.width() / 4, extremeRect.height() / 3);
    QTest::mousePress(&extremePlot, Qt::LeftButton, Qt::NoModifier, zoomCentre - inset);
    QTest::mouseRelease(&extremePlot, Qt::LeftButton, Qt::NoModifier, zoomCentre + inset);
    requirePainted();
    require(extremePlot.grab().toImage().copy(extremeRect) != beforeZoom,
        "zooming an extreme line range did not change the plotted geometry");
    requireHover();

    // Adaptive labels must keep their leading digits inside the widget.
    curves[0].line.values = {1.25663706212e-6, 1.256637062125e-6, 1.25663706213e-6};
    extremePlot.resetZoom();
    RecordingPaintDevice recording(extremePlot.width(), extremePlot.height());
    QPainter painter(&recording);
    extremePlot.render(&painter);
    painter.end();
    int preciseLabels = 0;
    for (const auto& label : recording.text()) {
        if (label.value.contains("e-06")) {
            ++preciseLabels;
            require(label.bounds.left() >= -0.5,
                "an adaptive y tick lost leading digits outside the plot widget");
        }
    }
    require(preciseLabels >= 5, "the paint probe did not see the adaptive axis labels");

    curves[0].line.positions = curves[0].line.values;
    extremePlot.resetZoom();
    RecordingPaintDevice bothAxes(extremePlot.width(), extremePlot.height());
    QPainter bothPainter(&bothAxes);
    extremePlot.render(&bothPainter);
    bothPainter.end();
    int horizontalLabels = 0;
    double previousRight = -1.0;
    for (const auto& label : bothAxes.text()) {
        if (label.bounds.top() > extremePlot.height() - bottomMargin
            && label.value.contains("e-06")) {
            ++horizontalLabels;
            require(label.bounds.left() >= previousRight
                    && label.bounds.right() <= extremePlot.width() + 0.5,
                "adaptive x labels overlap or extend outside the widget");
            previousRight = label.bounds.right();
        }
    }
    require(horizontalLabels >= 2, "adaptive x range lost its tick labels");

    // A larger font and a narrow window can leave less than one label's
    // width between the endpoints. Keep a readable tick in that case.
    auto narrowFont = extremePlot.font();
    narrowFont.setPixelSize(18);
    extremePlot.setFont(narrowFont);
    const double narrowLow = -1.2566370621234567e-200;
    double narrowHigh = narrowLow;
    for (int step = 0; step < 8; ++step) {
        narrowHigh = std::nextafter(narrowHigh, 0.0);
    }
    curves[0].line.positions = {narrowLow, std::lerp(narrowLow, narrowHigh, 0.5), narrowHigh};
    curves[0].line.values = curves[0].line.positions;
    extremePlot.resetZoom();
    // Sized from the label the painter measures rather than fixed pixels: a
    // runner without fonts draws box glyphs a full pixel size wide. Room for
    // both end labels first, then a hair too little for them.
    const int labelSpan = QFontMetrics(narrowFont).horizontalAdvance(
        QString::number(narrowLow, 'g', 17));
    for (const int width : {2 * labelSpan + 32, 2 * labelSpan - 44}) {
        extremePlot.resize(width, 400);
        RecordingPaintDevice narrowAxes(extremePlot.width(), extremePlot.height());
        QPainter narrowPainter(&narrowAxes);
        extremePlot.render(&narrowPainter);
        narrowPainter.end();
        int narrowLabels = 0;
        double right = -0.5;
        for (const auto& label : narrowAxes.text()) {
            if (label.bounds.top() > extremePlot.height() - bottomMargin
                && label.value.contains("e-200")) {
                ++narrowLabels;
                if (label.bounds.left() < right
                    || label.bounds.right() > extremePlot.width() + 0.5) {
                    std::cerr << "widget=" << extremePlot.width()
                              << " label=" << label.value.toStdString()
                              << " left=" << label.bounds.left()
                              << " right=" << label.bounds.right()
                              << " previous=" << right << '\n';
                }
                require(label.bounds.left() >= right
                        && label.bounds.right() <= extremePlot.width() + 0.5,
                    "narrow plot's adaptive x labels overlap or escape the widget");
                right = label.bounds.right();
            }
        }
        require(narrowLabels > 0, "narrow plot lost every x tick label");
    }

    return 0;
}
