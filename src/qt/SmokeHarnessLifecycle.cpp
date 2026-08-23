#include "SmokeHarnessInternal.hpp"

#include "MainWindow.hpp"

#include <QAction>
#include <QComboBox>
#include <QKeySequence>
#include <QRunnable>
#include <QThreadPool>
#include <QTimer>

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
            &application, [&application](bool success) {
                application.exit(success ? 0 : 1);
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
        && std::string_view(argv[1]) == "--idle-ui-state-smoke-test") {
        // Two controls that are reachable before any dataset is, and used to
        // strand state there.
        //
        // The Animation panel: shown from the View menu with nothing open, it
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
    } else if (argc == 4
        && std::string_view(argv[1]) == "--export-quit-smoke-test") {
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
    }
    else {
        return {false, std::nullopt};
    }
    return {true, std::nullopt};
}

} // namespace amrvis::qt::smoke
