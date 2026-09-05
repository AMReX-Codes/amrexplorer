#include "ImageView.hpp"
#include "ScaleBar.hpp"

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

void scaleBarUsesNativeOrExplicitUnits()
{
    constexpr double au = 1.495978707e13;
    constexpr double pc = 3.0856775814913673e18;

    const auto native = amrvis::qt::chooseScaleBar(8.0, 2.4, 120.0);
    require(native && native->label == "2.000e+00 code units",
        "an unset unit did not preserve the native scale in scientific notation");

    const auto centimetres = amrvis::qt::chooseScaleBar(8.0e12, 2.4e12,
        120.0, amrvis::qt::LengthUnit::Centimetre);
    require(centimetres && centimetres->label == "2e+07 km",
        "an explicit centimetre scale did not convert to a natural unit");

    const auto astronomical = amrvis::qt::chooseScaleBar(8.0 * au,
        2.4 * au, 120.0, amrvis::qt::LengthUnit::Centimetre);
    require(astronomical && astronomical->label == "2 AU",
        "an AU-scale view did not use AU");

    const auto parsecs = amrvis::qt::chooseScaleBar(1.0 * pc,
        0.26 * pc, 130.0, amrvis::qt::LengthUnit::Centimetre);
    require(parsecs && parsecs->label == "0.2 pc",
        "a parsec-scale view did not use pc");

    const auto kiloparsecs = amrvis::qt::chooseScaleBar(4.0e3 * pc,
        1.2e3 * pc, 120.0, amrvis::qt::LengthUnit::Centimetre);
    require(kiloparsecs && kiloparsecs->label == "1 kpc",
        "a kiloparsec-scale view did not use kpc");

    require(!amrvis::qt::chooseScaleBar(0.0, 1.0, 100.0),
        "a zero-width view produced a scale bar");
}

void scaleBarIsPaintedOverTheSlice()
{
    amrvis::qt::ImageView view;
    view.resize(400, 300);
    view.show();
    view.setImage(solidImage(400, 300));
    QApplication::processEvents();
    const QImage withoutBar = view.viewport()->grab().toImage();

    constexpr double pc = 3.0856775814913673e18;
    view.setScaleBarWidth(4.0 * pc, amrvis::qt::LengthUnit::Centimetre);
    QApplication::processEvents();
    const QImage withBar = view.viewport()->grab().toImage();

    require(withBar.size() == withoutBar.size(),
        "painting the scale bar changed the viewport size");
    int changed = 0;
    for (int y = 0; y < withBar.height(); ++y) {
        for (int x = withBar.width() / 2; x < withBar.width(); ++x) {
            changed += withBar.pixel(x, y) != withoutBar.pixel(x, y) ? 1 : 0;
        }
    }
    require(changed > 50,
        "setting a physical width did not paint a visible scale bar");
}

void scaleBarIsPaintedIntoExportedComposition()
{
    amrvis::qt::ImageView view;
    view.setImage(solidImage(400, 300));
    const QImage withoutBar = view.composedImage();

    constexpr double pc = 3.0856775814913673e18;
    view.setScaleBarWidth(4.0 * pc, amrvis::qt::LengthUnit::Centimetre);
    const QImage withBar = view.composedImage();

    require(withBar.size() == withoutBar.size(),
        "painting the scale bar changed the export size");
    int changed = 0;
    for (int y = 0; y < withBar.height(); ++y) {
        for (int x = withBar.width() / 2; x < withBar.width(); ++x) {
            changed += withBar.pixel(x, y) != withoutBar.pixel(x, y) ? 1 : 0;
        }
    }
    require(changed > 50,
        "the exported composition omitted the visible scale bar");
}

