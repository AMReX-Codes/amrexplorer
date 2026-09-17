#include "ImageView.hpp"
#include "LinePlotWindow.hpp"

#include <QApplication>
#include <QFocusEvent>
#include <QGraphicsLineItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QRubberBand>

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
void imageGestures()
{
    using View = amrvis::qt::ImageView;
    View view;
    view.resize(500, 400);
    view.show();
    QImage image(200, 200, QImage::Format_RGB32);
    image.fill(Qt::black);
    view.setImage(image);
    QApplication::processEvents();
    int zooms = 0, probes = 0, lines = 0;
    Qt::MouseButton orientation = Qt::NoButton;
    QObject::connect(&view, &View::rubberBandSelected, [&] { ++zooms; });
    QObject::connect(&view, &View::probeClicked, [&] { ++probes; });
    QObject::connect(&view, &View::linePlotRequested,
        [&](int, int, Qt::MouseButton button) { ++lines; orientation = button; });
    const auto drag = [&](QPoint from, QPoint to, Qt::MouseButton button) {
        mouse(view.viewport(), QEvent::MouseButtonPress, from, button, button);
        mouse(view.viewport(), QEvent::MouseMove, to, Qt::NoButton, button);
    };
    const auto transform = view.transform();
    drag({100, 100}, {200, 200}, Qt::LeftButton);
    escape(&view);
    mouse(view.viewport(), QEvent::MouseMove, {230, 230}, Qt::NoButton, Qt::LeftButton);
    mouse(view.viewport(), QEvent::MouseButtonRelease, {230, 230}, Qt::LeftButton, Qt::NoButton);
    require(zooms == 0 && probes == 0 && view.transform() == transform,
        "Escape committed a canceled image selection");
    for (auto* band : view.findChildren<QRubberBand*>())
        require(!band->isVisible(), "Escape left the image rubber band visible");
    drag({100, 100}, {200, 200}, Qt::LeftButton);
    mouse(view.viewport(), QEvent::MouseButtonRelease, {200, 200}, Qt::LeftButton, Qt::NoButton);
    require(zooms == 1, "new selection failed after Escape");
    drag({100, 100}, {107, 100}, Qt::RightButton);
    mouse(view.viewport(), QEvent::MouseMove, {100, 180}, Qt::NoButton, Qt::RightButton);
    bool horizontalGuide = false;
    for (auto* item : view.scene()->items()) {
        if (auto* line = dynamic_cast<QGraphicsLineItem*>(item))
            horizontalGuide = horizontalGuide || line->line().dy() == 0.0;
    }
    require(horizontalGuide, "line preview changed direction after latching");
    mouse(view.viewport(), QEvent::MouseButtonRelease, {100, 100}, Qt::RightButton, Qt::NoButton);
    require(lines == 1 && orientation == Qt::MiddleButton,
        "returning to the origin lost the latched horizontal line");
    drag({100, 100}, {106, 106}, Qt::RightButton);
    mouse(view.viewport(), QEvent::MouseButtonRelease, {107, 107}, Qt::RightButton, Qt::NoButton);
    require(lines == 2 && orientation == Qt::RightButton, "diagonal threshold tie was not vertical");
    view.setLineOrientation(View::LineOrientation::Horizontal);
    mouse(view.viewport(), QEvent::MouseButtonPress, {100, 100}, Qt::RightButton,
        Qt::RightButton, Qt::ShiftModifier);
    mouse(view.viewport(), QEvent::MouseButtonRelease, {100, 100}, Qt::RightButton,
        Qt::NoButton, Qt::ShiftModifier);
    require(lines == 3 && orientation == Qt::MiddleButton, "explicit horizontal click failed");
    drag({100, 100}, {110, 100}, Qt::RightButton);
    escape(&view);
    mouse(view.viewport(), QEvent::MouseButtonRelease, {150, 100}, Qt::RightButton, Qt::NoButton);
    require(lines == 3, "Escape committed a line");
    drag({100, 100}, {200, 200}, Qt::LeftButton);
    QFocusEvent lost(QEvent::FocusOut);
    QApplication::sendEvent(&view, &lost);
    mouse(view.viewport(), QEvent::MouseButtonRelease, {200, 200}, Qt::LeftButton, Qt::NoButton);
    require(zooms == 1, "focus loss committed a selection");
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
    const auto before = plot->grab().toImage();
    select(); escape(plot);
    mouse(plot, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    require(plot->grab().toImage() == before, "Escape changed the line plot range");
    for (auto* band : plot->findChildren<QRubberBand*>())
        require(!band->isVisible(), "Escape left the line rubber band visible");
    select();
    QFocusEvent lost(QEvent::FocusOut);
    QApplication::sendEvent(plot, &lost);
    mouse(plot, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    require(plot->grab().toImage() == before, "focus loss changed the line plot range");
    select();
    mouse(plot, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    require(plot->grab().toImage() != before, "new line zoom failed after cancellation");
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    imageGestures(); lineGestures();
}
