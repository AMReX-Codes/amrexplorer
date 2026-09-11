#include "ImageView.hpp"
#include "ScaleBar.hpp"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QScrollBar>
#include <QSizeF>
#include <QTransform>

#include <algorithm>
#include <cmath>
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

void sliceGuidesStayOnScreenButOutOfExports() {
    amrvis::qt::ImageView view;
    view.resize(450, 350);
    view.show();
    view.setImage(solidImage(400, 300));
    view.setGridBoxes({{QRectF(30, 30, 100, 100), Qt::yellow, {}}});
    QApplication::processEvents();
    const auto displayWithoutGuides = view.viewport()->grab().toImage();
    const auto nativeExport = view.composedImage();
    const auto scaledExport = view.composedImage(QSize(800, 600));

    view.setCrosshairs(QLineF(200, 0, 200, 300), QLineF(0, 150, 400, 150), Qt::red, Qt::cyan);
    QApplication::processEvents();
    const auto displayWithGuides = view.viewport()->grab().toImage();
    require(displayWithGuides != displayWithoutGuides,
            "slice-guide test did not paint visible guides in the display window");
    require(view.composedImage() == nativeExport,
            "slice-position guides appeared in the image export");
    require(view.composedImage(QSize(800, 600)) == scaledExport,
            "slice-position guides appeared in the fixed-size animation frame");
    QApplication::processEvents();
    require(view.viewport()->grab().toImage() == displayWithGuides,
            "export did not restore the on-screen slice-position guides");
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

// Move the pointer over a scene point and report which tile and raster pixel
// the view named, or -1 when it stayed silent.
struct TileProbe {
    int tile = -1;
    int x = -1;
    int y = -1;
};

TileProbe probeAt(amrvis::qt::ImageView& view, const QPointF& scenePoint)
{
    TileProbe probe;
    const auto connection = QObject::connect(&view,
        &amrvis::qt::ImageView::tileProbeMoved, &view,
        [&probe](int tile, int x, int y) { probe = {tile, x, y}; });
    const auto position = QPointF(view.mapFromScene(scenePoint));
    QMouseEvent event(QEvent::MouseMove, position,
        view.viewport()->mapToGlobal(position), Qt::NoButton, Qt::NoButton,
        Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &event);
    QObject::disconnect(connection);
    return probe;
}

void tilesShareOnePlacedScene()
{
    amrvis::qt::ImageView view;
    view.resize(400, 400);
    view.show();
    QApplication::processEvents();
    // An upper 100x50 raster over the top of a 100x90 canvas and a lower
    // 60x40 raster under it, offset 20 units in x: the shape of two datasets
    // stacked along z and aligned along x.
    view.setTileImage(0, solidImage(100, 50), QRectF(0.0, 0.0, 100.0, 50.0),
        QRectF(0.0, 0.0, 100.0, 90.0), amrvis::qt::ImageTransformPolicy::GeometryAware);
    view.setTileImage(1, solidImage(60, 40), QRectF(20.0, 50.0, 60.0, 40.0));
    require(view.tileCount() == 2 && view.hasTileImage(0) && view.hasTileImage(1),
        "two tiles were not installed");
    require(view.tileSceneRect(1) == QRectF(20.0, 50.0, 60.0, 40.0),
        "the second tile did not take its scene rect");
    require(view.displaySize() == QSizeF(100.0, 90.0),
        "displaySize is not the tiles' footprint");
    require(view.sceneRect() == QRectF(0.0, 0.0, 100.0, 90.0),
        "the canvas rect did not become the scene rect");
    QApplication::processEvents();
    // Fit frames the footprint: both tiles are inside the viewport.
    const auto footprint = view.mapFromScene(QRectF(0.0, 0.0, 100.0, 90.0)).boundingRect();
    require(view.viewport()->rect().contains(footprint),
        "Fit did not frame both tiles");

    // The pointer names the tile it is over, in that tile's raster pixels.
    auto probe = probeAt(view, QPointF(50.5, 70.5));
    require(probe.tile == 1 && probe.x == 30 && probe.y == 20,
        "a point over the lower tile was not reported in its pixels");
    probe = probeAt(view, QPointF(50.5, 25.5));
    require(probe.tile == 0 && probe.x == 50 && probe.y == 25,
        "a point over the upper tile was not reported in its pixels");
    // A hidden tile is not hit; the point falls outside the other tile.
    view.setTileVisible(1, false);
    probe = probeAt(view, QPointF(50.5, 70.5));
    require(probe.tile == -1, "a hidden tile still took the pointer");
    view.setTileVisible(1, true);

    // Overlays belong to their tile: replacing one raster keeps the other's.
    view.setGridBoxes({{QRectF(0.0, 0.0, 10.0, 10.0), Qt::yellow, {}}}, 1);
    view.setGridBoxes({{QRectF(0.0, 0.0, 10.0, 10.0), Qt::yellow, {}}}, 0);
    require(view.gridBoxCount() == 2, "grid boxes were not installed per tile");
    view.setTileImage(0, solidImage(100, 50), QRectF(0.0, 0.0, 100.0, 50.0));
    require(view.gridBoxCount() == 1 && view.hasTileImage(1),
        "replacing the upper tile disturbed the lower one");

    // Removing the extra tile shrinks the list; the placeholder clears all.
    view.removeTile(1);
    require(view.tileCount() == 1 && view.hasTileImage(0),
        "removing the second tile did not leave the first alone");
    view.setTileImage(1, solidImage(60, 40), QRectF(20.0, 50.0, 60.0, 40.0));
    view.setPlaceholder(QStringLiteral("Loading dataset..."));
    require(view.tileCount() == 1 && !view.hasImage(),
        "the placeholder left a tile behind");
    // And setImage is the single-tile form again.
    view.setTileImage(1, solidImage(60, 40), QRectF(20.0, 50.0, 60.0, 40.0));
    view.setImage(solidImage(8, 8));
    require(view.tileCount() == 1 && view.image().size() == QSize(8, 8),
        "setImage did not reduce the view to one tile");
}

void clearingAnAbsentTileLeavesTheViewAlone()
{
    amrvis::qt::ImageView view;
    view.resize(400, 400);
    view.show();
    QApplication::processEvents();
    // A remote raster over a 40x20 cell window of a 100x100 domain: the tile
    // rect is in cells, the raster in pixels, so a footprint measurement
    // would differ from the raster size.
    view.setImage(solidImage(80, 40), amrvis::qt::ImageTransformPolicy::GeometryAware,
        {}, amrvis::qt::ImageView::VirtualPlacement{
            QRectF(10.0, 10.0, 40.0, 20.0), QSizeF(100.0, 100.0)});
    require(view.displaySize() == QSizeF(80.0, 40.0),
        "a placed single raster does not report its pixel size");
    // Clearing overlays on a tile that was never placed is a no-op.
    view.setCellHighlight(std::nullopt, 1);
    view.setCellHighlightPath(std::nullopt, 1);
    view.setGridBoxes({}, 1);
    view.setOverlaySegments({}, 1);
    view.setOverlayPaths({}, 1);
    view.setPointOverlays({}, 1);
    require(view.tileCount() == 1, "clearing an absent tile grew the tile list");
    require(view.displaySize() == QSizeF(80.0, 40.0),
        "clearing an absent tile changed displaySize");
}

void fitFramesTheCanvasNotTheTilesOnShow()
{
    amrvis::qt::ImageView view;
    view.resize(400, 400);
    view.show();
    QApplication::processEvents();
    // Two layers on a panel that shows one at a time: the canvas is their
    // union and Fit must frame it whichever tile is visible.
    view.setTileImage(0, solidImage(100, 50), QRectF(0.0, 0.0, 100.0, 50.0),
        QRectF(0.0, 0.0, 100.0, 50.0), amrvis::qt::ImageTransformPolicy::GeometryAware);
    view.setTileImage(1, solidImage(40, 50), QRectF(30.0, 0.0, 40.0, 50.0));
    QApplication::processEvents();
    const auto both = view.transform();
    view.setTileVisible(0, false);
    view.fitToWindow();
    require(view.transform() == both, "Fit re-framed the one tile on show");
    view.setTileVisible(0, true);
    view.setTileVisible(1, false);
    view.fitToWindow();
    require(view.transform() == both, "Fit re-framed the other tile on show");
}

} // namespace

