#include "SmokeHarnessInternal.hpp"

#include "MainWindow.hpp"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QSignalBlocker>
#include <QTimer>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Zoom: the zoom / pan / scale scenarios: raster and rubber-band zoom, pan,
// spherical supersample, fixed and effective scale and its report, arrow-key
// routing. Every branch drives MainWindow through its ForTest accessors and
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
        && std::string_view(argv[1]) == "--spherical-supersample-smoke-test") {
        // Zoom-preserve regression for the 2-D spherical supersample control:
        // after zooming a spherical view (view-only, no re-slice), changing the
        // warp factor must resize the warped raster yet keep the same zoomed
        // framing rather than refitting to the whole sector.
        const std::filesystem::path path(argv[2]);
        auto beforeWidth = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, beforeWidth](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                // Spherical zoom is view-only; it must leave fit-to-window.
                window.rubberBandZoomActiveViewForTest();
                if (window.activeViewFitsWindowForTest()) {
                    application.exit(2);
                    return;
                }
                *beforeWidth = window.activeViewImageWidthForTest();
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, beforeWidth] {
                        const int afterWidth = window.activeViewImageWidthForTest();
                        // The 8x warp resized the raster larger, and the view is
                        // still zoomed (framing preserved, not refit to fit).
                        const bool resized = afterWidth > *beforeWidth;
                        const bool preserved = !window.activeViewFitsWindowForTest();
                        application.exit(resized && preserved ? 0 : 3);
                    }, Qt::SingleShotConnection);
                // Default factor is 4x; bump to 8x so the raster grows.
                window.setSphericalSupersampleForTest(8);
            });
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
        QTimer::singleShot(20000, &application,
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
