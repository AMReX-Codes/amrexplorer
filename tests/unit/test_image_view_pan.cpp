#include "ImageView.hpp"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QPoint>
#include <QPointF>
#include <QScrollBar>
#include <QTransform>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

QImage solidImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::black);
    return image;
}

// A scene larger than the viewport pans by its scroll bars, and the delta says
// how far the *content* moves: content travelling +x backs the bar off by the
// same amount. MainWindow's arrow-key step and its drag handler both hand
// panViewport a delta in that convention, so a sign slip here silently inverts
// the arrow keys in exactly one display mode.
void scrollBarPanFollowsContentDelta()
{
    amrvis::qt::ImageView view;
    view.resize(200, 150);
    view.show();
    QApplication::processEvents();
    view.setImage(solidImage(800, 600));
    view.setFixedScale(2);
    QApplication::processEvents();

    auto* const hBar = view.horizontalScrollBar();
    auto* const vBar = view.verticalScrollBar();
    require(hBar->maximum() > hBar->minimum(),
        "a 2x scale of an 800px raster must overflow a 200px viewport");
    require(vBar->maximum() > vBar->minimum(),
        "a 2x scale of a 600px raster must overflow a 150px viewport");

    // Park both bars mid-range so neither end clamps the step under test.
    hBar->setValue((hBar->minimum() + hBar->maximum()) / 2);
    vBar->setValue((vBar->minimum() + vBar->maximum()) / 2);
    const auto startX = hBar->value();
    const auto startY = vBar->value();

    view.panViewport(QPoint(10, 6));
    require(hBar->value() == startX - 10,
        "content moving +x must decrease the horizontal scroll value by 10");
    require(vBar->value() == startY - 6,
        "content moving +y must decrease the vertical scroll value by 6");
    require(view.transformMode()
            == amrvis::qt::ImageView::TransformMode::FixedScale,
        "scrolling must leave the display mode untouched");
}

// With the whole scene visible there is nowhere to scroll, so a pan is a
// no-op. Translating instead would slide the image off-centre and demote the
// mode to Custom without telling MainWindow, leaving the Scale button stale.
void fullyVisibleSceneIgnoresPan()
{
    amrvis::qt::ImageView view;
    view.resize(400, 300);
    view.show();
    QApplication::processEvents();
    view.setImage(solidImage(50, 40));
    view.fitToWindow();
    QApplication::processEvents();

    auto* const hBar = view.horizontalScrollBar();
    auto* const vBar = view.verticalScrollBar();
    require(hBar->maximum() == hBar->minimum()
            && vBar->maximum() == vBar->minimum(),
        "a fitted raster must leave both scroll bars without range");

    const auto before = view.transform();
    view.panViewport(QPoint(25, 25));
    require(view.transform() == before,
        "a fully visible scene must not be translated by a pan");
    require(view.transformMode() == amrvis::qt::ImageView::TransformMode::Fit,
        "panning must not demote Fit to Custom");
}

// The arrow keys pan the view that has focus, and only that view. They used to
// be window-wide QShortcuts, which took Up/Down from every spin box and combo
// in the toolbars -- Qt line edits claim Left/Right through ShortcutOverride
// but not Up/Down, and non-editable combos claim no arrows at all -- so a
// keyboard user stepping the Z position panned the image instead. Handling the
// keys in the view means only the focused widget receives them.
//
// What this covers is the handler's own gates: an image-less view stays
// silent, and a modified arrow belongs to whoever else wants it. It does not
// cover the routing, and cannot: sendEvent delivers straight to the view, so
// the setFocus below only makes the widget a plausible recipient rather than
// proving anything about focus. The routing is Qt's, not ours -- keyPressEvent
// has no focus test to get wrong -- and the window-level consequence is
// covered end to end by qt_arrow_key_routing_smoke, which sends its keys to
// whatever holds focus instead.
void arrowKeysRequestPanOnlyWhenFocusedWithAnImage()
{
    amrvis::qt::ImageView view;
    view.resize(200, 150);
    view.show();
    QApplication::processEvents();

    std::vector<QPointF> requested;
    QObject::connect(&view, &amrvis::qt::ImageView::panStepRequested,
        [&requested](const QPointF& direction) {
            requested.push_back(direction);
        });

    const auto press = [&view](::Qt::Key key,
                           ::Qt::KeyboardModifiers modifiers
                           = ::Qt::NoModifier) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers);
        QApplication::sendEvent(&view, &event);
    };

    // No image yet: nothing to pan, so nothing is requested.
    press(::Qt::Key_Left);
    require(requested.empty(),
        "an arrow key on an empty view must not request a pan");

    view.setImage(solidImage(800, 600));
    view.setFixedScale(2);
    view.setFocus();
    QApplication::processEvents();

    press(::Qt::Key_Left);
    press(::Qt::Key_Right);
    press(::Qt::Key_Up);
    press(::Qt::Key_Down);
    require(requested.size() == 4, "each arrow key must request one pan step");
    // Left scrolls the content right, and Up scrolls it up: the same convention
    // panViewport takes above.
    require(requested[0] == QPointF(1.0, 0.0), "Left must pan content +x");
    require(requested[1] == QPointF(-1.0, 0.0), "Right must pan content -x");
    require(requested[2] == QPointF(0.0, 1.0), "Up must pan content +y");
    require(requested[3] == QPointF(0.0, -1.0), "Down must pan content -y");

    requested.clear();
    press(::Qt::Key_Up, ::Qt::ControlModifier);
    press(::Qt::Key_Down, ::Qt::ShiftModifier);
    require(requested.empty(),
        "a modified arrow key must not be claimed as a pan");

    // macOS stamps KeypadModifier on the arrow keys -- Qt documents them as
    // part of the keypad -- so a gate testing against NoModifier alone leaves
    // panning dead there while passing everywhere else. Every case above
    // builds its own events, so only an explicit case covers it.
    requested.clear();
    press(::Qt::Key_Left, ::Qt::KeypadModifier);
    press(::Qt::Key_Down, ::Qt::KeypadModifier);
    require(requested.size() == 2,
        "a keypad-flagged arrow key must still pan (macOS sets this)");
    require(requested[0] == QPointF(1.0, 0.0)
            && requested[1] == QPointF(0.0, -1.0),
        "a keypad-flagged arrow key panned the wrong way");

    // ...but the mask must not swallow a real modifier that happens to arrive
    // with the keypad flag.
    requested.clear();
    press(::Qt::Key_Up, ::Qt::KeypadModifier | ::Qt::ControlModifier);
    require(requested.empty(),
        "Ctrl with the keypad flag must not be claimed as a pan");
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    scrollBarPanFollowsContentDelta();
    fullyVisibleSceneIgnoresPan();
    arrowKeysRequestPanOnlyWhenFocusedWithAnImage();
    return 0;
}
