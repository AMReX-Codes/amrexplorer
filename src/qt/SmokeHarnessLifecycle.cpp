#include "SmokeHarnessInternal.hpp"

#include "MainWindow.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QRectF>
#include <QRunnable>
#include <QThreadPool>
#include <QTreeWidget>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

// Lifecycle: the open / failure / idle-state / shutdown scenarios: plain
// open, open failure, cache budget, idle UI state, closing with a busy pool,
// closing one window of several, quitting, quitting mid-export. Every branch
// drives MainWindow through its ForTest accessors and arms connections and
// timers for main() to run; see SmokeHarness.hpp.

namespace amrvis::qt::smoke {

Outcome dispatchLifecycle(Context& context)
{
    // The branches read these names as main() declared them; binding them
    // here keeps the moved code verbatim.
    auto& application = context.application;
    auto& window = context.window;
    const int argc = context.argc;
    char** argv = context.argv;

    if (argc == 3 && std::string_view(argv[1]) == "--smoke-test") {
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::datasetOpenFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                // The Dataset Metadata dock is filled before the open is
                // reported, geometry rows included for a plotfile.
                auto* metadataTree = window.findChild<QTreeWidget*>(
                    QStringLiteral("metadataTree"));
                const bool geometryShown = metadataTree != nullptr
                    && !metadataTree->findItems(QStringLiteral("Cell size"),
                        Qt::MatchExactly | Qt::MatchRecursive).isEmpty()
                    && !metadataTree->findItems(
                        QStringLiteral("Coordinate system"),
                        Qt::MatchExactly | Qt::MatchRecursive).isEmpty();
                if (!geometryShown) {
                    qCritical("the metadata dock lists no geometry");
                }
                application.exit(geometryShown ? 0 : 1);
            });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path, true); });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--open-failure-smoke-test") {
        // A failed open has already torn the previous dataset down, so it must
        // leave a placeholder that says so rather than the "Loading dataset..."
        // one it replaced -- and the window must still be usable afterwards.
        // Open a bad path, check the settled state, then open a good one.
        const std::filesystem::path bad(argv[2]);
        const std::filesystem::path good(argv[3]);
        auto attemptedGood = std::make_shared<bool>(false);
        QObject::connect(&window, &amrvis::qt::MainWindow::datasetOpenFinished,
            &application, [&window, &application, good, attemptedGood](
                              bool success) {
                if (*attemptedGood) {
                    // The recovery open. Opening is not the end of it -- the
                    // slice has to arrive and clear the placeholder -- so the
                    // verdict is left to initialSliceFinished below.
                    if (!success) {
                        qCritical("the recovery open failed");
                        application.exit(1);
                    }
                    return;
                }
                if (success) {
                    qCritical("the bad path opened successfully");
                    application.exit(1);
                    return;
                }
                const auto placeholder = window.viewPlaceholderForTest();
                if (placeholder.isEmpty()
                    || placeholder.contains(QStringLiteral("Loading"))) {
                    qCritical("a failed open left the panels at '%s'",
                        qUtf8Printable(placeholder));
                    application.exit(1);
                    return;
                }
                *attemptedGood = true;
                // Rendered, not metadata-only: the placeholder is what a
                // failed open leaves behind, so only a real slice arriving
                // proves the recovery cleared it. Both legs used to skip the
                // render, which left that unproven.
                QTimer::singleShot(0, &window,
                    [&window, good] { window.openDataset(good); });
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, attemptedGood](bool success) {
                if (!*attemptedGood) {
                    // The failed open's own signal; the placeholder it leaves
                    // is checked above.
                    return;
                }
                if (!success) {
                    qCritical("the recovery open did not render");
                    application.exit(1);
                    return;
                }
                if (!window.viewPlaceholderForTest().isEmpty()) {
                    qCritical("the recovery open left a placeholder: '%s'",
                        qUtf8Printable(window.viewPlaceholderForTest()));
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, bad] { window.openDataset(bad, true); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--cache-budget-smoke-test") {
        // Regression for cache-budget-exceeded-hard-fails-after-load: load a
        // 2-D dataset at finest, shrink the cache budget just below the finest
        // working set, then switch field to force a non-cache finest re-slice
        // that overflows the budget. With the fix the slice degrades to a lower
        // composite level (the level combo drops from "Finest available", -1);
        // without it the slice hard-fails and the level is unchanged.
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                auto* levels = window.findChild<QComboBox*>(
                    QStringLiteral("levelSelector"));
                auto* fields = window.findChild<QComboBox*>(
                    QStringLiteral("fieldSelector"));
                if (levels == nullptr || fields == nullptr
                    || fields->count() < 2
                    || levels->currentData().toInt() != -1) {
                    application.exit(1);  // expected finest (-1) with >=2 fields
                    return;
                }
                const auto resident = window.cacheResidentBytesForTest();
                if (resident == 0) {
                    application.exit(1);
                    return;
                }
                window.setCacheBudgetForTest(resident - 1);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&application, levels] {
                        // With the fix the overflowing finest re-slice fell back
                        // to a lower composite level, so the combo no longer
                        // reads "Finest available" (-1).
                        application.exit(
                            levels->currentData().toInt() != -1 ? 0 : 1);
                    });
                fields->setCurrentIndex(1);  // non-cache finest re-slice
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--slice-planes-smoke-test") {
        // View > Slice Planes on a 3-D plotfile: on by default, each panel
        // draws the two lines where the other planes cut it; off takes the
        // lines away, on brings them back, and I is its key.
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                const auto fail = [&application](const char* message) {
                    qCritical("%s", message);
                    application.exit(1);
                };
                const auto* action = window.findChild<QAction*>(
                    QStringLiteral("slicePlanesAction"));
                if (action == nullptr || !action->isEnabled()
                    || !action->shortcuts().contains(QKeySequence(Qt::Key_I))) {
                    fail("Slice Planes is not offered on a 3-D plotfile with I as its key");
                    return;
                }
                if (!window.slicePlanesVisibleForTest()
                    || window.activeViewCrosshairCountForTest() != 2) {
                    fail("the slice planes are not shown by default");
                    return;
                }
                window.setSlicePlanesVisibleForTest(false);
                if (window.activeViewCrosshairCountForTest() != 0) {
                    fail("switching the slice planes off left their lines");
                    return;
                }
                window.setSlicePlanesVisibleForTest(true);
                application.exit(window.activeViewCrosshairCountForTest() == 2 ? 0 : 3);
            });
        QTimer::singleShot(15000, &application, [&application] { application.exit(4); });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 5
        && std::string_view(argv[1]) == "--panel-layout-smoke-test") {
        // View > Panel Layout on a 3-D plotfile: F maximizes the active slice
        // panel and restores the grid, a zoomed panel keeps its framing, and
        // the submenu picks any panel. A maximized slice panel stays the
        // active one across a reopen and a 2-D -> 3-D sequence; a 2-D
        // plotfile offers neither control.
        const std::filesystem::path path3d(argv[2]);
        const std::filesystem::path second3d(argv[3]);
        const std::filesystem::path path2d(argv[4]);
        auto opened = std::make_shared<int>(0);
        const auto fail = [&application](const char* message) {
            qCritical("%s", message);
            application.exit(1);
        };
        const auto only = [&window](int panel) {
            for (int other = 0; other < 4; ++other) {
                if (window.panelWidgetForTest(other)->isVisibleTo(&window)
                    != (other == panel || panel == -1)) {
                    return false;
                }
            }
            return true;
        };
        // Still the region shown before the resize: contained in what is shown
        // now, and filling it along at least one axis.
        const auto sameFraming = [](const QRectF& before, const QRectF& after) {
            const auto slack = 0.02 * std::max(before.width(), before.height());
            return after.adjusted(-slack, -slack, slack, slack).contains(before)
                && std::min(after.width() / before.width(),
                       after.height() / before.height()) < 1.05;
        };
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, opened, fail, only, sameFraming, path3d,
                              second3d, path2d](bool success) {
                if (!success) {
                    fail("a plotfile did not open");
                    return;
                }
                auto* maximize = window.findChild<QAction*>(
                    QStringLiteral("maximizePanelAction"));
                if (maximize == nullptr) {
                    fail("Maximize Active Panel is missing");
                    return;
                }
                const auto stage = ++*opened;
                if (stage == 2) {
                    if (window.maximizedPanelForTest() != 0 || !only(0)
                        || window.navigationViewForTest() != window.panelWidgetForTest(0)) {
                        fail("reopening lost the maximized YZ panel as the active one");
                        return;
                    }
                    QTimer::singleShot(0, &window, [&window, path2d] { window.openDataset(path2d); });
                    return;
                }
                if (stage == 3) {
                    if (maximize->isEnabled()) {
                        fail("Maximize Active Panel is offered on a 2-D plotfile");
                        return;
                    }
                    QTimer::singleShot(0, &window, [&window, path3d, second3d] {
                        window.openSequence({path3d, second3d});
                    });
                    return;
                }
                if (!maximize->isEnabled()
                    || !maximize->shortcuts().contains(QKeySequence(Qt::Key_F))) {
                    fail("Maximize Active Panel is not offered on 3-D with F as its key");
                    return;
                }
                // Qt appends the display name; the title must not repeat it.
                if (window.windowTitle().contains(QGuiApplication::applicationDisplayName())) {
                    fail("the window title repeats the application name");
                    return;
                }
                window.activateWindow();
                window.setActiveViewForTest(1);
                window.navigationViewForTest()->setFocus();
                // Switching panels must leave the keyboard on the one shown:
                // on a toolbar control the arrow keys change the field.
                const auto focusOn = [&window](int panel) {
                    return QApplication::focusWidget() == window.panelWidgetForTest(panel);
                };
                auto* xz = window.panelWidgetForTest(1);
                const auto gridSize = xz->size();
                maximize->trigger();
                if (window.maximizedPanelForTest() != 1 || !only(1)
                    || xz->width() < gridSize.width() * 3 / 2
                    || xz->height() < gridSize.height() * 3 / 2) {
                    fail("F did not let the active XZ panel fill the grid");
                    return;
                }
                maximize->trigger();
                if (window.maximizedPanelForTest() != -1 || !only(-1)
                    || xz->size() != gridSize) {
                    fail("F again did not restore the grid");
                    return;
                }
                // Wheel zoom leaves the panel in a custom zoom, which a resize
                // alone would show at the same scale over more of the plane.
                const auto shown = [&window] {
                    const auto* view = window.navigationViewForTest();
                    return view->mapToScene(view->viewport()->rect()).boundingRect();
                };
                window.wheelActiveViewForTest(4);
                const auto zoomed = shown();
                maximize->trigger();
                if (!sameFraming(zoomed, shown())) {
                    fail("maximizing a zoomed panel changed the region it shows");
                    return;
                }
                maximize->trigger();
                if (!sameFraming(zoomed, shown())) {
                    fail("restoring a zoomed panel changed the region it shows");
                    return;
                }
                auto* isometric = window.findChild<QAction*>(
                    QStringLiteral("panelIsometricAction"));
                auto* xzChoice = window.findChild<QAction*>(QStringLiteral("panelXzAction"));
                auto* yz = window.findChild<QAction*>(QStringLiteral("panelYzAction"));
                if (isometric == nullptr || xzChoice == nullptr || yz == nullptr) {
                    fail("Panel Layout lacks the Isometric, XZ or YZ choice");
                    return;
                }
                isometric->trigger();
                if (!only(3) || !maximize->isChecked() || !focusOn(3)) {
                    fail("Isometric did not show and focus the isometric view alone");
                    return;
                }
                xzChoice->trigger();
                if (!only(1) || !focusOn(1)) {
                    fail("XZ after Isometric did not show and focus the XZ panel");
                    return;
                }
                yz->trigger();
                if (!only(0) || window.navigationViewForTest()
                        != window.panelWidgetForTest(0) || !focusOn(0)) {
                    fail("YZ after XZ did not show, activate and focus the YZ panel");
                    return;
                }
                window.setActiveViewForTest(2);  // reopening must not keep this
                QTimer::singleShot(0, &window, [&window, path3d] { window.openDataset(path3d); });
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application, fail, only](int index) {
                if (index != 0) {
                    return;
                }
                if (!only(0) || window.navigationViewForTest() != window.panelWidgetForTest(0)) {
                    fail("a 2-D -> 3-D sequence lost the maximized YZ panel");
                    return;
                }
                application.exit(0);
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [fail] { fail("a sequence frame failed"); });
        QTimer::singleShot(20000, &application, [&application] { application.exit(4); });
        QTimer::singleShot(0, &window, [&window, path3d] { window.openDataset(path3d); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--idle-ui-state-smoke-test") {
        // Two controls that are reachable before any dataset is, and used to
        // strand state there.
        //
        // The Animation panel: shown from the Window menu with nothing open, it
        // holds no controls at all, and an edge trigger on "does it apply"
        // never fired on the following open because the answer stayed false --
        // so an empty dock stayed parked for the session.
        //
        // Reset Zoom: reachable by its shortcut with nothing open, where it
        // iterates no views. Reporting from inside the per-view reset meant it
        // reported nothing, and the button kept a factor nothing applied.
        const std::filesystem::path path(argv[2]);
        window.setAnimationDockVisibleForTest(true);
        window.selectFixedScaleForTest(4);
        // Checked before the reset, which would mask it: applyFixedScale only
        // touches currentViews(), and setFixedScale early-returns on a view
        // with no image, so with nothing open the factor reaches no view and
        // claiming it puts a number on the button nothing backs.
        if (window.scaleUiLabelForTest() != QStringLiteral("Fit")) {
            qCritical("picking 4x from the View menu with no dataset left the "
                      "button at '%s'",
                qUtf8Printable(window.scaleUiLabelForTest()));
            return {true, 1};
        }
        // The toolbar menu is a separate call site with the same hazard.
        window.selectToolbarFixedScaleForTest(8);
        if (window.scaleUiLabelForTest() != QStringLiteral("Fit")) {
            qCritical("picking 8x from the toolbar with no dataset left the "
                      "button at '%s'",
                qUtf8Printable(window.scaleUiLabelForTest()));
            return {true, 1};
        }
        window.resetZoomAllViewsForTest();
        if (window.scaleUiLabelForTest() != QStringLiteral("Fit")) {
            qCritical("Reset Zoom with no dataset left the button at '%s'",
                qUtf8Printable(window.scaleUiLabelForTest()));
            return {true, 1};
        }
        window.setAnimationDockVisibleForTest(true);
        if (!window.animationDockVisibleForTest()) {
            qCritical("the Animation panel would not open with no dataset, so "
                      "this test proves nothing");
            return {true, 1};
        }
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                // A 2-D plotfile: no sweep controls, no sequence, so the panel
                // has nothing to show and must not stay up.
                if (window.animationDockVisibleForTest()) {
                    qCritical("an empty Animation panel survived an open");
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--window-close-pool-smoke-test") {
        // Regression for window-close-clears-shared-thread-pool: opening and
        // closing a second window must not discard the first window's queued
        // work on the shared global pool (which would strand it on
        // "Loading..." forever). Constrain the pool to one thread and occupy
        // that thread with a gate runnable, so this window's initial-load
        // worker is genuinely queued (not running) when the second window
        // closes. The pre-fix closeEvent called QThreadPool::clear(), which
        // dropped that queued worker; the fix keeps clear() off the per-window
        // path, so the worker survives, runs once the gate releases, and the
        // load completes. A watchdog fails instead of hanging on a strand.
        const std::filesystem::path path(argv[2]);
        auto* pool = QThreadPool::globalInstance();
        pool->setMaxThreadCount(1);
        auto gate = std::make_shared<std::atomic<bool>>(false);
        pool->start(QRunnable::create([gate] {
            while (!gate->load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }));
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&application](bool success) {
                application.exit(success ? 0 : 1);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, path, gate] {
            // Queue this window's metadata/initial-load worker behind the gate.
            window.openDataset(path);
            // A second window, opened and closed while that worker is still
            // queued. Its closeEvent runs synchronously here, so the pre-fix
            // clear() would drop the queued worker before the gate releases.
            auto* second = new amrvis::qt::MainWindow;
            second->show();
            second->close();
            second->deleteLater();
            // Release the gate so the surviving worker (with the fix) can run.
            gate->store(true);
        });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--close-window-smoke-test") {
        // File > Close Window closes only the window it was chosen from: the
        // second window goes away (its WA_DeleteOnClose delete is the proof)
        // and the first one is still up afterwards. quitOnLastWindowClosed is
        // off so that a regression closing every window cannot exit 0 through
        // Qt's own quit before the verdict below runs.
        const std::filesystem::path path(argv[2]);
        application.setQuitOnLastWindowClosed(false);
        // Every exit below sets this. An action wired to quit()/exit() instead
        // of close() unwinds exec() on the spot, so the verdict never runs and
        // the process would otherwise exit 0 -- passing on the very
        // regression this scenario exists to catch.
        auto decided = std::make_shared<bool>(false);
        QObject::connect(&application, &QCoreApplication::aboutToQuit,
            &application, [decided] {
                if (!*decided) {
                    qFatal("the run ended before the Close Window verdict: "
                        "the action quit the application instead of closing "
                        "its own window");
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, decided](bool success) {
                if (!success) {
                    qCritical("the initial slice failed");
                    *decided = true;
                    application.exit(1);
                    return;
                }
                auto* second = window.createNewWindowForTest();
                auto* action = second->findChild<QAction*>(
                    QStringLiteral("closeWindowAction"));
                if (action == nullptr || !action->isEnabled()) {
                    qCritical("the Close Window menu item is missing"
                        " or disabled");
                    *decided = true;
                    application.exit(1);
                    return;
                }
                if (action->shortcut()
                    != QKeySequence(Qt::CTRL | Qt::Key_W)) {
                    qCritical("Close Window carries the shortcut '%s'",
                        qUtf8Printable(action->shortcut().toString()));
                    *decided = true;
                    application.exit(1);
                    return;
                }
                QObject::connect(second, &QObject::destroyed, &application,
                    [&window, &application, decided] {
                        // The verdict waits a turn: quitting from inside the
                        // second window's destructor races its own teardown
                        // and segfaults or hangs about half the time. Nothing
                        // in the app quits from there -- a user's Quit is
                        // always a separate event -- so the delay keeps the
                        // harness on a reachable path. isVisible() is read in
                        // that turn rather than snapshotted here, so a close
                        // that takes the first window down one turn late is
                        // caught too.
                        QTimer::singleShot(0, &application,
                            [&window, &application, decided] {
                                *decided = true;
                                if (!window.isVisible()) {
                                    qCritical("Close Window closed the first"
                                        " window as well");
                                    application.exit(1);
                                    return;
                                }
                                window.close();
                                application.exit(0);
                            });
                    });
                action->trigger();
            });
        QTimer::singleShot(15000, &application, [&application, decided] {
            qCritical("the close-window scenario timed out");
            *decided = true;
            application.exit(1);
        });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--quit-smoke-test") {
        // Open a dataset, then quit through the main window once the initial
        // slice resolves (success or failure) and also mid-load. Passes if the
        // process exits promptly; a regression that blocks quit (an uncanceled
        // worker pinning the global pool, or a modal failure dialog) keeps it
        // alive until the watchdog fails the test.
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window](bool) {
                QTimer::singleShot(0, &window, [&window] { window.close(); });
            });
        QTimer::singleShot(300, &window, [&window] { window.close(); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 4 && std::string_view(argv[1]) == "--export-axes-smoke-test") {
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        const QString directory = QString::fromStdString(first.parent_path().string());
        // Test the actual PNG export, independent of installed encoders.
        qputenv("PATH", QByteArray());
        auto started = std::make_shared<bool>(false);
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed, &application,
                         [&window, directory, started](int index) {
                             if (index == 0 && !*started) {
                                 *started = true;
                                 window.startAnimationExportForTest(directory + "/axes.png", true,
                                                                    true, true);
                             }
                         });
        auto* poll = new QTimer(&application);
        QObject::connect(poll, &QTimer::timeout, &application,
                         [&window, &application, directory, poll] {
                             const auto firstFrames =
                                 QDir(directory).entryList({"axes*_00000.png"}, QDir::Files);
                             if (firstFrames.isEmpty())
                                 return;
                             for (const auto& name : firstFrames) {
                                 auto nextName = name;
                                 nextName.replace("_00000.png", "_00001.png");
                                 if (!QFileInfo::exists(directory + "/" + nextName))
                                     return;
                             }
                             bool valid = firstFrames.size() == 1 || firstFrames.size() == 3;
                             if (!valid) {
                                 qWarning("Export produced %lld panels, expected one or three",
                                          static_cast<long long>(firstFrames.size()));
                             }
                             for (const auto& name : firstFrames) {
                                 auto nextName = name;
                                 nextName.replace("_00000.png", "_00001.png");
                                 const QImage firstImage(directory + "/" + name);
                                 const QImage nextImage(directory + "/" + nextName);
                                 const bool samePixels = firstImage == nextImage;
                                 const bool panelValid = !firstImage.isNull() && samePixels &&
                                                         firstImage.pixelColor(0, 0).alpha() == 0 &&
                                                         firstImage.dotsPerMeterX() > 0;
                                 if (!panelValid) {
                                     qWarning(
                                         "Export mismatch (%s, Qt %s): sizes %dx%d / %dx%d, "
                                         "equal=%d, alpha=%d, dpm=%d / %d",
                                         qPrintable(name), qVersion(), firstImage.width(),
                                         firstImage.height(), nextImage.width(), nextImage.height(),
                                         samePixels,
                                         firstImage.isNull() ? -1
                                                             : firstImage.pixelColor(0, 0).alpha(),
                                         firstImage.dotsPerMeterX(), nextImage.dotsPerMeterX());
                                 }
                                 valid = valid && panelValid;
                             }
                             poll->stop();
                             window.close();
                             application.exit(valid ? 0 : 1);
                         });
        poll->start(10);
        QTimer::singleShot(15000, &application, [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
                           [&window, first, second] { window.openSequence({first, second}); });
    } else if (argc == 4 && std::string_view(argv[1]) == "--export-quit-smoke-test") {
        // Open a two-frame sequence, start an animation export (bypassing the
        // interactive color-bar/save dialogs), and quit the instant FFmpeg
        // encoding begins. With a hung stand-in ffmpeg on PATH the encoder
        // workers block, so shutdown stays alive unless they are cancelled and
        // their process terminated on close; the ctest timeout fails the test
        // if the process never exits.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        const QString outputPath = QString::fromStdString(
            (first.parent_path() / "anim.png").string());
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, outputPath](int index) {
                if (index == 0) {
                    window.startAnimationExportForTest(outputPath, false);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::exportEncodingStarted,
            &application, [&window] {
                QTimer::singleShot(0, &window, [&window] { window.close(); });
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else {
        return {false, std::nullopt};
    }
    return {true, std::nullopt};
}

} // namespace amrvis::qt::smoke
