#include "LinePlotWindow.hpp"

#include <QAction>
#include <QApplication>
#include <QTest>
#include <QEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPointF>
#include <QToolTip>

#include <cstdlib>
#include <iostream>
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

    constexpr int leftMargin = 92;
    constexpr int rightMargin = 32;
    constexpr int topMargin = 18;
    constexpr int bottomMargin = 36;
    const QRect plotRect(leftMargin, topMargin,
        plot->width() - leftMargin - rightMargin,
        plot->height() - topMargin - bottomMargin);
    const QPoint hoverPosition(
        plotRect.left() + plotRect.width() / 2,
        plotRect.bottom() - plotRect.height() / 2);
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
    return 0;
}
