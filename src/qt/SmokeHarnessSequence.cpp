#include "SmokeHarnessInternal.hpp"

#include "FabSelectorDock.hpp"
#include "MainWindow.hpp"

#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

// Sequence: the plotfile-sequence scenarios: open, no-op and failing
// sequences, sequences after a FAB, spec changes, the animation dock,
// transform / density / geometry preservation across frames. Every branch
// drives MainWindow through its ForTest accessors and arms connections and
// timers for main() to run; see SmokeHarness.hpp.

namespace amrvis::qt::smoke {

namespace {

// The sequence preserve smokes assert that the *data window* survives the
// step: rubberBandZoomActiveViewForTest selects the domain's centered half,
// and after the density change the view must still show exactly that window.
// They used to assert the transform differed from a fresh fit instead, but
// that proxy only held while the rubber-band feedback disturbed the viewport
// (transient scroll bars): once the arrival is framed cleanly, preserving a
// window that the raster covers exactly *is* numerically a fit, while a
// broken preserve (refit to the whole frame) still moves the window and
// fails here.
bool centeredHalfWindowPreserved(const amrvis::qt::MainWindow& window)
{
    const auto domain = window.datasetPhysicalDomainForTest();
    const auto shown = window.activeViewVisibleDataWindowForTest();
    if (domain.isEmpty() || shown.isEmpty()) {
        return false;
    }
    const QRectF expected(
        domain.left() + 0.25 * domain.width(),
        domain.top() + 0.25 * domain.height(),
        0.5 * domain.width(), 0.5 * domain.height());
    const auto tolerance
        = 0.02 * std::max(domain.width(), domain.height());
    return std::abs(shown.left() - expected.left()) <= tolerance
        && std::abs(shown.top() - expected.top()) <= tolerance
        && std::abs(shown.right() - expected.right()) <= tolerance
        && std::abs(shown.bottom() - expected.bottom()) <= tolerance;
}

} // namespace

Outcome dispatchSequence(Context& context)
{
    // The branches read these names as main() declared them; binding them
    // here keeps the moved code verbatim.
    auto& application = context.application;
    auto& window = context.window;
    const int argc = context.argc;
    char** argv = context.argv;

    if (argc == 4
        && std::string_view(argv[1]) == "--sequence-noop-smoke-test") {
        // Two sequence annoyances at once, because both are observed on the
        // same frame step. On frame 0: hide the Animation dock, then ask for
        // frame 0 again the way an idle slider press-and-release does. That
        // must not reload -- a reload would close the inspection windows,
        // cancel work, and re-render the frame already on screen -- so the
        // second displayed frame must be frame 1, not another frame 0. And on
        // reaching frame 1, the dock must still be hidden: a frame refresh has
        // no business reasserting the user's dock choice.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        auto displays = std::make_shared<std::vector<int>>();
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed, &application,
            [&window, &application, displays](int index) {
                displays->push_back(index);
                if (displays->size() == 1) {
                    if (index != 0) {
                        qCritical("sequence started on frame %d", index);
                        application.exit(1);
                        return;
                    }
                    window.setAnimationDockVisibleForTest(false);
                    window.requestSequenceFrameForTest(0);
                    // Do *not* step yet. Stepping immediately bumps the load
                    // generation and cancels the redundant frame-0 load before
                    // it can display, so the observed sequence is [0, 1]
                    // whether or not the request was suppressed -- which is to
                    // say the assertion below would pass against the bug it
                    // exists for. Give the reload time to arrive instead: if
                    // one was started, it displays frame 0 a second time and
                    // the branch below catches it.
                    //
                    // This margin is a timing assumption, and it fails open: a
                    // machine loaded enough to keep a redundant frame-0 reload
                    // of a small local fixture from displaying inside 500 ms
                    // would let this pass without testing anything. It is not
                    // the only cover. test_sequence_controller pins the same
                    // property deterministically by counting
                    // frameSwitchStarted, which is emitted synchronously
                    // exactly when a switch proceeds; what is left here is the
                    // end-to-end check that MainWindow's slider path reaches
                    // that suppression at all.
                    QTimer::singleShot(500, &window, [&window] {
                        window.stepSequence(1);
                    });
                    return;
                }
                if (index != 1 || displays->size() != 2) {
                    qCritical("a no-op frame request reloaded the frame");
                    application.exit(1);
                    return;
                }
                if (window.animationDockVisibleForTest()) {
                    qCritical("a frame refresh reopened the Animation dock");
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--sequence-failure-smoke-test") {
        // Playback wraps, so a frame that cannot be read comes back around
        // forever, raising a diagnostic every cycle. Start playing a sequence
        // whose second frame is unreadable and require playback to have
        // stopped by the time the failure is reported.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed, &application,
            [&window](int index) {
                if (index == 0 && !window.sequencePlayingForTest()) {
                    window.toggleSequencePlaybackForTest();
                }
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed, &application,
            [&window, &application] {
                // Queued: the failure handler stops playback around this
                // signal, so read the state once that handler has finished.
                QTimer::singleShot(0, &window, [&window, &application] {
                    if (window.sequencePlayingForTest()) {
                        qCritical("playback kept running past a failed frame");
                        application.exit(1);
                        return;
                    }
                    application.exit(0);
                });
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--sequence-smoke-test") {
        // Opens the two-frame sequence, waits for the first frame to display,
        // steps to frame 1 through the same slot the step button uses, and
        // exits 0 once frame 1 is on screen.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application](int index) {
                if (index == 0) {
                    if (window.particleSampleCountForTest() != 0) {
                        application.exit(1);
                        return;
                    }
                    // Opt in, start a frame load carrying that specification,
                    // then change it before the worker can complete. The final
                    // frame must reflect the new empty selection.
                    window.setParticleSelectionForTest({"Tracer"}, 1.0, 37);
                    window.stepSequence(1);
                    window.setParticleSelectionForTest({}, 1.0, 41);
                } else if (index == 1) {
                    application.exit(
                        window.particleSampleCountForTest() == 0
                                && window.particleSeedForTest() == 41
                            ? 0 : 1);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 5
        && std::string_view(argv[1]) == "--sequence-after-fab-smoke-test") {
        // Regression for open-sequence-stale-fab-state: open a raw FAB (enters
        // FAB mode -- selector dock visible, "— FAB" title suffix), then open a
        // plotfile sequence. openSequence does not go through openDatasetImpl,
        // so without the reset the FAB mode, dock, and title leak into the
        // frames. Exit 0 only if the first frame shows with the dock hidden and
        // no "— FAB" title.
        const std::filesystem::path fab(argv[2]);
        const std::filesystem::path first(argv[3]);
        const std::filesystem::path second(argv[4]);
        const auto inFabMode = [](const amrvis::qt::MainWindow& w) {
            const auto* selector = w.findChild<amrvis::qt::FabSelectorDock*>();
            return selector != nullptr && selector->isVisible()
                && w.windowTitle().endsWith(QStringLiteral(" FAB"));
        };
        auto opened = std::make_shared<bool>(false);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, first, second, opened,
                inFabMode](bool success) {
                if (*opened) {
                    return;  // later FAB re-slices are irrelevant
                }
                // Precondition: the raw FAB really did enter FAB mode, so the
                // sequence open below is exercising the leak.
                if (!success || !inFabMode(window)) {
                    application.exit(1);
                    return;
                }
                *opened = true;
                window.openSequence({first, second});
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application, inFabMode](int index) {
                if (index != 0) {
                    return;
                }
                application.exit(inFabMode(window) ? 1 : 0);
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, fab] { window.openDataset(fab); });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--sequence-spec-change-smoke-test") {
        // Start frame 1, then immediately drive an ordinary slice-affecting
        // control through scheduleSliceRequest while its worker is in flight.
        // The queued completion for the obsolete specification must not strand
        // the sequence in its in-flight state.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application](int index) {
                if (index == 0) {
                    window.stepSequence(1);
                    window.enableVisibleRasterForTest();
                } else if (index == 1) {
                    application.exit(0);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 5
        && std::string_view(argv[1]) == "--animation-dock-role-smoke-test") {
        // The Animation panel hosts two different sets of controls: the 3-D
        // slice sweep and the sequence transport. Hiding it while it holds the
        // sweep controls is not a standing refusal of the transport.
        //
        // Open a 3-D plotfile (the panel applies, and is shown), hide it, then
        // open a plotfile sequence. Testing one "applies" flag made that a
        // true -> true change, so neither the close nor the first frame was a
        // transition and the sequence arrived with its slider and play button
        // in a dock nothing would reopen.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        const std::filesystem::path bad(argv[4]);
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, first, second, bad, phase](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                if (!window.animationDockVisibleForTest()) {
                    qCritical("a 3-D dataset left the Animation panel hidden "
                              "(phase %d)", *phase);
                    application.exit(1);
                    return;
                }
                if (*phase == 0) {
                    // A failed open tears the 3-D dataset down, so the panel
                    // stops applying and must not stay up empty. The teardown's
                    // own call runs while the outgoing dataset is still
                    // installed, so it is not the one that can settle this.
                    *phase = 1;
                    QTimer::singleShot(0, &window,
                        [&window, bad] { window.openDataset(bad, true); });
                    return;
                }
                if (*phase == 2) {
                    // 3-D -> 3-D with the dock hidden. The teardown's
                    // closeSequence runs while the outgoing dataset is still
                    // installed, so the !applies branch that clears the flags
                    // never runs on this path; without an explicit reset the
                    // hide carried into the new dataset, while the same hide
                    // followed by a 2-D one reopened it.
                    *phase = 3;
                    window.setAnimationDockVisibleForTest(false);
                    QTimer::singleShot(0, &window,
                        [&window, first] { window.openDataset(first); });
                    return;
                }
                window.setAnimationDockVisibleForTest(false);
                QTimer::singleShot(0, &window, [&window, first, second] {
                    window.openSequence({first, second});
                });
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::datasetOpenFinished,
            &application, [&window, &application, first, phase](bool success) {
                if (*phase != 1) {
                    return;
                }
                if (success) {
                    qCritical("the bad path opened successfully");
                    application.exit(1);
                    return;
                }
                if (window.animationDockVisibleForTest()) {
                    qCritical("a failed open left an empty Animation panel up");
                    application.exit(1);
                    return;
                }
                *phase = 2;
                QTimer::singleShot(0, &window,
                    [&window, first] { window.openDataset(first); });
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed, &application,
            [&window, &application](int) {
                if (!window.animationDockVisibleForTest()) {
                    qCritical("a sequence opened with its transport controls "
                              "in a hidden Animation panel");
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
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--sequence-transform-preserve-smoke-test") {
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        auto before = std::make_shared<QRectF>();
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application, before](int index) {
                if (index == 0) {
                    window.wheelZoomAndPanActiveViewForTest();
                    *before = window.activeViewVisibleDataWindowForTest();
                    window.stepSequence(1);
                    return;
                }
                if (index != 1) {
                    return;
                }
                const auto after
                    = window.activeViewVisibleDataWindowForTest();
                const auto close = [](double lhs, double rhs) {
                    return std::fabs(lhs - rhs) <= 3.0e-2
                        * std::max({1.0, std::fabs(lhs), std::fabs(rhs)});
                };
                const bool preserved = !window.activeViewFitsWindowForTest()
                        && close(before->left(), after.left())
                        && close(before->top(), after.top())
                        && close(before->width(), after.width())
                        && close(before->height(), after.height());
                if (!preserved) {
                    std::fprintf(stderr,
                        "transform preservation mismatch: fit=%d "
                        "before=(%.17g,%.17g %.17gx%.17g) "
                        "after=(%.17g,%.17g %.17gx%.17g)\n",
                        window.activeViewFitsWindowForTest() ? 1 : 0,
                        before->x(), before->y(), before->width(),
                        before->height(), after.x(), after.y(), after.width(),
                        after.height());
                }
                application.exit(preserved ? 0 : 1);
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(2); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--sequence-density-preserve-smoke-test") {
        // Preserve a physical crop while moving from an 8x8 frame to an 8x12
        // frame. Pixel density changes, but the physical geometry is
        // compatible, so Custom mode must preserve the data window.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        auto zoomSettled = std::make_shared<bool>(false);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [&window, zoomSettled] {
                if (!*zoomSettled) {
                    *zoomSettled = true;
                    window.stepSequence(1);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application, zoomSettled](int index) {
                if (index == 0) {
                    window.rubberBandZoomActiveViewForTest();
                } else if (index == 1) {
                    application.exit(
                        *zoomSettled
                            && window.activeViewIsZoomedForTest()
                            && centeredHalfWindowPreserved(window)
                        ? 0 : 1);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--sequence-equal-size-transform-preserve-smoke-test") {
        // Exercise the timing edge directly: the first frame's full-domain
        // raster is 8x8. Rubber-band zoom changes the view transform and queues
        // a 4x4 crop, but stepping immediately cancels that work. The second
        // frame is 16x16, so its central-half crop is also 8x8. A size-only
        // transform policy must use physical compatibility rather than the
        // fresh dataset id or equal raster dimensions.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application](int index) {
                if (index == 0) {
                    window.rubberBandZoomActiveViewForTest();
                    window.stepSequence(1);
                } else if (index == 1) {
                    application.exit(
                        window.activeViewIsZoomedForTest()
                            && centeredHalfWindowPreserved(window)
                        ? 0 : 1);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--sequence-geometry-refit-smoke-test") {
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application](int index) {
                if (index == 0) {
                    window.wheelZoomAndPanActiveViewForTest();
                    window.stepSequence(1);
                } else if (index == 1) {
                    application.exit(
                        window.activeViewIsFitToWindowForTest() ? 0 : 1);
                }
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(2); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(3); });
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
