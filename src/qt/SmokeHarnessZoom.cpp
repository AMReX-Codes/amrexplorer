#include "SmokeHarnessInternal.hpp"

#include "MainWindow.hpp"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// Zoom: the zoom / pan / scale scenarios: raster and rubber-band zoom, pan,
// the mapped-grid and spherical R-Z warps, fixed and effective scale and its
// report, arrow-key routing. Every branch drives MainWindow through its ForTest accessors and
// arms connections and timers for main() to run; see SmokeHarness.hpp.

namespace amrvis::qt::smoke {

Outcome dispatchZoom(Context& context)
{
    // The branches read these names as main() declared them; binding them
    // here keeps the moved code verbatim.
    auto& application = context.application;
    auto& window = context.window;
    const int argc = context.argc;
    char** argv = context.argv;

    if (argc == 3
        && std::string_view(argv[1]) == "--raster-zoom-smoke-test") {
        // 2-D Visible-range raster/color-bar consistency: after a full-domain
        // Visible slice caches the range, a zoom must re-render the raster
        // against that reused range (not the subregion's local range) so it
        // matches the color bar. See raster-colorbar-mismatch-on-2d-visible-zoom.
        // interactiveSlicesSettled fires twice: after the full-domain slice
        // (phase 0 -> zoom) and after the zoom (phase 1 -> verify). phase is a
        // shared_ptr so it outlives this branch's scope through exec().
        const std::filesystem::path path(argv[2]);
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                const auto* sync = window.findChild<QAction*>(
                    QStringLiteral("syncRubberBandZoomAction"));
                if (!success || sync == nullptr || sync->isVisible()) {
                    application.exit(1);
                    return;
                }
                window.enableVisibleRasterForTest();
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [&window, &application, phase] {
                if (*phase == 0) {
                    *phase = 1;
                    window.zoomActiveViewForTest();
                } else {
                    application.exit(
                        window.activeViewRasterMatchesDisplayRangeForTest()
                            ? 0 : 1);
                }
        });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--physical-aspect-smoke-test") {
        // View > Aspect Ratio on a dataset whose cells are 64 times taller
        // than wide (plotfile_2d_tall). Cell Counts draws one square pixel
        // per cell and withholds the scale bar; Physical Size stretches the
        // vertical axis by the cell aspect in the view transform, leaving the
        // raster itself at the cell aspect, and the scale bar becomes
        // truthful; an unequal axis factor takes it away again; a fixed scale
        // states its factor along the less stretched axis.
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                const auto near = [](double actual, double expected) {
                    return std::abs(actual - expected) <= 0.02 * expected;
                };
                if (!success) {
                    application.exit(2);
                    return;
                }
                if (!window.aspectMenuEnabledForTest()
                    || !near(window.activeViewStretchRatioForTest(), 1.0)
                    || !window.activeViewRasterHasCellAspectForTest()
                    || window.scaleBarActionEnabledForTest()) {
                    qCritical("Cell Counts did not start square and barless");
                    application.exit(1);
                    return;
                }
                auto* physical = window.findChild<QAction*>(
                    QStringLiteral("aspectPhysicalSizeAction"));
                if (physical == nullptr || !physical->isEnabled()) {
                    qCritical("Physical Size is not offered for a plotfile");
                    application.exit(1);
                    return;
                }
                physical->trigger();
                if (!near(window.activeViewStretchRatioForTest(), 64.0)
                    || !window.activeViewRasterHasCellAspectForTest()
                    || !window.scaleBarActionEnabledForTest()) {
                    qCritical("Physical Size did not stretch the view by the "
                              "cell aspect with the raster left alone");
                    application.exit(1);
                    return;
                }
                window.setAxisScaleForTest({2.0, 1.0, 1.0});
                if (!near(window.activeViewStretchRatioForTest(), 32.0)
                    || window.scaleBarActionEnabledForTest()) {
                    qCritical("an axis factor did not rescale one axis and "
                              "withdraw the scale bar");
                    application.exit(1);
                    return;
                }
                window.selectFixedScaleForTest(2);
                if (!window.fixedScaleStateMatchesForTest(2)
                    || !near(window.activeViewStretchRatioForTest(), 32.0)) {
                    qCritical("a fixed scale under a stretch is not the factor "
                              "along the less stretched axis");
                    application.exit(1);
                    return;
                }
                window.setAspectModeForTest(amrvis::qt::AspectMode::CellCounts);
                // Cells again, but X still doubled: half as tall as wide.
                application.exit(
                    near(window.activeViewStretchRatioForTest(), 0.5)
                        && window.fixedScaleStateMatchesForTest(2)
                        ? 0 : 1);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--mapped-grid-smoke-test") {
        // View > Mapped Grid on plotfile_3d_mapped (4^3 cells, dx = 0.25,
        // Nu_nd lifting the bottom nodes by nu_z = 0.125*(1 - k/4)*(i+j)/8):
        // off by default with the menu offered. On, each panel's warp is drawn
        // for the window it shows at its own pixels: at Fit the tile is the
        // canvas (Physical Size pinned, probe, volume ROI, boxes); a wheel
        // zoom and a synced rubber band zoom the views and the warps follow
        // without a re-slice; a scroll moves the window and its arrival leaves
        // the view put; contours keep the probe; off returns the logical
        // grid, and on again with a cosmetic change ends on the warp.
        const std::filesystem::path path(argv[2]);
        struct Progress {
            int phase = 0;
            std::array<double, 3> positionsBefore{};
            std::array<QRectF, 3> fitWindows;  // per normal, at Fit
            double fitScale = 0.0;
            QRectF zoomWindow;
            std::array<double, 4> scrolled{};
            QRectF scrolledFrom;
            QRectF tileBefore;
            std::array<int, 2> flatSize{};
        };
        auto progress = std::make_shared<Progress>();
        const auto fail = [&application](const char* message) {
            qCritical("%s", message);
            application.exit(1);
        };
        // A panel shows a warp drawn for its window (physical, x the first
        // panel axis) one to one: as many pixels as its tile covers on screen,
        // at whole device pixels.
        const auto drawnForScreen = [&window](int normal) {
            const auto panel = window.mappedPanelForTest(normal);
            const auto& tile = panel.tileDevice;
            const auto whole = [](double value) {
                return std::abs(value - std::round(value)) < 1e-6;
            };
            const bool ok = panel.warped && !panel.window.isEmpty()
                && std::abs(tile.width() - panel.image.width()) < 1e-6
                && std::abs(tile.height() - panel.image.height()) < 1e-6
                && whole(tile.left()) && whole(tile.top());
            if (!ok) {
                qCritical("panel %d: warp %d x %d on a tile of %g x %g device pixels "
                          "at (%g, %g)", normal, panel.image.width(),
                    panel.image.height(), tile.width(), tile.height(), tile.left(),
                    tile.top());
            }
            return ok;
        };
        // A panel's tile covers its whole canvas, reaching past each edge by
        // less than a device pixel (the window is whole device pixels).
        const auto coversCanvas = [&window](int normal) {
            const auto panel = window.mappedPanelForTest(normal);
            const auto pixel = 1.0 / (panel.scale * window.devicePixelRatioF());
            const auto& tile = panel.tile;
            const auto& canvas = panel.canvas;
            const bool ok = !canvas.isEmpty()
                && tile.left() <= canvas.left() + 1e-9 && tile.left() > canvas.left() - pixel
                && tile.top() <= canvas.top() + 1e-9 && tile.top() > canvas.top() - pixel
                && tile.right() >= canvas.right() - 1e-9
                && tile.right() < canvas.right() + pixel
                && tile.bottom() >= canvas.bottom() - 1e-9
                && tile.bottom() < canvas.bottom() + pixel;
            if (!ok) {
                qCritical("panel %d: tile %g x %g at (%g, %g), canvas %g x %g at (%g, %g)",
                    normal, tile.width(), tile.height(), tile.left(), tile.top(),
                    canvas.width(), canvas.height(), canvas.left(), canvas.top());
            }
            return ok;
        };
        const auto probeCentre = [&window] {
            const auto size = window.activeViewImageSizeForTest();
            const auto readout
                = window.probeReadoutActiveViewForTest(size[0] / 2, size[1] / 2);
            return readout.contains(QStringLiteral("value"))
                && readout.contains(QStringLiteral("cell"));
        };
        // A panel frames a physical target as a zoom to it does: all of it on
        // screen, filling the viewport along one axis, give or take the Fit
        // margin and the scroll bars the zoom raises. The target is placed in
        // the scene through the tile, which spans the window the warp was
        // drawn for.
        const auto frames = [&window](int normal, const QRectF& target) {
            const auto panel = window.mappedPanelForTest(normal);
            const auto& shown = panel.window;
            const auto& tile = panel.tile;
            const auto& visible = panel.visible;
            if (shown.isEmpty() || tile.isEmpty() || visible.isEmpty()
                || !(panel.scale > 0.0)) {
                return false;
            }
            // Physical (x, up) to scene (x, down).
            const auto toScene = [&shown, &tile](double x, double y) {
                return QPointF(
                    tile.left() + (x - shown.left()) * tile.width() / shown.width(),
                    tile.top() + (shown.bottom() - y) * tile.height() / shown.height());
            };
            const QRectF wanted(toScene(target.left(), target.bottom()),
                toScene(target.right(), target.top()));
            const auto slack = static_cast<double>(
                QApplication::style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 6)
                / panel.scale;
            const bool covers = visible.left() <= wanted.left() + slack
                && visible.right() >= wanted.right() - slack
                && visible.top() <= wanted.top() + slack
                && visible.bottom() >= wanted.bottom() - slack;
            const bool fills = std::abs(visible.width() - wanted.width()) <= slack
                || std::abs(visible.height() - wanted.height()) <= slack;
            if (!covers || !fills) {
                qCritical("panel %d shows [%g, %g] x [%g, %g] for [%g, %g] x [%g, %g]",
                    normal, visible.left(), visible.right(), visible.top(),
                    visible.bottom(), wanted.left(), wanted.right(), wanted.top(),
                    wanted.bottom());
            }
            return covers && fills;
        };
        const auto runPhase = [&window, &application, progress, fail, coversCanvas,
                                  drawnForScreen, probeCentre, frames] {
            // A settle can leave the view's own ask for its window queued
            // behind it: read the state once no slice is on its way.
            if (window.sliceRequestPendingForTest()
                || window.slicesInFlightForTest() > 0) {
                return;
            }
            // The x-z panel stays the active one: making another panel active
            // resizes both views (the active border).
            window.setActiveViewForTest(1);
            const auto size = window.activeViewImageSizeForTest();
            switch (progress->phase) {
            case 0: {
                // On: every panel shows the warp of its whole canvas.
                for (const int normal : {0, 1, 2}) {
                    if (!drawnForScreen(normal) || !coversCanvas(normal)) {
                        fail("at Fit a mapped panel is not the warp of its canvas");
                        return;
                    }
                    progress->fitWindows[static_cast<std::size_t>(normal)]
                        = window.mappedPanelForTest(normal).window;
                }
                if (!window.displayIsMappedForTest() || size[0] <= 16 || size[1] <= 16) {
                    qCritical("mapped pixmap %d x %d", size[0], size[1]);
                    fail("the mapped grid did not replace the raster");
                    return;
                }
                if (window.aspectMenuCheckedModeForTest()
                        != amrvis::qt::AspectMode::PhysicalSize
                    || window.aspectRadiosEnabledForTest()
                    || window.aspectModePreferenceForTest()
                        != amrvis::qt::AspectMode::CellCounts
                    || !window.aspectMenuEnabledForTest()
                    || !window.scaleBarActionEnabledForTest()) {
                    fail("mapped display did not pin Physical Size with "
                         "the preference kept and the scale bar offered");
                    return;
                }
                // Bottom-left: the x = 0 nodes are never lifted, so the lowest
                // row there holds cell (0, 0); bottom-right: the x = 1 bottom
                // node is lifted by about 0.06 above the canvas bottom, so
                // nothing is drawn there.
                const auto left = window.probeReadoutActiveViewForTest(0, size[1] - 1);
                const auto right = window.probeReadoutActiveViewForTest(
                    size[0] - 1, size[1] - 1);
                if (!left.contains(QStringLiteral("value"))
                    || !left.contains(QStringLiteral("cell"))
                    || right != QObject::tr("no data")) {
                    qCritical("left '%s' right '%s'", qPrintable(left), qPrintable(right));
                    fail("the probe does not follow the warp");
                    return;
                }
                // The volume's visible-region box is logical: with the whole
                // warp on screen it is the whole domain, bottom included,
                // although the lowest drawn node sits above it.
                const auto roi = window.volumeRegionOfInterestForTest();
                if (std::abs(roi.lower[2]) > 1e-9 || std::abs(roi.upper[2] - 1.0) > 1e-9
                    || std::abs(roi.lower[0]) > 1e-9 || std::abs(roi.upper[0] - 1.0) > 1e-9) {
                    qCritical("volume ROI z [%g, %g] x [%g, %g]", roi.lower[2],
                        roi.upper[2], roi.lower[0], roi.upper[0]);
                    fail("the volume region of interest followed the warp, "
                         "not the logical grid");
                    return;
                }
                if (window.activeViewGridBoxCountForTest() == 0) {
                    fail("the grid boxes are missing from the warp");
                    return;
                }
                progress->fitScale = window.activeViewTransformAndScrollForTest()[0];
                progress->phase = 1;
                for (int notch = 0; notch < 3; ++notch) {
                    window.wheelActiveViewForTest(1);
                }
                break;
            }
            case 1: {
                // Three notches in: the view zoomed over the canvas and the
                // warp is the part of it on screen. The plane is native, so
                // nothing was re-sliced.
                const auto xz = window.mappedPanelForTest(1);
                const bool smaller = xz.tile.width() < xz.canvas.width() - 1e-6
                    || xz.tile.height() < xz.canvas.height() - 1e-6;
                // The window may reach a device pixel past the canvas edges.
                const auto margin = 1.0 / (xz.scale * window.devicePixelRatioF());
                if (!drawnForScreen(1) || xz.fit || xz.resliced
                    || std::abs(xz.scale / progress->fitScale - 1.15 * 1.15 * 1.15) > 1e-6
                    || !smaller
                    || !xz.canvas.adjusted(-margin, -margin, margin, margin).contains(xz.tile)
                    || !probeCentre() || window.activeViewGridBoxCountForTest() == 0) {
                    qCritical("scale %g over %g, tile %gx%g of canvas %gx%g", xz.scale,
                        progress->fitScale, xz.tile.width(), xz.tile.height(),
                        xz.canvas.width(), xz.canvas.height());
                    fail("a wheel zoom did not draw the warp for the window it shows");
                    return;
                }
                progress->zoomWindow = xz.window;
                progress->phase = 2;
                window.rubberBandZoomActiveViewForTest();
                break;
            }
            case 2: {
                // The rubber band took the central half of that window: the
                // view frames it and the warp follows, still without a
                // re-slice. Sync is on by default: the y-z panel frames the
                // selection's z over its whole y, the x-y panel its x over its
                // whole y.
                const auto& zoom = progress->zoomWindow;
                const QRectF selection(zoom.left() + 0.25 * zoom.width(),
                    zoom.top() + 0.25 * zoom.height(), 0.5 * zoom.width(),
                    0.5 * zoom.height());
                const auto xz = window.mappedPanelForTest(1);
                if (!drawnForScreen(1) || xz.fit || xz.resliced || !probeCentre()
                    || !frames(1, selection)) {
                    fail("a mapped rubber band did not frame its selection");
                    return;
                }
                const auto& yz = progress->fitWindows[0];
                const auto& xy = progress->fitWindows[2];
                const std::array<std::pair<int, QRectF>, 2> synced{
                    std::pair{0,
                        QRectF(QPointF(yz.left(), std::max(yz.top(), selection.top())),
                            QPointF(yz.right(), std::min(yz.bottom(), selection.bottom())))},
                    std::pair{2,
                        QRectF(QPointF(std::max(xy.left(), selection.left()), xy.top()),
                            QPointF(std::min(xy.right(), selection.right()), xy.bottom()))}};
                for (const auto& [normal, target] : synced) {
                    const auto panel = window.mappedPanelForTest(normal);
                    if (!drawnForScreen(normal) || panel.fit || panel.resliced
                        || !frames(normal, target)) {
                        qCritical("panel %d", normal);
                        fail("a synced panel did not frame the selection along "
                             "the axis it shares");
                        return;
                    }
                }
                const auto before = window.activeViewTransformAndScrollForTest();
                progress->scrolledFrom = xz.window;
                progress->tileBefore = xz.tile;
                window.scrollActiveViewForTest(-24, -24);
                progress->scrolled = window.activeViewTransformAndScrollForTest();
                if (progress->scrolled == before) {
                    fail("the zoomed x-z panel had nowhere to scroll");
                    return;
                }
                progress->phase = 3;
                break;
            }
            case 3: {
                // The scroll moved the window; the warp drawn for it landed
                // with the view exactly where the scroll left it.
                const auto now = window.activeViewTransformAndScrollForTest();
                if (now != progress->scrolled) {
                    qCritical("scale %g, %g scroll %g, %g; scrolled to %g, %g scroll %g, %g",
                        now[0], now[1], now[2], now[3], progress->scrolled[0],
                        progress->scrolled[1], progress->scrolled[2],
                        progress->scrolled[3]);
                    fail("a mapped arrival moved the view");
                    return;
                }
                const auto xz = window.mappedPanelForTest(1);
                if (!drawnForScreen(1) || xz.resliced || xz.window == progress->scrolledFrom
                    || xz.tile == progress->tileBefore || !probeCentre()) {
                    fail("a scroll did not draw the warp for the window it moved to");
                    return;
                }
                progress->phase = 4;
                window.setDisplayModeForTest(amrvis::DisplayMode::RasterContours, 3);
                break;
            }
            case 4:
            case 5:
            case 6: {
                // Into contours (a fresh slice), a new contour count (the
                // cache path with the pixmap kept, rasterUnchanged), then the
                // raster again: the probe reads the cells under it and the
                // view stays where it is throughout.
                if (!window.activeViewIsMappedForTest() || !probeCentre()
                    || window.activeViewTransformAndScrollForTest() != progress->scrolled) {
                    qCritical("phase %d", progress->phase);
                    fail("a contour refresh lost the mapped source index or "
                         "moved the view");
                    return;
                }
                if (progress->phase == 4) {
                    window.setDisplayModeForTest(amrvis::DisplayMode::RasterContours, 7);
                } else if (progress->phase == 5) {
                    window.setDisplayModeForTest(amrvis::DisplayMode::Raster, 7);
                } else {
                    window.setMappedGridForTest(false);
                }
                ++progress->phase;
                break;
            }
            case 7: {
                // Off: the logical grid again, radios back.
                if (window.displayIsMappedForTest()
                    || window.activeViewIsMappedForTest()
                    || !window.aspectRadiosEnabledForTest()
                    || window.aspectMenuCheckedModeForTest()
                        != amrvis::qt::AspectMode::CellCounts) {
                    fail("switching the mapped grid off did not restore "
                         "the logical grid and the aspect radios");
                    return;
                }
                // The iso wireframe follows Axis Scaling: the unit cube's
                // outline becomes four times as tall as it is wide.
                window.setAxisScaleForTest({1.0, 1.0, 4.0});
                const auto iso = window.isoDomainDisplayBoxForTest();
                const auto isoWidth = iso.upper[0] - iso.lower[0];
                const auto isoHeight = iso.upper[2] - iso.lower[2];
                if (!(isoWidth > 0.0) || std::abs(isoHeight / isoWidth - 4.0) > 1e-9) {
                    qCritical("iso domain %g x %g", isoWidth, isoHeight);
                    fail("the iso wireframe did not follow the z axis factor");
                    return;
                }
                window.setAxisScaleForTest({1.0, 1.0, 1.0});
                const auto reset = window.isoDomainDisplayBoxForTest();
                if (std::abs((reset.upper[2] - reset.lower[2]) / isoWidth - 1.0) > 1e-9) {
                    fail("resetting Axis Scaling did not restore the iso cube");
                    return;
                }
                // On again with a cosmetic change in the same turn: the pixmap
                // must end up the warp with its source index, not the logical
                // raster flagged mapped.
                progress->flatSize = size;
                progress->phase = 8;
                window.setMappedGridForTest(true);
                window.setDisplayModeForTest(amrvis::DisplayMode::RasterContours, 5);
                break;
            }
            case 8: {
                // The toggle refitted: the warp of the whole canvas, far larger
                // than the few-cell raster, with the probe reading its cells.
                const auto left = window.probeReadoutActiveViewForTest(0, size[1] - 1);
                if (!drawnForScreen(1) || !coversCanvas(1)
                    || size[0] < 2 * progress->flatSize[0]
                    || size[1] < 2 * progress->flatSize[1]
                    || !left.contains(QStringLiteral("value"))
                    || !left.contains(QStringLiteral("cell"))) {
                    qCritical("phase 8: %d x %d over flat %d x %d, probe '%s'",
                        size[0], size[1], progress->flatSize[0],
                        progress->flatSize[1], qPrintable(left));
                    fail("a refresh overtaking the mapped toggle left a "
                         "flat pixmap flagged mapped");
                    return;
                }
                // A plain right click on the warp moves the other two slices
                // to the cell under it (the line tool is off there, which
                // must not swallow the click): from the tile's upper-left
                // quarter point, x lands on a cell centre below the middle
                // and z on one above it.
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    progress->positionsBefore[axis]
                        = window.slicePositionForTest(static_cast<int>(axis));
                }
                const auto tile = window.mappedPanelForTest(1).tileDevice;
                const auto ratio = window.devicePixelRatioF();
                window.rightClickActiveViewForTest(QPoint(
                    static_cast<int>((tile.left() + 0.25 * tile.width()) / ratio),
                    static_cast<int>((tile.top() + 0.25 * tile.height()) / ratio)));
                // A fixed scale on the warp has no raster clamp to report:
                // the button says the plain factor, not "2x→2x".
                window.selectToolbarFixedScaleForTest(2);
                if (window.scaleUiLabelForTest() != QStringLiteral("2x")
                    || window.effectiveFixedScaleForTest(2) != 0.0) {
                    qCritical("scale label '%s'", qPrintable(window.scaleUiLabelForTest()));
                    fail("a mapped view reported a clamped fixed scale");
                    return;
                }
                progress->phase = 9;
                window.setDisplayModeForTest(amrvis::DisplayMode::Raster, 7);
                window.setMappedGridForTest(false);
                break;
            }
            default: {
                const auto x = window.slicePositionForTest(0);
                const auto z = window.slicePositionForTest(2);
                const auto onCentre = [](double value) {
                    // 4 cells of 0.25 over [0, 1]: centres at 0.125 + k/4.
                    const auto k = (value - 0.125) / 0.25;
                    return std::abs(k - std::round(k)) < 1e-9;
                };
                if (x == progress->positionsBefore[0] || z == progress->positionsBefore[2]
                    || !onCentre(x) || !onCentre(z) || x >= 0.5 || z <= 0.5) {
                    qCritical("slice positions x %g z %g", x, z);
                    fail("a right click on the warp did not move the slices to "
                         "the clicked cell");
                    return;
                }
                application.exit(window.activeViewIsMappedForTest()
                        || window.displayIsMappedForTest() ? 3 : 0);
                break;
            }
            }
        };
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, fail](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                window.setActiveViewForTest(1);  // the x-z panel
                if (!window.mappedGridMenuEnabledForTest()
                    || window.displayIsMappedForTest()
                    || window.activeViewIsMappedForTest()
                    || window.aspectMenuCheckedModeForTest()
                        != amrvis::qt::AspectMode::CellCounts
                    || !window.aspectRadiosEnabledForTest()) {
                    fail("Mapped Grid is not offered, or is on by default");
                    return;
                }
                window.setMappedGridForTest(true);
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [&window, runPhase] {
                QTimer::singleShot(0, &window, runPhase);
            });
        QTimer::singleShot(60000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window, [&window, path] {
            window.setGridBoxesVisibleForTest(true);
            window.openDataset(path);
        });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--spherical-rz-smoke-test") {
        // The R-Z view of plotfile_2d_spherical (16 x 8 cells, r in [1, 2],
        // theta in [0, 0.5]) drawn the mapped-grid way: at Fit the warp of
        // the sector's box at the view's own pixels, with the probe reading a
        // cell inside the sector and nothing outside it, and the grid boxes
        // on; a wheel zoom and a rubber band zoom the view and the warp
        // follows without a re-slice; a scroll moves the window and its
        // arrival leaves the view put; a contour change overtaking the
        // redraw of a second scroll still ends on the warp of the new window;
        // contours keep the probe; r-theta is the flat raster again at Fit,
        // and R-Z once more lands on the canvas.
        const std::filesystem::path path(argv[2]);
        struct Progress {
            int phase = 0;
            double fitScale = 0.0;
            QRectF zoomWindow;
            std::array<double, 4> scrolled{};
            QRectF scrolledFrom;
            QRectF tileBefore;
            bool contoursSent = false;
        };
        auto progress = std::make_shared<Progress>();
        const auto fail = [&application](const char* message) {
            qCritical("%s", message);
            application.exit(1);
        };
        // The view shows a warp drawn for its window one to one: as many
        // pixels as its tile covers on screen, at whole device pixels.
        const auto drawnForScreen = [&window] {
            const auto panel = window.mappedPanelForTest(-1);
            const auto& tile = panel.tileDevice;
            const auto whole = [](double value) {
                return std::abs(value - std::round(value)) < 1e-6;
            };
            const bool ok = panel.warped && !panel.window.isEmpty()
                && std::abs(tile.width() - panel.image.width()) < 1e-6
                && std::abs(tile.height() - panel.image.height()) < 1e-6
                && whole(tile.left()) && whole(tile.top());
            if (!ok) {
                qCritical("warp %d x %d on a tile of %g x %g device pixels at (%g, %g)",
                    panel.image.width(), panel.image.height(), tile.width(),
                    tile.height(), tile.left(), tile.top());
            }
            return ok;
        };
        // The tile covers the whole canvas, reaching past each edge by less
        // than a device pixel.
        const auto coversCanvas = [&window] {
            const auto panel = window.mappedPanelForTest(-1);
            const auto pixel = 1.0 / (panel.scale * window.devicePixelRatioF());
            const auto& tile = panel.tile;
            const auto& canvas = panel.canvas;
            const bool ok = !canvas.isEmpty()
                && tile.left() <= canvas.left() + 1e-9 && tile.left() > canvas.left() - pixel
                && tile.top() <= canvas.top() + 1e-9 && tile.top() > canvas.top() - pixel
                && tile.right() >= canvas.right() - 1e-9
                && tile.right() < canvas.right() + pixel
                && tile.bottom() >= canvas.bottom() - 1e-9
                && tile.bottom() < canvas.bottom() + pixel;
            if (!ok) {
                qCritical("tile %g x %g at (%g, %g), canvas %g x %g at (%g, %g)",
                    tile.width(), tile.height(), tile.left(), tile.top(),
                    canvas.width(), canvas.height(), canvas.left(), canvas.top());
            }
            return ok;
        };
        // The pixmap's centre lies inside the sector: R = 0.48, Z = 1.44 is
        // r = 1.52 at theta = 0.32.
        const auto probeCentre = [&window] {
            const auto size = window.activeViewImageSizeForTest();
            const auto readout
                = window.probeReadoutActiveViewForTest(size[0] / 2, size[1] / 2);
            return readout.contains(QStringLiteral("value"))
                && readout.contains(QStringLiteral("R="));
        };
        // The view frames a physical target as a zoom to it does (see the
        // mapped-grid smoke).
        const auto frames = [&window](const QRectF& target) {
            const auto panel = window.mappedPanelForTest(-1);
            const auto& shown = panel.window;
            const auto& tile = panel.tile;
            const auto& visible = panel.visible;
            if (shown.isEmpty() || tile.isEmpty() || visible.isEmpty()
                || !(panel.scale > 0.0)) {
                return false;
            }
            const auto toScene = [&shown, &tile](double x, double y) {
                return QPointF(
                    tile.left() + (x - shown.left()) * tile.width() / shown.width(),
                    tile.top() + (shown.bottom() - y) * tile.height() / shown.height());
            };
            const QRectF wanted(toScene(target.left(), target.bottom()),
                toScene(target.right(), target.top()));
            const auto slack = static_cast<double>(
                QApplication::style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 6)
                / panel.scale;
            const bool covers = visible.left() <= wanted.left() + slack
                && visible.right() >= wanted.right() - slack
                && visible.top() <= wanted.top() + slack
                && visible.bottom() >= wanted.bottom() - slack;
            const bool fills = std::abs(visible.width() - wanted.width()) <= slack
                || std::abs(visible.height() - wanted.height()) <= slack;
            if (!covers || !fills) {
                qCritical("view shows [%g, %g] x [%g, %g] for [%g, %g] x [%g, %g]",
                    visible.left(), visible.right(), visible.top(), visible.bottom(),
                    wanted.left(), wanted.right(), wanted.top(), wanted.bottom());
            }
            return covers && fills;
        };
        const auto runPhase = [&window, &application, progress, fail, coversCanvas,
                                  drawnForScreen, probeCentre, frames] {
            // Read the state once no slice is on its way (see the mapped-grid
            // smoke).
            if (window.sliceRequestPendingForTest()
                || window.slicesInFlightForTest() > 0) {
                return;
            }
            const auto size = window.activeViewImageSizeForTest();
            switch (progress->phase) {
            case 0: {
                // Fit: the warp of the sector's whole box at the view's pixels.
                if (!drawnForScreen() || !coversCanvas()) {
                    fail("at Fit the R-Z view is not the warp of its canvas");
                    return;
                }
                if (!window.displayIsSphericalWarpForTest() || size[0] <= 32
                    || size[1] <= 32) {
                    qCritical("R-Z pixmap %d x %d", size[0], size[1]);
                    fail("the R-Z warp is not drawn at the screen's pixels");
                    return;
                }
                // The box's bottom-right corner (R = 0.96, Z = 0.88) is past
                // theta = 0.5: no cell is drawn there.
                const auto corner = window.probeReadoutActiveViewForTest(
                    size[0] - 1, size[1] - 1);
                if (!probeCentre() || corner != QObject::tr("no data")) {
                    qCritical("corner '%s'", qPrintable(corner));
                    fail("the probe does not follow the sector");
                    return;
                }
                if (window.activeViewGridBoxCountForTest() == 0) {
                    fail("the grid boxes are missing from the warp");
                    return;
                }
                progress->fitScale = window.activeViewTransformAndScrollForTest()[0];
                progress->phase = 1;
                for (int notch = 0; notch < 3; ++notch) {
                    window.wheelActiveViewForTest(1);
                }
                break;
            }
            case 1: {
                // Three notches in: the warp is the part of the sector on
                // screen; the plane is the whole (r, theta) grid still.
                const auto panel = window.mappedPanelForTest(-1);
                const bool smaller = panel.tile.width() < panel.canvas.width() - 1e-6
                    || panel.tile.height() < panel.canvas.height() - 1e-6;
                const auto margin = 1.0 / (panel.scale * window.devicePixelRatioF());
                if (!drawnForScreen() || panel.fit || panel.resliced
                    || std::abs(panel.scale / progress->fitScale - 1.15 * 1.15 * 1.15) > 1e-6
                    || !smaller
                    || !panel.canvas.adjusted(-margin, -margin, margin, margin)
                            .contains(panel.tile)
                    || !probeCentre()) {
                    qCritical("scale %g over %g, tile %gx%g of canvas %gx%g", panel.scale,
                        progress->fitScale, panel.tile.width(), panel.tile.height(),
                        panel.canvas.width(), panel.canvas.height());
                    fail("a wheel zoom did not draw the warp for the window it shows");
                    return;
                }
                progress->zoomWindow = panel.window;
                progress->phase = 2;
                window.rubberBandZoomActiveViewForTest();
                break;
            }
            case 2: {
                // The rubber band took the central half of that window: the
                // view frames it and the warp follows, still without a
                // re-slice.
                const auto& zoom = progress->zoomWindow;
                const QRectF selection(zoom.left() + 0.25 * zoom.width(),
                    zoom.top() + 0.25 * zoom.height(), 0.5 * zoom.width(),
                    0.5 * zoom.height());
                const auto panel = window.mappedPanelForTest(-1);
                if (!drawnForScreen() || panel.fit || panel.resliced || !probeCentre()
                    || !frames(selection)) {
                    fail("an R-Z rubber band did not frame its selection");
                    return;
                }
                const auto before = window.activeViewTransformAndScrollForTest();
                progress->scrolledFrom = panel.window;
                progress->tileBefore = panel.tile;
                window.scrollActiveViewForTest(-24, -24);
                progress->scrolled = window.activeViewTransformAndScrollForTest();
                if (progress->scrolled == before) {
                    fail("the zoomed R-Z view had nowhere to scroll");
                    return;
                }
                progress->phase = 3;
                break;
            }
            case 3: {
                // The scroll moved the window; the warp drawn for it landed
                // with the view exactly where the scroll left it.
                const auto now = window.activeViewTransformAndScrollForTest();
                if (now != progress->scrolled) {
                    qCritical("scale %g, %g scroll %g, %g; scrolled to %g, %g scroll %g, %g",
                        now[0], now[1], now[2], now[3], progress->scrolled[0],
                        progress->scrolled[1], progress->scrolled[2],
                        progress->scrolled[3]);
                    fail("an R-Z arrival moved the view");
                    return;
                }
                const auto panel = window.mappedPanelForTest(-1);
                if (!drawnForScreen() || panel.resliced
                    || panel.window == progress->scrolledFrom
                    || panel.tile == progress->tileBefore || !probeCentre()) {
                    fail("a scroll did not draw the warp for the window it moved to");
                    return;
                }
                progress->phase = 4;
                window.setDisplayModeForTest(amrvis::DisplayMode::RasterContours, 3);
                break;
            }
            case 4:
            case 5:
            case 6: {
                // Into contours (a fresh slice); then a second scroll whose
                // redraw is held at the worker gate while a new contour
                // count is sent and dispatched, cancelling it: that refresh,
                // asked with the raster clean, must still draw the raster for
                // the window it names rather than keep the one drawn for the
                // old window; then the raster again. Throughout, the pixmap
                // is the warp of the window on show, the probe reads the
                // cells under it and the view stays put.
                const auto panel = window.mappedPanelForTest(-1);
                if (!panel.warped || !drawnForScreen() || panel.window != panel.drawn
                    || !probeCentre()
                    || window.activeViewTransformAndScrollForTest() != progress->scrolled) {
                    qCritical("phase %d: window [%g, %g] x [%g, %g], pixmap drawn for "
                              "[%g, %g] x [%g, %g]", progress->phase, panel.window.left(),
                        panel.window.right(), panel.window.top(), panel.window.bottom(),
                        panel.drawn.left(), panel.drawn.right(), panel.drawn.top(),
                        panel.drawn.bottom());
                    fail("a contour refresh lost the R-Z warp, left the raster behind "
                         "the window, or moved the view");
                    return;
                }
                if (progress->phase == 4) {
                    window.armSliceGateForTest();
                    window.scrollActiveViewForTest(-16, -16);
                    progress->scrolled = window.activeViewTransformAndScrollForTest();
                    progress->phase = 5;
                    auto* poll = new QTimer(&window);
                    poll->setInterval(1);
                    QObject::connect(poll, &QTimer::timeout, &window,
                        [&window, progress, poll] {
                            // The redraw is on its way (and held): send the
                            // count; once its request is dispatched too, the
                            // redraw is cancelled, and both may run.
                            if (!progress->contoursSent
                                && window.slicesInFlightForTest() > 0) {
                                progress->contoursSent = true;
                                window.setDisplayModeForTest(
                                    amrvis::DisplayMode::RasterContours, 7);
                            }
                            if (progress->contoursSent
                                && !window.sliceRequestPendingForTest()) {
                                window.releaseSliceGateForTest();
                                poll->stop();
                                poll->deleteLater();
                            }
                        });
                    poll->start();
                    break;
                }
                if (progress->phase == 5) {
                    window.setDisplayModeForTest(amrvis::DisplayMode::Raster, 7);
                } else {
                    window.selectSphericalDisplayForTest(1);  // r-theta
                }
                ++progress->phase;
                break;
            }
            case 7: {
                // r-theta: the flat 16 x 8 raster, refitted, with the line
                // tool back.
                if (window.mappedPanelForTest(-1).warped
                    || window.displayIsSphericalWarpForTest() || size[0] != 16
                    || size[1] != 8 || !window.activeViewIsFitToWindowForTest()) {
                    qCritical("r-theta pixmap %d x %d", size[0], size[1]);
                    fail("switching to r-theta did not restore the flat raster");
                    return;
                }
                progress->phase = 8;
                window.selectSphericalDisplayForTest(0);  // R-Z
                break;
            }
            default:
                // R-Z again: back on the canvas, the warp of the whole sector.
                application.exit(drawnForScreen() && coversCanvas() ? 0 : 3);
                break;
            }
        };
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, fail](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                if (!window.displayIsSphericalWarpForTest()) {
                    fail("the dataset did not open in the R-Z layout");
                }
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [&window, runPhase] {
                QTimer::singleShot(0, &window, runPhase);
            });
        QTimer::singleShot(60000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window, [&window, path] {
            window.setGridBoxesVisibleForTest(true);
            window.selectSphericalDisplayForTest(0);  // R-Z, whatever persisted
            window.openDataset(path);
        });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--mapped-grid-cap-smoke-test") {
        // View > Mapped Grid on plotfile_3d_mapped_wide, whose x-z plane
        // (4200 x 4 cells) is past the 4096 output cap. At Fit it is drawn
        // whole; zoomed to a fixed scale, the cells on show are re-sliced with
        // a margin; a scroll within that margin only re-draws the warp, and
        // one far past the plane re-slices where the view went.
        const std::filesystem::path path(argv[2]);
        struct Progress {
            int phase = 0;
            QRectF plane;
            QRectF shown;
        };
        auto progress = std::make_shared<Progress>();
        const auto runPhase = [&window, &application, progress] {
            // Read once nothing is on its way (see --mapped-grid-smoke-test).
            if (window.sliceRequestPendingForTest()
                || window.slicesInFlightForTest() > 0) {
                return;
            }
            window.setActiveViewForTest(1);  // the x-z panel
            const auto plane = window.activeViewPlaneRegionForTest();
            const auto shown = window.activeViewMappedWindowForTest();
            const auto size = window.activeViewImageSizeForTest();
            const auto probe
                = window.probeReadoutActiveViewForTest(size[0] / 2, size[1] / 2);
            const bool drawn = window.activeViewIsMappedForTest() && !shown.isEmpty()
                && probe.contains(QStringLiteral("value"));
            const bool holdsWindow
                = plane.left() <= shown.left() && plane.right() >= shown.right();
            const auto fail = [&](const char* message) {
                qCritical("phase %d: plane x [%g, %g], window x [%g, %g], probe '%s'",
                    progress->phase, plane.left(), plane.right(), shown.left(),
                    shown.right(), qPrintable(probe));
                qCritical("%s", message);
                application.exit(1);
            };
            switch (progress->phase) {
            case 0:
                // At Fit the plane is under a pixel tall, so a pixel centre
                // may miss it and the probe is not asked.
                if (!window.activeViewIsMappedForTest() || shown.isEmpty()
                    || window.activeViewIsZoomedForTest()) {
                    fail("the capped plane was not drawn whole at Fit");
                    return;
                }
                progress->phase = 1;
                window.setActiveViewScaleForTest(8);
                break;
            case 1:
                // Eight pixels per cell: the cells on show and a margin,
                // re-sliced at native resolution.
                if (!drawn || !window.activeViewIsZoomedForTest()
                    || !(plane.width() < 1000.0) || !holdsWindow) {
                    fail("zooming in did not re-slice the cells on show");
                    return;
                }
                progress->plane = plane;
                progress->shown = shown;
                progress->phase = 2;
                window.scrollActiveViewForTest(-40, 0);
                break;
            case 2:
                // Five cells along, within the margin: the warp is drawn again
                // from the same plane.
                if (!drawn || plane != progress->plane || shown == progress->shown) {
                    fail("a scroll within the plane re-sliced it");
                    return;
                }
                progress->phase = 3;
                window.scrollActiveViewForTest(-4000, 0);
                break;
            case 3:
                // Five hundred cells along, far past the plane: re-sliced
                // where the view went, and drawn there.
                if (!drawn || !window.activeViewIsZoomedForTest() || !holdsWindow
                    || !(shown.left() > progress->plane.right())) {
                    fail("a scroll past the plane did not re-slice where the view went");
                    return;
                }
                application.exit(0);
                break;
            default:
                break;
            }
        };
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                window.setActiveViewForTest(1);
                window.setMappedGridForTest(true);
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [&window, runPhase] {
                QTimer::singleShot(0, &window, runPhase);
            });
        QTimer::singleShot(60000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--physical-fixed-scale-smoke-test") {
        // A 3-D dataset whose cells are 0.25 in x and y and 0.0625 in z
        // (plotfile_3d_pair_lower).
        // In Physical Size a fixed scale means the same pixels per length on
        // every panel: the tightest cell (z) is one pixel at 1x, so x and y
        // are four pixels a cell whether the panel shows z beside them or
        // not. Cell Counts keeps one pixel per cell everywhere.
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                const auto near = [](double actual, double expected) {
                    return std::abs(actual - expected) <= 0.02 * expected;
                };
                const auto scales = [&window, near](double xzX, double xzY,
                                        double xyX, double xyY, double yzX, double yzY) {
                    const auto xz = window.panelTransformScaleForTest(1);
                    const auto xy = window.panelTransformScaleForTest(2);
                    const auto yz = window.panelTransformScaleForTest(0);
                    return near(xz.first, xzX) && near(xz.second, xzY)
                        && near(xy.first, xyX) && near(xy.second, xyY)
                        && near(yz.first, yzX) && near(yz.second, yzY);
                };
                const auto report = [&window] {
                    const auto xz = window.panelTransformScaleForTest(1);
                    const auto xy = window.panelTransformScaleForTest(2);
                    const auto yz = window.panelTransformScaleForTest(0);
                    qCritical("XZ (%g, %g) XY (%g, %g) YZ (%g, %g)", xz.first, xz.second,
                        xy.first, xy.second, yz.first, yz.second);
                };
                if (!success) {
                    application.exit(2);
                    return;
                }
                window.setAspectModeForTest(amrvis::qt::AspectMode::CellCounts);
                window.selectFixedScaleForTest(1);
                if (!scales(1.0, 1.0, 1.0, 1.0, 1.0, 1.0)) {
                    report();
                    qCritical("Cell Counts at 1x is not one pixel per cell on every panel");
                    application.exit(1);
                    return;
                }
                window.setAspectModeForTest(amrvis::qt::AspectMode::PhysicalSize);
                if (!scales(4.0, 1.0, 4.0, 4.0, 4.0, 1.0)) {
                    report();
                    qCritical("Physical Size at 1x does not show x the same size on "
                              "the XY and XZ panels");
                    application.exit(1);
                    return;
                }
                window.selectFixedScaleForTest(2);
                application.exit(scales(8.0, 2.0, 8.0, 8.0, 8.0, 2.0) ? 0 : 1);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--rubber-zoom-sync-smoke-test") {
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                auto* sync = window.findChild<QAction*>(
                    QStringLiteral("syncRubberBandZoomAction"));
                if (!success || sync == nullptr || !sync->isVisible()) {
                    application.exit(1);
                    return;
                }
                const QSignalBlocker blocker(sync);
                sync->setChecked(true);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application] {
                        application.exit(
                            window.allViewsRubberBandZoomedForTest() ? 0 : 1);
                    }, Qt::SingleShotConnection);
                window.rubberBandZoomActiveViewForTest();
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--rubber-zoom-local-smoke-test") {
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                auto* sync = window.findChild<QAction*>(
                    QStringLiteral("syncRubberBandZoomAction"));
                if (!success || sync == nullptr) {
                    application.exit(1);
                    return;
                }
                const QSignalBlocker blocker(sync);
                sync->setChecked(false);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application] {
                        application.exit(
                            window.rubberBandZoomedViewCountForTest() == 1
                                ? 0 : 1);
                    }, Qt::SingleShotConnection);
                window.rubberBandZoomActiveViewForTest();
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--rubber-overzoom-smoke-test") {
        // Regression for issue #45 (over-zoom after rubber-band): on a dataset
        // whose full-domain raster is capped at maxSliceOutputDimension, the
        // cropped re-slice arrives at a finer pixels-per-cell density than the
        // raster it replaces. Preserving the scene transform then shows the
        // crop over-zoomed with part of it outside the viewport. Rubber-band
        // the central half and require the arrived crop to be fully visible.
        const std::filesystem::path path(argv[2]);
        // Distinct exit codes so a failure pinpoints its stage: 2 = the load
        // itself failed, 3 = the initial fitted raster was not fully visible,
        // 1 = the regression (arrived crop not fully framed).
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                // Sanity: the fitted full-domain raster starts fully visible.
                if (!window.activeViewShowsWholeImageForTest()) {
                    application.exit(3);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application] {
                        application.exit(
                            window.activeViewIsZoomedForTest()
                                && window.activeViewShowsWholeImageForTest()
                            ? 0 : 1);
                    }, Qt::SingleShotConnection);
                window.rubberBandZoomActiveViewForTest();
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--pan-zoom-smoke-test") {
        const std::filesystem::path path(argv[2]);
        auto phase = std::make_shared<int>(0);
        auto immediateScale = std::make_shared<double>(0.0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, immediateScale](bool success) {
                auto* sync = window.findChild<QAction*>(
                    QStringLiteral("syncRubberBandZoomAction"));
                if (!success || sync == nullptr) {
                    application.exit(1);
                    return;
                }
                const QSignalBlocker blocker(sync);
                sync->setChecked(true);
                window.rubberBandZoomActiveViewForTest();
                *immediateScale = window.activeViewScaleForTest();
                // Exercise the timing window: pan before the cropped slice
                // requested by the rubber band has settled.
                window.panActiveViewForTest(5.0, 0.0);
                if (std::abs(window.activeViewScaleForTest() - *immediateScale)
                    > 1.0e-12) {
                    application.exit(1);
                }
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [&window, &application, phase, immediateScale] {
                constexpr double tolerance = 1.0e-12;
                if ((*phase)++ == 0) {
                    if (std::abs(window.activeViewScaleForTest()
                            - *immediateScale)
                        > tolerance) {
                        application.exit(1);
                        return;
                    }
                    // A valid pan from the central crop must preserve the
                    // active panel's custom transform through the re-slice.
                    window.setActiveViewScaleForTest(4);
                    window.panActiveViewForTest(-5.0, 0.0);
                    return;
                }
                if (std::abs(window.activeViewScaleForTest() - 4.0)
                    > tolerance) {
                    application.exit(1);
                    return;
                }
                // The first pan reached the domain edge. Panning farther is a
                // no-op and must not refit the panel either.
                window.setActiveViewScaleForTest(3);
                window.panActiveViewForTest(-5.0, 0.0);
                application.exit(
                    std::abs(window.activeViewScaleForTest() - 3.0)
                            <= tolerance
                        ? 0 : 1);
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--fixed-scale-arrival-smoke-test") {
        const std::filesystem::path path(argv[2]);
        const int factor = std::stoi(argv[3]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, factor](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                window.selectFixedScaleForTest(factor);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, factor] {
                        application.exit(
                            window.fixedScaleStateMatchesForTest(factor)
                                ? 0 : 1);
                    }, Qt::SingleShotConnection);
                // Force an asynchronous replacement raster after selecting the
                // scale, reproducing the delayed-arrival race.
                window.enableVisibleRasterForTest();
            }, Qt::SingleShotConnection);
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--effective-scale-smoke-test") {
        // A domain wider than maxSliceOutputDimension finest cells cannot have
        // a whole-domain raster at finest resolution, so a local fixed scale
        // magnifies it by less than the factor says. The UI has to state what
        // it actually applied, and the number it states has to be the one the
        // view is really using -- checked here against the visible window.
        const std::filesystem::path path(argv[2]);
        const int factor = std::stoi(argv[3]);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, factor](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                window.selectToolbarFixedScaleForTest(factor);
                // Measure past the layout pass the new scale's scroll bars
                // demand: they shrink the viewport, and the window below is
                // read in viewport pixels.
                QTimer::singleShot(200, &window,
                    [&window, &application, factor] {
                        const auto claimed
                            = window.effectiveFixedScaleForTest(factor);
                        if (!(claimed > 0.0)) {
                            qCritical("no reduced scale reported on a domain "
                                      "past the raster clamp");
                            application.exit(1);
                            return;
                        }
                        const auto label = window.scaleUiLabelForTest();
                        // →, not the raw character: QStringLiteral converts
                        // at compile time, and MSVC without a BOM or /utf-8
                        // (neither the windows preset nor
                        // amrexplorer_warnings.cmake passes it) reads the source
                        // as CP1252, so the three UTF-8 bytes would become three
                        // wrong code points here. The production label survives
                        // that because tr() takes a narrow literal and decodes
                        // it with fromUtf8 at run time, so only this comparison
                        // would break -- on windows-2022 alone.
                        if (!label.contains(QStringLiteral("\u2192"))) {
                            qCritical("the Scale button reports '%s', which "
                                      "does not state the applied scale",
                                qUtf8Printable(label));
                            application.exit(1);
                            return;
                        }
                        // The decorated label must not cost the menu its
                        // check: matching the radio on that string finds
                        // nothing, and the toolbar/menu split reopens on
                        // exactly the domains this reporting exists for.
                        const auto checked
                            = window.scaleMenuCheckedLabelForTest();
                        if (checked
                            != QStringLiteral("%1x").arg(factor)) {
                            qCritical("a clamped toolbar pick left View > "
                                      "Scale showing '%s'",
                                qUtf8Printable(checked));
                            application.exit(1);
                            return;
                        }
                        // What the view really does: viewport pixels per
                        // finest cell across the window it shows.
                        const auto window_ = window
                            .activeViewVisibleDataWindowForTest();
                        const auto viewport
                            = window.activeViewViewportSizeForTest();
                        const auto cellSize
                            = window.activeViewFinestCellSizeForTest();
                        if (!(window_.width() > 0.0) || !(cellSize > 0.0)) {
                            application.exit(1);
                            return;
                        }
                        const auto cells = window_.width() / cellSize;
                        const auto actual
                            = static_cast<double>(viewport[0]) / cells;
                        if (std::abs(actual - claimed) > 0.05 * claimed) {
                            qCritical("the UI claims %gx but the view applies "
                                      "%gx", claimed, actual);
                            application.exit(1);
                            return;
                        }
                        application.exit(0);
                    });
            }, ::Qt::SingleShotConnection);
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--scale-state-smoke-test") {
        // The toolbar Scale button and View > Scale are one state shown twice.
        // Pick 4x from the *toolbar* menu -- the path that used to leave the
        // View-menu radio unchecked -- and require the full agreement
        // fixedScaleStateMatchesForTest asserts. Then open a second dataset,
        // which arrives fitted, and require the report to have come back to
        // Fit rather than still claiming 4x.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, phase, second](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                if (*phase == 0) {
                    *phase = 1;
                    window.selectToolbarFixedScaleForTest(4);
                    if (!window.fixedScaleStateMatchesForTest(4)) {
                        qCritical("a toolbar scale pick left the state split");
                        application.exit(1);
                        return;
                    }
                    QTimer::singleShot(0, &window, [&window, second] {
                        window.openDataset(second);
                    });
                    return;
                }
                if (window.scaleUiLabelForTest() != QStringLiteral("Fit")) {
                    qCritical("a new dataset kept the old scale report '%s'",
                        qUtf8Printable(window.scaleUiLabelForTest()));
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, first] { window.openDataset(first); });
    } else if (argc == 5
        && std::string_view(argv[1]) == "--sequence-scale-report-smoke-test") {
        // The clamped scale report is computed from the active view's dataset,
        // and a sequence can carry a different domain than the dataset the
        // scale was picked on. A fixed scale is a persistent view mode and
        // survives the raster replacement, so the factor carries over -- but
        // what it *comes to* does not.
        //
        // Pick 4x on a narrow plotfile (literal, no clamp), then open a
        // sequence 8192 finest cells across, twice the largest whole-domain
        // raster. The same 4x now applies 2x, and the button has to say so
        // rather than keep the number it computed for the dataset before.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path frameOne(argv[3]);
        const std::filesystem::path frameTwo(argv[4]);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, frameOne, frameTwo](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                window.selectToolbarFixedScaleForTest(4);
                if (window.scaleUiLabelForTest() != QStringLiteral("4x")) {
                    qCritical("a narrow domain reported '%s', expected a "
                              "literal 4x",
                        qUtf8Printable(window.scaleUiLabelForTest()));
                    application.exit(1);
                    return;
                }
                QTimer::singleShot(0, &window, [&window, frameOne, frameTwo] {
                    window.openSequence({frameOne, frameTwo});
                });
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed, &application,
            [&window, &application](int) {
                const auto label = window.scaleUiLabelForTest();
                if (!label.startsWith(QStringLiteral("4x"))) {
                    qCritical("a sequence frame dropped the 4x scale: '%s'",
                        qUtf8Printable(label));
                    application.exit(1);
                    return;
                }
                if (label == QStringLiteral("4x")) {
                    qCritical("a wider sequence frame kept the previous "
                              "dataset's literal 4x, applying 2x");
                    application.exit(1);
                    return;
                }
                // ...and the number it now states must be the one in force.
                const auto effective = window.effectiveFixedScaleForTest(4);
                if (std::fabs(effective - 2.0) > 1.0e-9) {
                    qCritical("reported an effective scale of %f, expected 2",
                        effective);
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed, &application,
            [&application] { application.exit(2); });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, first] { window.openDataset(first); });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--spherical-scale-report-smoke-test") {
        // A spherical view reports the plain factor, never a reduced one. Its
        // raster is warped, so one raster pixel does not stand for a fixed
        // number of finest cells and there is no single magnification to
        // state; effectiveFixedScale excludes it for the same reason
        // logicalImageSize does.
        //
        // The fixture is 8192 finest cells across -- twice the largest
        // whole-domain raster -- so a Cartesian view of the same size would
        // decorate. That is what makes this distinguish the exclusion from a
        // domain that simply does not clamp.
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                if (!window.displayIsSphericalForTest()) {
                    qCritical("the fixture did not open as a spherical view, "
                              "so this test proves nothing");
                    application.exit(1);
                    return;
                }
                // Pin the mode: choosing a display writes it through
                // saveSettings(), so it survives into the next run and this
                // test would otherwise inherit whatever the last one left.
                window.selectSphericalDisplayForTest(0);
                if (!window.displayIsSphericalWarpForTest()) {
                    qCritical("R-Z did not select, so the warp case is untested");
                    application.exit(1);
                    return;
                }
                window.selectToolbarFixedScaleForTest(32);
                const auto label = window.scaleUiLabelForTest();
                if (label != QStringLiteral("32x")) {
                    qCritical("an R-Z spherical view reported '%s', expected a "
                              "plain 32x",
                        qUtf8Printable(label));
                    application.exit(1);
                    return;
                }
                if (window.effectiveFixedScaleForTest(32) != 0.0) {
                    qCritical("an R-Z spherical view claimed a scale");
                    application.exit(1);
                    return;
                }
                // ...but only R-Z warps. r-theta draws the logical grid as-is
                // and theta-r transposes it, so both are clamped exactly like a
                // Cartesian raster and must report the reduction. Excluding
                // every spherical view left these two silently applying 16x
                // while the button said 32x.
                for (const auto mode : {1, 2}) {
                    window.selectSphericalDisplayForTest(mode);
                    if (window.displayIsSphericalWarpForTest()) {
                        qCritical("mode %d still reports as warped", mode);
                        application.exit(1);
                        return;
                    }
                    const auto effective = window.effectiveFixedScaleForTest(32);
                    if (std::fabs(effective - 16.0) > 1.0e-9) {
                        qCritical("unwarped spherical mode %d reported an "
                                  "effective scale of %f, expected 16",
                            mode, effective);
                        application.exit(1);
                        return;
                    }
                }
                window.selectSphericalDisplayForTest(0);
                application.exit(0);
            });
        QTimer::singleShot(60000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--arrow-key-routing-smoke-test") {
        // The arrow keys pan the focused image view and nothing else. They
        // used to be window-context QShortcuts, which took Up/Down from every
        // toolbar spin box and combo -- Qt line edits claim Left/Right through
        // ShortcutOverride but not Up/Down, and non-editable combos claim no
        // arrows at all -- so a keyboard user stepping the level or a slice
        // position panned the image instead.
        //
        // Only a window-level test sees this. The ImageView unit test sends
        // its events to the view directly, which is the one delivery that
        // cannot tell a focused view from an unfocused one. Here the events go
        // to whatever holds focus, the way Qt delivers real key presses, so
        // the routing is the thing under test.
        const std::filesystem::path path(argv[2]);
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, phase, path](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                const auto press = [&application](::Qt::Key key) {
                    auto* const target = QApplication::focusWidget();
                    if (target == nullptr) {
                        qCritical("no widget held focus");
                        application.exit(1);
                        return false;
                    }
                    QKeyEvent event(QEvent::KeyPress, key, ::Qt::NoModifier);
                    QApplication::sendEvent(target, &event);
                    return true;
                };
                const auto reopen = [&window, path] {
                    QTimer::singleShot(0, &window,
                        [&window, path] { window.openDataset(path); });
                };
                if (*phase == 0) {
                    *phase = 1;
                    // Scrollable, so a pan step has somewhere to go.
                    window.selectToolbarFixedScaleForTest(8);
                    // Precondition, not the property: a freshly shown window
                    // gives the view focus on its own. Phase 1 is where the
                    // open path's own focus handling is put to the question.
                    if (!window.activeViewHasFocusForTest()) {
                        qCritical("the view did not start focused");
                        application.exit(1);
                        return;
                    }
                    if (!press(::Qt::Key_Left) || !press(::Qt::Key_Up)) {
                        return;
                    }
                    if (window.panStepRequestsForTest() != 2) {
                        qCritical("arrow keys on the focused view produced %zu "
                                  "pan requests, expected 2",
                            window.panStepRequestsForTest());
                        application.exit(1);
                        return;
                    }
                    // The level combo. Up/Down belong to it -- this is the
                    // binding that used to be stolen -- and must not reach the
                    // view at all.
                    window.focusLevelSelectorForTest();
                    if (window.activeViewHasFocusForTest()) {
                        qCritical("the level selector did not take focus");
                        application.exit(1);
                        return;
                    }
                    if (!press(::Qt::Key_Up) || !press(::Qt::Key_Down)
                        || !press(::Qt::Key_Left) || !press(::Qt::Key_Right)) {
                        return;
                    }
                    if (window.panStepRequestsForTest() != 2) {
                        qCritical("an arrow key in the level selector reached the "
                                  "image view (%zu pan requests)",
                            window.panStepRequestsForTest());
                        application.exit(1);
                        return;
                    }
                    // Focus is nowhere in particular, the way it is when a file
                    // dialog closes. The open should claim it for the view, so
                    // the keys work without a click first.
                    window.clearFocusForTest();
                    reopen();
                    return;
                }
                if (*phase == 1) {
                    *phase = 2;
                    if (!window.activeViewHasFocusForTest()) {
                        qCritical("an open left the view unfocused, so the "
                                  "arrow keys need a click first");
                        application.exit(1);
                        return;
                    }
                    // ...but an open must not take focus away from a control
                    // the user is working in. This arrives from a watcher
                    // completion, which on a slow open lands long after the
                    // dialog closed and they moved on.
                    window.focusLevelSelectorForTest();
                    reopen();
                    return;
                }
                // Not necessarily the level selector by now -- teardown
                // disables it and Qt moves focus to a neighbouring control --
                // but it must not have landed in the view.
                if (window.activeViewHasFocusForTest()) {
                    qCritical("an open pulled focus into the view while a "
                              "control had it");
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    }
    else {
        return {false, std::nullopt};
    }
    return {true, std::nullopt};
}

} // namespace amrvis::qt::smoke