bool nearly(double actual, double expected, double relative = 1e-9)
{
    return std::abs(actual - expected) <= relative * std::max(1.0, std::abs(expected));
}

void displayStretchLivesInTheViewTransform()
{
    amrvis::qt::ImageView view;
    view.resize(400, 400);
    view.show();
    QApplication::processEvents();
    view.setImage(solidImage(100, 50));
    QApplication::processEvents();
    const auto unstretched = view.transform();
    require(nearly(unstretched.m22() / unstretched.m11(), 1.0),
        "an unstretched Fit is not isotropic");

    // Fit: the 100x50 raster shown 1:4 is 100x200 in display units, so the
    // horizontal axis binds and the vertical is four times as tall.
    view.setDisplayStretch(1.0, 4.0);
    const auto fitted = view.transform();
    require(nearly(fitted.m22() / fitted.m11(), 4.0),
        "Fit did not apply the vertical stretch");
    require(fitted.m11() < unstretched.m11(),
        "the stretched Fit did not shrink to keep the raster inside the view");
    require(nearly(view.isotropicScale(), fitted.m11()),
        "the isotropic scale is not m11 when the horizontal stretch is one");
    require(view.displaySize() == QSizeF(100.0, 200.0),
        "displaySize does not multiply the raster by the stretch");
    require(view.composedImageSize(1.0) == QSize(100, 200),
        "the export size ignores the stretch");
    // A footprint past the cap on its own is still capped, aspect kept.
    {
        amrvis::qt::ImageView tall;
        tall.setImage(solidImage(1024, 1024));
        tall.setDisplayStretch(1.0, 1000.0);
        const auto capped = tall.composedImageSize(1.0);
        require(capped.height() == 8192 && capped.width() == 8,
            "a stretched export footprint escaped the size cap");
    }
    // The raster still fits: both device-space extents are within the view.
    const auto footprint
        = view.mapFromScene(view.imageSceneRect()).boundingRect();
    require(footprint.width() <= 400 && footprint.height() <= 400,
        "the stretched raster overflows the viewport");

    // Fixed scale: N is pixels per raster pixel along the less stretched axis.
    view.setFixedScale(2);
    require(nearly(view.transform().m11(), 2.0)
            && nearly(view.transform().m22(), 8.0),
        "a fixed scale did not multiply each axis by its stretch");
    require(nearly(view.isotropicScale(), 2.0),
        "isotropicScale is not the fixed factor");

    // Custom: rubber-band zoom keeps the ratio too.
    view.zoomToRect(QRectF(10.0, 10.0, 20.0, 20.0));
    require(view.transformMode() == amrvis::qt::ImageView::TransformMode::Custom,
        "zoomToRect left the view out of Custom mode");
    require(nearly(view.transform().m22() / view.transform().m11(), 4.0),
        "zoomToRect flattened the stretch");
    // Changing the stretch in Custom mode keeps the zoom and the centre (the
    // raster must still overflow the viewport on both axes for the centre to
    // be the view's to keep; a smaller raster is centred by alignment).
    const auto zoom = view.isotropicScale();
    const auto centre
        = view.mapToScene(view.viewport()->rect()).boundingRect().center();
    view.setDisplayStretch(1.0, 2.0);
    require(nearly(view.transform().m22() / view.transform().m11(), 2.0),
        "a Custom-mode stretch change did not update the ratio");
    require(nearly(view.isotropicScale(), zoom),
        "a Custom-mode stretch change altered the zoom");
    const auto after
        = view.mapToScene(view.viewport()->rect()).boundingRect().center();
    require(std::abs(after.x() - centre.x()) < 1.0
            && std::abs(after.y() - centre.y()) < 1.0,
        "a Custom-mode stretch change moved the viewport centre");

    // An equal stretch is a no-op on the transform, and bad values mean one.
    const auto before = view.transform();
    view.setDisplayStretch(1.0, 2.0);
    require(before == view.transform(), "an equal stretch touched the view");
    view.setDisplayStretch(0.0, -3.0);
    require(view.displayStretch() == QPointF(1.0, 1.0),
        "non-positive stretch factors were not treated as one");
}

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    displayStretchLivesInTheViewTransform();
    scaleBarUsesNativeOrExplicitUnits();
    scaleBarIsPaintedOverTheSlice();
    scaleBarIsPaintedIntoExportedComposition();
    sliceGuidesStayOnScreenButOutOfExports();
    fixedExportSizePreservesLandmarks();
    scrollBarPanFollowsContentDelta();
    fullyVisibleSceneIgnoresPan();
    arrowKeysRequestPanOnlyWhenFocusedWithAnImage();
    tearingDownTheSceneForgetsThePointTally();
    tilesShareOnePlacedScene();
    clearingAnAbsentTileLeavesTheViewAlone();
    fitFramesTheCanvasNotTheTilesOnShow();
    return 0;
}