void fixedExportSizePreservesLandmarks() {
    amrvis::qt::ImageView view;
    QImage first(400, 300, QImage::Format_RGB32);
    first.fill(Qt::blue);
    {
        QPainter painter(&first);
        painter.fillRect(100, 75, 40, 30, Qt::yellow);
    }
    view.setImage(first);
    const auto before = view.composedImage(QSize(600, 450));
    for (const auto size : {QSize(600, 450), QSize(2400, 1800)}) {
        const auto image = view.composedImage(size);
        for (int y = 0; y < image.height(); ++y) {
            require(image.pixelColor(0, y) == QColor(Qt::blue),
                    "export introduced a gap at the left raster edge");
        }
        for (int x = 0; x < image.width(); ++x) {
            require(image.pixelColor(x, 0) == QColor(Qt::blue),
                    "export introduced a gap at the top raster edge");
        }
    }
    view.setImage(first.scaled(800, 600));
    const auto after = view.composedImage(QSize(600, 450));
    require(before.size() == after.size(), "fixed export size followed the source resolution");
    require(before.pixelColor(170, 125) == QColor(Qt::yellow) &&
                after.pixelColor(170, 125) == QColor(Qt::yellow) &&
                before.pixelColor(140, 100) == QColor(Qt::blue) &&
                after.pixelColor(140, 100) == QColor(Qt::blue),
            "a source-resolution change moved an exported landmark");
    require(view.composedImage(QSize()).isNull(), "empty export dimensions were accepted");

    QImage coarse(17, 31, QImage::Format_RGB32);
    coarse.fill(Qt::blue);
    view.setImage(coarse);
    view.setGridBoxes(
        {{QRectF(0, 0, 17, 31), Qt::white, {}}, {QRectF(4, 6, 7, 12), Qt::white, {}}});
    for (const bool placed : {false, true}) {
        if (placed) {
            view.setVirtualCanvas(amrvis::qt::ImageView::VirtualPlacement{
                QRectF(3.25, 7.5, 8.5, 15.5), QSizeF(128, 256)});
        }
        for (const auto size : {QSize(270, 540), QSize(541, 541), QSize(1082, 1082)}) {
            const auto original = view.composedImage(size);
            require(original.pixelColor(0, 0) == QColor(Qt::white),
                    "white boundary-grid strip reproducer did not exercise the original gap");
            const auto image = view.composedImage(size, nullptr, true);
            for (int y = 0; y < image.height(); ++y) {
                require(image.pixelColor(0, y) == QColor(Qt::blue),
                        "coarse/placed raster left an export strip on the left");
            }
            for (int x = 0; x < image.width(); ++x) {
                require(image.pixelColor(x, 0) == QColor(Qt::blue),
                        "coarse/placed raster left an export strip at the top");
            }
            const auto interior = QRect(2, 2, size.width() - 4, size.height() - 4);
            require(image.copy(interior) == original.copy(interior),
                    "suppressing outer grid strokes changed interior data or grid lines");
            require(view.composedImage(size) == original,
                    "export failed to restore normal grid rendering");
        }
    }
}

