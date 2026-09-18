#include "LinePlotWindow.hpp"
#include "NavigationHistory.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool value, const char* message)
{
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void mouse(QWidget* target, QEvent::Type type, QPoint position,
    Qt::MouseButton button, Qt::MouseButtons buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, QPointF(position), QPointF(target->mapToGlobal(position)),
        button, buttons, modifiers);
    QApplication::sendEvent(target, &event);
}
void escape(QWidget* target)
{
    QKeyEvent event(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}
void history()
{
    amrvis::qt::NavigationHistory<int> h;
    h.push(0, 1);
    h.push(1, 2);
    require(h.back()->before == 1 && h.back()->before == 0, "history back ordering");
    require(!h.canBack() && h.forward()->after == 1, "history forward ordering");
    h.push(1, 1);
    require(h.canForward(), "no-op discarded forward history");
    h.push(1, 3);
    require(!h.canForward() && h.back()->after == 3, "history branch truncation");
    h.clear();
    for (int i = 0; i < 110; ++i) h.push(i, i + 1);
    require(h.size() == 100, "history capacity");
    int earliest = 0;
    while (const auto* entry = h.back()) earliest = entry->before;
    require(earliest == 10, "history evicted the wrong end");
}
void lineGestures()
{
    amrvis::qt::LinePlotWindow window("navigation");
    amrvis::qt::LinePlotCurve curve;
    curve.fieldName = "density";
    curve.line.positions = {0.0, 1.0, 2.0};
    curve.line.values = {10.0, 20.0, 30.0};
    curve.line.valid = {1, 1, 1};
    window.addCurve(curve);
    window.show();
    QApplication::processEvents();
    auto* plot = window.findChild<amrvis::qt::LinePlotWidget*>();
    const auto rect = plot->plotRect();
    const QPoint from = rect.topLeft() + QPoint(rect.width() / 4, rect.height() / 4);
    const QPoint to = rect.bottomRight() - QPoint(rect.width() / 4, rect.height() / 4);
    const auto select = [&] {
        mouse(plot, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        mouse(plot, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    };
    select(); escape(plot);
    mouse(plot, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    require(!plot->canNavigateBack(), "canceled line zoom entered history");
    select();
    mouse(plot, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    require(plot->canNavigateBack(), "line zoom missing from history");
    plot->navigateBack();
    require(!plot->canNavigateBack() && plot->canNavigateForward(), "line back failed");
    plot->navigateForward();
    plot->resetZoom();
    require(plot->canNavigateBack(), "line reset was not reversible");
    plot->clearNavigation();
    require(!plot->canNavigateBack() && !plot->canNavigateForward(), "line clear kept history");
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    history(); lineGestures();
}