// A scene larger than the viewport pans by its scroll bars, and the delta says
// how far the *content* moves: content travelling +x backs the bar off by the
// same amount. MainWindow's arrow-key step and its drag handler both hand
// panViewport a delta in that convention, so a sign slip here silently inverts
// the arrow keys in exactly one display mode.
void scrollBarPanFollowsContentDelta()
{
    amrvis::qt::ImageView view;
    // A local view reports its own scrolling and resizing. canvasScrolled does
    // not: it fires only over a virtual canvas, so the volume window's region
    // of interest -- which is read off visibleImageRect() -- was wired to a
    // signal that never came for a local fixed-scale view, and stopped
    // following the viewport the moment you touched a scroll bar.
    {
        amrvis::qt::ImageView local;
        local.resize(200, 150);
        local.show();
        QApplication::processEvents();
        local.setImage(solidImage(800, 600));
        local.setFixedScale(2);
        QApplication::processEvents();
        int moved = 0;
        int scrolled = 0;
        QObject::connect(&local, &amrvis::qt::ImageView::viewportMoved,
            &local, [&moved] { ++moved; });
        QObject::connect(&local, &amrvis::qt::ImageView::canvasScrolled,
            &local, [&scrolled] { ++scrolled; });
        auto* bar = local.horizontalScrollBar();
        require(bar->maximum() > 0,
            "the fixed-scale view did not scroll, so this proves nothing");
        bar->setValue(bar->maximum() / 2);
        QApplication::processEvents();
        require(moved > 0, "a local scroll reported no viewport movement");
        require(scrolled == 0,
            "canvasScrolled fired without a virtual canvas, so it is not the "
            "signal this test says it is");
    }

    // And a resize on its own, in the arrangement that needs it: a scrolled
    // view sitting at offset zero, grown so that more of the raster shows
    // without either bar moving. Fit mode cannot exercise this -- it always
    // shows the whole raster, so its region does not change with the window --
    // and a view scrolled away from zero reports the resize through its bars
    // instead, proving nothing about the resize itself.
    {
        amrvis::qt::ImageView grown;
        grown.resize(200, 150);
        grown.show();
        QApplication::processEvents();
        grown.setImage(solidImage(800, 600));
        grown.setFixedScale(2);
        QApplication::processEvents();
        // Put the bars at their minima rather than asserting they are there:
        // where setFixedScale leaves them is the platform's business, and on
        // macOS it is not zero, which failed this before it reached the resize
        // it exists to check. At the minimum, growing the view cannot move a
        // bar -- a clamp can only hold it where it is.
        grown.horizontalScrollBar()->setValue(
            grown.horizontalScrollBar()->minimum());
        grown.verticalScrollBar()->setValue(
            grown.verticalScrollBar()->minimum());
        QApplication::processEvents();
        const auto before = grown.visibleImageRect();
        int moved = 0;
        QObject::connect(&grown, &amrvis::qt::ImageView::viewportMoved,
            &grown, [&moved] { ++moved; });
        grown.resize(360, 280);
        QApplication::processEvents();
        require(grown.visibleImageRect() != before,
            "growing the view did not change what is visible, so there is "
            "nothing here to report");
        require(moved > 0, "a viewport resize reported no viewport movement");
    }

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

// Every teardown that drops the point items has to drop their tally with
// them. setImage and setPointOverlays do; setPlaceholder is the third one,
// and a stale tally there outlives the scene it counted -- the particle
// overlay accessors then answer for a view that is showing "Loading...".
// The tally only: m_pointOverlayColors has the same gap in setPlaceholder,
// which predates this and is not fixed here.
void tearingDownTheSceneForgetsThePointTally()
{
    amrvis::qt::ImageView view;
    view.setImage(solidImage(16, 16));
    amrvis::qt::PointOverlay overlay;
    overlay.points = {{1.0, 1.0}, {2.0, 2.0}, {3.0, 3.0}};
    overlay.color = Qt::red;
    overlay.size = 2.0F;
    view.setPointOverlays({overlay});
    require(view.pointOverlayCount() == 1 && view.pointOverlayPointCount() == 3,
        "the point overlay was not installed");

    view.setPlaceholder(QStringLiteral("Loading dataset..."));
    require(view.pointOverlayCount() == 0,
        "the placeholder left the point items behind");
    require(view.pointOverlayPointCount() == 0,
        "the placeholder left the point tally behind");

    // The other two teardowns, so the three cannot drift apart.
    view.setImage(solidImage(16, 16));
    view.setPointOverlays({overlay});
    view.setImage(solidImage(16, 16));
    require(view.pointOverlayPointCount() == 0,
        "a new image left the point tally behind");
    view.setPointOverlays({overlay});
    view.setPointOverlays({});
    require(view.pointOverlayPointCount() == 0,
        "replacing the overlays left the point tally behind");
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    scaleBarUsesNativeOrExplicitUnits();
    scaleBarIsPaintedOverTheSlice();
    scaleBarIsPaintedIntoExportedComposition();
    fixedExportSizePreservesLandmarks();
    scrollBarPanFollowsContentDelta();
    fullyVisibleSceneIgnoresPan();
    arrowKeysRequestPanOnlyWhenFocusedWithAnImage();
    tearingDownTheSceneForgetsThePointTally();
    return 0;
}
