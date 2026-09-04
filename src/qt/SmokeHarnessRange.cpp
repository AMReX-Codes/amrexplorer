#include "SmokeHarnessInternal.hpp"

#include "AppSettings.hpp"
#include "MainWindow.hpp"

#include <amrexplorer/render2d/Contours.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

// Range: the display-state scenarios: range mode and range cache, contour
// sync, visible-range sync staleness, particles. Every branch drives
// MainWindow through its ForTest accessors and arms connections and timers
// for main() to run; see SmokeHarness.hpp.

namespace amrvis::qt::smoke {

namespace {

bool rangeSelectorMatches(
    const amrvis::qt::MainWindow& window, bool metadataRangesAvailable)
{
    const auto* selector = window.findChild<QComboBox*>(
        QStringLiteral("rangeModeSelector"));
    if (selector == nullptr) {
        return false;
    }
    const auto fileIndex = selector->findData(
        static_cast<int>(amrvis::qt::RangeMode::File));
    const auto levelIndex = selector->findData(
        static_cast<int>(amrvis::qt::RangeMode::Level));
    if (fileIndex < 0 || levelIndex < 0) {
        return false;
    }
    const auto fileEnabled = selector->model()->flags(
        selector->model()->index(fileIndex, 0)) & Qt::ItemIsEnabled;
    const auto levelEnabled = selector->model()->flags(
        selector->model()->index(levelIndex, 0)) & Qt::ItemIsEnabled;
    const auto expectedMode = metadataRangesAvailable
        ? amrvis::qt::RangeMode::File : amrvis::qt::RangeMode::Visible;
    return selector->currentData().toInt() == static_cast<int>(expectedMode)
        && static_cast<bool>(fileEnabled) == metadataRangesAvailable
        && static_cast<bool>(levelEnabled) == metadataRangesAvailable;
}

// Verifies the contour-sync smoke scenario after the re-slice batch settles.
// The three 3-D panels were sliced at asymmetric positions, so each has its
// own local range; Visible mode reconciles them into one shared range. The fix
// requires every panel's contour levels to come from that shared range. We
// check: all three panels agree on the (positive) shared range, and every
// contour level shown in any panel is one of contourValues(shared range) --
// which fails if a panel kept its local-range levels (the bug). A non-vacuous
// guard ensures contours actually rendered.
bool contourSyncMatches(amrvis::qt::MainWindow& window)
{
    const auto probes = window.contourViewProbesForTest();
    if (probes.size() != 3) {
        return false;
    }
    const auto within = [](double a, double b) {
        const auto scale = std::max({1.0, std::fabs(a), std::fabs(b)});
        return std::fabs(a - b) <= 1.0e-6 * scale;
    };
    // Log was requested and every slice is strictly positive, so log must have
    // applied and all panels must agree on the shared, positive Visible range.
    const auto& shared = probes.front();
    for (const auto& probe : probes) {
        if (!probe.logarithmic || !(probe.displayMinimum > 0.0)
            || !(probe.displayMinimum < probe.displayMaximum)
            || !within(probe.displayMinimum, shared.displayMinimum)
            || !within(probe.displayMaximum, shared.displayMaximum)) {
            return false;
        }
    }
    std::vector<double> expected;
    try {
        expected = amrvis::contourValues(
            shared.displayMinimum, shared.displayMaximum, 3, true);
    } catch (const std::exception&) {
        return false;
    }
    // Every level shown in any panel must be one of the shared-range levels;
    // the bug leaves a panel showing its own local-range levels instead. Track
    // which shared levels are actually drawn (map each shown level to its
    // canonical expected index -- a well-defined membership, unlike dedup by an
    // intransitive tolerance) so the pass is not vacuously met by empty
    // overlays: require >= 2 shared levels drawn across >= 2 panels.
    std::vector<bool> drawn(expected.size(), false);
    std::size_t panelsWithLevels = 0;
    for (const auto& probe : probes) {
        for (const auto level : probe.contourLevels) {
            std::size_t match = expected.size();
            for (std::size_t i = 0; i < expected.size(); ++i) {
                if (within(level, expected[i])) {
                    match = i;
                    break;
                }
            }
            if (match == expected.size()) {
                return false;  // a level not derived from the shared range
            }
            drawn[match] = true;
        }
        if (!probe.contourLevels.empty()) {
            ++panelsWithLevels;
        }
    }
    const auto sharedLevelsDrawn = static_cast<std::size_t>(
        std::count(drawn.begin(), drawn.end(), true));
    return panelsWithLevels >= 2 && sharedLevelsDrawn >= 2;
}

} // namespace

Outcome dispatchRange(Context& context)
{
    // The branches read these names as main() declared them; binding them
    // here keeps the moved code verbatim.
    auto& application = context.application;
    auto& window = context.window;
    const int argc = context.argc;
    char** argv = context.argv;

    if (argc == 3
        && std::string_view(argv[1]) == "--missing-range-smoke-test") {
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                const auto valid = success
                    && rangeSelectorMatches(window, false);
                application.exit(valid ? 0 : 1);
            });
        QTimer::singleShot(0, &window, [&window, path] {
            window.openDataset(path);
        });
    } else if (argc == 3 && std::string_view(argv[1]) == "--slice-smoke-test") {
        const std::filesystem::path path(argv[2]);
        const auto expectedScaleBarVisible = makeSettings().value(
            QStringLiteral("overlay/scaleBar"), false).toBool();
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, expectedScaleBarVisible](bool success) {
                const auto initialVisibilityMatches =
                    window.activeViewHasScaleBarForTest() == expectedScaleBarVisible;
                const auto actionEnabled =
                    window.scaleBarActionEnabledForTest();
                auto* lengthUnitsAction = window.findChild<QAction*>(
                    QStringLiteral("lengthUnitsAction"));
                if (lengthUnitsAction != nullptr) {
                    lengthUnitsAction->trigger();
                    QApplication::processEvents();
                }
                auto* lengthUnitsCombo = window.findChild<QComboBox*>(
                    QStringLiteral("lengthUnitsCombo"));
                const auto unitsUnsetByDefault = lengthUnitsCombo != nullptr
                    && lengthUnitsCombo->currentData().toString().isEmpty();
                if (auto* dialog = qobject_cast<QDialog*>(
                        lengthUnitsCombo == nullptr ? nullptr
                                                    : lengthUnitsCombo->window())) {
                    dialog->reject();
                }
                window.setScaleBarVisibleForTest(true);
                const auto shown = window.activeViewHasScaleBarForTest();
                window.setScaleBarVisibleForTest(false);
                const auto hidden = !window.activeViewHasScaleBarForTest();
                window.setScaleBarVisibleForTest(expectedScaleBarVisible);
                const auto valid = success && rangeSelectorMatches(window, true)
                    && actionEnabled && unitsUnsetByDefault && initialVisibilityMatches
                    && shown && hidden;
                application.exit(valid ? 0 : 1);
        });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--contour-sync-smoke-test") {
        // Once the initial load lands, switch to contours in Visible+log mode
        // with asymmetric per-panel slice positions (so the three panels have
        // unequal local ranges), then verify their contour levels once the
        // re-slice batch settles. See contourSyncMatches / the issue note.
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application] {
                        application.exit(contourSyncMatches(window) ? 0 : 1);
                    });
                // YZ(x)@i=3, XZ(y)@j=2, XY(z)@k=1 on the 4^3 cube q=(i+j+k)/9:
                // local ranges [3/9,1], [2/9,8/9], [1/9,7/9] -> shared [1/9,1].
                window.configureContourSyncForTest(
                    3, true, {0.875, 0.625, 0.375});
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--visible-sync-staleness-smoke-test") {
        // Regression for the visible-range sync staleness guard (all-or-nothing
        // form) and the single-flight dispatch gate. A panel re-rendered while a
        // sync runs makes the sync's outcome stale for it. The invalidation here
        // is a cache-path refresh (a contour-count change: refreshCachedSlice
        // keeps every plane pointer and bumps the per-view render generation),
        // so a guard keyed on plane identity would call the stale sync current
        // -- exactly the regression the generation stamp exists for. Drive that
        // path: gate a sync mid-flight, change the contour count (all three
        // panels re-render from cache with the new count, at their local
        // ranges), release the now-stale sync -- extracted at the OLD count --
        // and require it dropped and the panels' contours untouched while the
        // self-healing rerun is still gated; then release the rerun. Gate
        // coverage: a clean multi-panel setup batch drops nothing (skips == 0),
        // and a non-slice background request in flight must not hold a sync's
        // dispatch (the gate is on slice work only, never the global count).
        // Failure path: a worker that throws after being superseded is counted
        // stale, not reported; one that throws while still current is.
        constexpr int kSetupContours = 3;
        constexpr int kChangedContours = 7;
        static constexpr std::array<double, 3> kPositions{0.5, 0.5, 0.5};
        const std::filesystem::path path(argv[2]);
        auto* poll = new QTimer(&window);
        poll->setInterval(5);
        auto phase = std::make_shared<int>(0);
        auto attempts = std::make_shared<int>(0);
        // The active panel's render generation before the invalidating refresh,
        // so phase 2 can wait for the (debounced) re-render to actually land.
        auto refreshGen = std::make_shared<std::uint64_t>(0);
        // Reported-failure count before the two injected throws; the failure
        // phases assert on its delta (monotonic, unlike the status-bar text,
        // which clearMessage or a "Loading..." can overwrite between polls).
        auto baselineErrors = std::make_shared<int>(0);
        // Distinct contour levels per panel: before the change, and as the
        // refresh left them (the rerun must find them untouched).
        using LevelSets = std::vector<std::vector<double>>;
        auto before = std::make_shared<LevelSets>();
        auto refreshed = std::make_shared<LevelSets>();
        const auto levelSets = [&window] {
            LevelSets sets;
            for (const auto& probe : window.contourViewProbesForTest()) {
                auto levels = probe.contourLevels;
                std::sort(levels.begin(), levels.end());
                levels.erase(std::unique(levels.begin(), levels.end()),
                    levels.end());
                sets.push_back(std::move(levels));
            }
            return sets;
        };
        const auto finish = [&application, poll, &window](int code) {
            window.disarmVisibleSyncGateForTest();  // free any parked worker
            poll->stop();  // no stray timeout fires after we ask to exit
            application.exit(code);
        };
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application,
            [&window, &application, finish, poll, phase, before, levelSets](
                bool success) {
                if (!success) {
                    finish(1);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, finish, poll, phase, before,
                                      levelSets] {
                        if (*phase != 0) {
                            return;
                        }
                        *phase = 1;
                        // Single-flight gate coverage: the setup was a clean
                        // multi-panel batch, so one all-current sync ran and
                        // nothing was dropped. Per-arrival dispatch would have
                        // dropped the first (premature) sync.
                        if (window.visibleSyncStaleSkipsForTest() != 0) {
                            finish(7);
                            return;
                        }
                        *before = levelSets();
                        // Gate a sync so it cannot land before we invalidate
                        // the panels below.
                        window.armVisibleSyncGateForTest();
                        window.requestVisibleSyncForTest();
                        poll->start();
                    });
                // Into 3-D Visible + contours; its re-slice settles into a sync.
                window.configureContourSyncForTest(
                    kSetupContours, false, kPositions);
            });
        QObject::connect(poll, &QTimer::timeout, &application,
            [&window, finish, phase, attempts, refreshGen, before, refreshed,
                levelSets, baselineErrors] {
                if (++*attempts > 3000) {
                    finish(3);
                    return;
                }
                if (*phase == 1) {
                    if (!window.visibleSyncWorkerWaitingForTest()) {
                        return;  // wait for the gated sync to reach the gate
                    }
                    *phase = 2;
                    // Re-render every panel from cache while the sync is
                    // parked: same slice spec, new contour count. The planes
                    // keep their pointers; only the render generations move,
                    // which is what makes the parked sync's outcome stale.
                    *refreshGen = window.activeViewRenderGenerationForTest();
                    window.configureContourSyncForTest(
                        kChangedContours, false, kPositions);
                    return;
                }
                if (*phase == 2) {
                    if (window.activeViewRenderGenerationForTest() <= *refreshGen
                        || window.sliceRequestPendingForTest()
                        || window.slicesInFlightForTest() != 0) {
                        return;  // wait for the debounced re-render of all panels
                    }
                    *refreshed = levelSets();
                    // The change must be observable, else the check below is
                    // vacuous: every panel's level set differs from before.
                    if (refreshed->size() != 3 || *refreshed == *before) {
                        finish(9);
                        return;
                    }
                    *phase = 3;
                    window.releaseVisibleSyncGateForTest();  // let the stale sync land
                    return;
                }
                if (*phase == 3) {
                    // Nothing may touch the panels until we release the rerun:
                    // every panel must still show the refresh's contours. The
                    // stale outcome, extracted at the old count, is the only
                    // thing that could change them -- by being applied.
                    if (levelSets() != *refreshed) {
                        finish(6);
                        return;
                    }
                    // The stale sync must drop the whole outcome, then re-dispatch
                    // a rerun that parks at the gate. Wait for both.
                    if (window.visibleSyncStaleSkipsForTest() == 0
                        || !window.visibleSyncWorkerWaitingForTest()) {
                        return;
                    }
                    if (window.visibleSyncStaleSkipsForTest() != 1) {
                        finish(5);  // exactly one drop expected; more is a bug
                        return;
                    }
                    *phase = 4;
                    window.releaseVisibleSyncGateForTest();  // let the rerun apply
                    return;
                }
                if (*phase == 4) {
                    if (window.visibleSyncWorkerWaitingForTest()
                        || window.slicesInFlightForTest() != 0
                        || window.sliceRequestPendingForTest()) {
                        return;  // the rerun is still applying
                    }
                    // Gate coverage: with a non-slice background request
                    // outstanding (a particle load, say), a requested sync
                    // must still dispatch -- it gates on slice work alone.
                    window.adjustActiveRequestsForTest(1);
                    window.armVisibleSyncGateForTest();
                    window.requestVisibleSyncForTest();
                    *phase = 5;
                    *attempts = 0;
                    return;
                }
                if (*phase == 5) {
                    if (!window.visibleSyncWorkerWaitingForTest()) {
                        if (*attempts > 400) {
                            finish(8);  // a foreign request held the sync
                        }
                        return;
                    }
                    window.adjustActiveRequestsForTest(-1);
                    window.releaseVisibleSyncGateForTest();
                    *phase = 6;
                    return;
                }
                if (*phase == 6) {
                    if (window.visibleSyncWorkerWaitingForTest()
                        || window.slicesInFlightForTest() != 0
                        || window.sliceRequestPendingForTest()) {
                        return;
                    }
                    // Failure path, superseded: park a sync that will throw,
                    // re-render the panels under it (cache path again), release
                    // it. Its throw must be counted stale, not reported -- the
                    // panels it rendered are gone. (The self-healing rerun then
                    // parks.)
                    *baselineErrors = window.backgroundErrorCountForTest();
                    *refreshGen = window.activeViewRenderGenerationForTest();
                    window.armVisibleSyncGateForTest();
                    window.failNextVisibleSyncForTest();
                    window.requestVisibleSyncForTest();
                    *phase = 7;
                    return;
                }
                if (*phase == 7) {
                    if (!window.visibleSyncWorkerWaitingForTest()) {
                        return;
                    }
                    window.configureContourSyncForTest(
                        kSetupContours, false, kPositions);
                    *phase = 8;
                    return;
                }
                if (*phase == 8) {
                    if (window.activeViewRenderGenerationForTest() <= *refreshGen
                        || window.sliceRequestPendingForTest()
                        || window.slicesInFlightForTest() != 0) {
                        return;  // wait for the re-render of all panels
                    }
                    window.releaseVisibleSyncGateForTest();  // the throwing sync lands
                    *phase = 9;
                    return;
                }
                if (*phase == 9) {
                    // The rerun (armed by the re-render's arrival) parks next;
                    // it is dispatched from the throwing sync's completion, so
                    // by now that completion has decided whether to report.
                    if (!window.visibleSyncWorkerWaitingForTest()) {
                        return;
                    }
                    if (window.backgroundErrorCountForTest()
                        != *baselineErrors) {
                        finish(10);  // a superseded failure was reported
                        return;
                    }
                    window.releaseVisibleSyncGateForTest();  // let the rerun apply
                    *phase = 10;
                    return;
                }
                if (*phase == 10) {
                    if (window.visibleSyncWorkerWaitingForTest()
                        || window.slicesInFlightForTest() != 0
                        || window.sliceRequestPendingForTest()) {
                        return;
                    }
                    // Failure path, current: the same throw with nothing
                    // superseding it is the user's problem and is reported.
                    window.armVisibleSyncGateForTest();
                    window.failNextVisibleSyncForTest();
                    window.requestVisibleSyncForTest();
                    *phase = 11;
                    return;
                }
                if (*phase == 11) {
                    if (!window.visibleSyncWorkerWaitingForTest()) {
                        return;
                    }
                    window.releaseVisibleSyncGateForTest();
                    *phase = 12;
                    return;
                }
                if (*phase == 12) {
                    const auto reported = window.backgroundErrorCountForTest()
                        - *baselineErrors;
                    if (reported == 0) {
                        return;  // the completion has not run yet
                    }
                    if (reported != 1) {
                        finish(11);  // one throw, one report
                        return;
                    }
                    finish(0);
                }
            });
        QTimer::singleShot(20000, &application,
            [finish] { finish(4); });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--particle-visible-range-smoke-test") {
        // A shared Visible-range reconciliation replaces all three rasters.
        // Particle point batches must be restored after those setImage calls.
        const std::filesystem::path path(argv[2]);
        auto* poll = new QTimer(&window);
        poll->setInterval(10);
        auto attempts = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, poll](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                window.setParticleSelectionForTest({"Tracer"}, 1.0, 37);
                if (!window.particleLoadingForTest()
                    || !window.particleLoadingUiActiveForTest()) {
                    application.exit(2);
                    return;
                }
                poll->start();
            });
        QObject::connect(poll, &QTimer::timeout, &application,
            [&window, &application, poll, attempts] {
                if (++*attempts > 500) {
                    application.exit(1);
                    return;
                }
                if (window.particleSampleCountForTest() == 0
                    || window.particleOverlayCountForTest() == 0
                    || window.particleSeedForTest() != 37
                    || window.particleLoadingForTest()
                    || !window.particleLoadingUiSettledForTest()) {
                    return;
                }
                poll->stop();
                const QColor particleColor(12, 34, 56, 77);
                window.setParticleColorForTest("Tracer", particleColor);
                if (!window.particleOverlaysUseColorForTest(particleColor)) {
                    application.exit(1);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, particleColor] {
                        application.exit(
                            window.particleSampleCountForTest() > 0
                                && window.particleOverlayCountForTest() > 0
                                && window.particleSeedForTest() == 37
                                && window.particleOverlaysUseColorForTest(
                                    particleColor)
                            ? 0 : 1);
                    }, Qt::SingleShotConnection);
                window.enableVisibleRasterForTest();
            });
        QTimer::singleShot(0, &window, [&window, path] {
            window.openDataset(path);
        });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--particle-dialog-smoke-test") {
        // The particles dialog is modeless with an Apply button: settings are
        // meant to be tried against the image, so the dialog must not block the
        // main window, must survive Apply, and must not stack up copies of
        // itself when the menu item is chosen again. It also belongs to the
        // dataset whose species it lists, so opening a sequence -- which never
        // runs openDatasetImpl -- must take it down, as it must the contours
        // dialog beside it.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        // Both close with WA_DeleteOnClose, so a just-closed one can still be a
        // child of the window; the live dialog is the visible one.
        const auto liveNamedDialog
            = [&window](const QString& name) -> QDialog* {
            for (auto* candidate : window.findChildren<QDialog*>(name)) {
                if (candidate->isVisible()) {
                    return candidate;
                }
            }
            return nullptr;
        };
        const auto liveDialog = [liveNamedDialog]() {
            return liveNamedDialog(QStringLiteral("particlesDialog"));
        };
        auto* poll = new QTimer(&window);
        poll->setInterval(10);
        auto attempts = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, poll](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                auto* action = window.findChild<QAction*>(
                    QStringLiteral("particlesAction"));
                if (action == nullptr || !action->isEnabled()) {
                    qCritical("the particles menu item is missing or disabled");
                    application.exit(1);
                    return;
                }
                action->trigger();
                action->trigger();
                const auto dialogs = window.findChildren<QDialog*>(
                    QStringLiteral("particlesDialog"));
                if (dialogs.size() != 1) {
                    qCritical("expected one particles dialog, found %lld",
                        static_cast<long long>(dialogs.size()));
                    application.exit(1);
                    return;
                }
                auto* dialog = dialogs.front();
                if (!dialog->isVisible() || dialog->isModal()
                    || QApplication::activeModalWidget() != nullptr) {
                    qCritical("the particles dialog blocks the main window");
                    application.exit(1);
                    return;
                }
                auto* buttons = dialog->findChild<QDialogButtonBox*>(
                    QStringLiteral("particlesDialogButtons"));
                if (buttons == nullptr
                    || buttons->button(QDialogButtonBox::Apply) == nullptr) {
                    qCritical("the particles dialog has no Apply button");
                    application.exit(1);
                    return;
                }
                buttons->button(QDialogButtonBox::Apply)->click();
                // Apply draws the checked species and leaves the dialog up.
                if (!dialog->isVisible() || !window.particleLoadingForTest()) {
                    qCritical("Apply did not draw, or closed the dialog");
                    application.exit(1);
                    return;
                }
                poll->start();
            }, Qt::SingleShotConnection);
        // Let the Apply read finish rather than tearing the window down around
        // a live worker, then check the rest of the dialog's lifecycle.
        QObject::connect(poll, &QTimer::timeout, &application,
            [&window, &application, poll, attempts, liveDialog, liveNamedDialog,
                first, second] {
                if (++*attempts > 500) {
                    // exit() only flags the loop, so stop the timer too rather
                    // than report the same stall on every tick until it unwinds.
                    poll->stop();
                    qCritical("the particle read started by Apply never settled");
                    application.exit(1);
                    return;
                }
                if (window.particleLoadingForTest()) {
                    return;
                }
                poll->stop();
                auto* dialog = liveDialog();
                if (dialog == nullptr) {
                    qCritical("the dialog did not survive the particle read");
                    application.exit(1);
                    return;
                }
                auto* buttons = dialog->findChild<QDialogButtonBox*>(
                    QStringLiteral("particlesDialogButtons"));
                if (buttons == nullptr
                    || buttons->button(QDialogButtonBox::Ok) == nullptr) {
                    qCritical("the particles dialog has no Ok button");
                    application.exit(1);
                    return;
                }
                buttons->button(QDialogButtonBox::Ok)->click();
                if (liveDialog() != nullptr) {
                    qCritical("Ok did not close the dialog");
                    application.exit(1);
                    return;
                }
                auto* action = window.findChild<QAction*>(
                    QStringLiteral("particlesAction"));
                if (action == nullptr || !action->isEnabled()) {
                    qCritical("the particles menu item did not come back");
                    application.exit(1);
                    return;
                }
                action->trigger();
                if (liveDialog() == nullptr) {
                    qCritical("the dialog did not reopen");
                    application.exit(1);
                    return;
                }
                // The contours dialog is the other one bound to this dataset.
                auto* contoursAction = window.findChild<QAction*>(
                    QStringLiteral("contoursAction"));
                if (contoursAction == nullptr || !contoursAction->isEnabled()) {
                    qCritical("the contours menu item is missing or disabled");
                    application.exit(1);
                    return;
                }
                contoursAction->trigger();
                const auto contoursName = QStringLiteral("setContoursDialog");
                if (liveNamedDialog(contoursName) == nullptr) {
                    qCritical("the contours dialog did not open");
                    application.exit(1);
                    return;
                }
                // The species and fields they list belong to the outgoing
                // dataset, which a sequence open replaces.
                window.openSequence({first, second});
                if (liveDialog() != nullptr) {
                    qCritical("opening a sequence left the dialog on screen");
                    application.exit(1);
                    return;
                }
                if (liveNamedDialog(contoursName) != nullptr) {
                    qCritical(
                        "opening a sequence left the contours dialog on screen");
                    application.exit(1);
                    return;
                }
                window.close();
                application.exit(0);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window, [&window, first] {
            window.openDataset(first);
        });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--particle-settings-reset-smoke-test") {
        // Particle settings belong to the dataset they were chosen for. Both
        // paths that install a different one owe the same reset: a plain open,
        // and a sequence open, which reaches it through prepareSequence rather
        // than openDatasetImpl. Every setting resets, not just the species --
        // a subset chosen for a dense plotfile decimates the next one silently.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        const QColor chosenColor(12, 34, 56);
        // The startup point size is the default this compares against, so the
        // test does not have to name the constant.
        const auto defaultPointSize
            = std::make_shared<int>(window.particlePointSizeForTest());
        const auto choose = [&window, chosenColor] {
            window.setParticleSelectionForTest({"Tracer"}, 0.0005, 37);
            window.setParticlePointSizeForTest(9);
            window.setParticleSliceCellsOnlyForTest(true);
            window.setParticleColorForTest("Tracer", chosenColor);
            return window.particleSeedForTest() == 37
                && window.particleFractionForTest() == 0.0005
                && window.particlePointSizeForTest() == 9
                && window.particleSliceCellsOnlyForTest()
                && window.particleColorForTest("Tracer") == chosenColor;
        };
        const auto wasReset = [&window, chosenColor, defaultPointSize](
                                  const char* what) {
            if (window.particleSeedForTest() == 0
                && window.particleFractionForTest() == 1.0
                && window.particlePointSizeForTest() == *defaultPointSize
                && !window.particleSliceCellsOnlyForTest()
                && window.particleColorForTest("Tracer") != chosenColor) {
                return true;
            }
            qCritical("%s inherited particle settings: seed %llu, subset %g, "
                      "point size %d, slice cells only %d, colour %s",
                what,
                static_cast<unsigned long long>(window.particleSeedForTest()),
                window.particleFractionForTest(),
                window.particlePointSizeForTest(),
                static_cast<int>(window.particleSliceCellsOnlyForTest()),
                qUtf8Printable(
                    window.particleColorForTest("Tracer").name(QColor::HexArgb)));
            return false;
        };
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application,
            [&window, &application, first, second, phase, choose, wasReset](
                bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                if (*phase == 0) {
                    if (!choose()) {
                        qCritical("the particle settings did not take");
                        application.exit(1);
                        return;
                    }
                    // A plain open of a different plotfile resets them.
                    *phase = 1;
                    window.openDataset(second);
                    return;
                }
                if (!wasReset("a plain open")) {
                    application.exit(1);
                    return;
                }
                if (!choose()) {
                    qCritical("the particle settings did not take");
                    application.exit(1);
                    return;
                }
                // prepareSequence resets synchronously; frame 0 then arrives
                // through a different path, which must not put them back.
                window.openSequence({first, second});
                if (!wasReset("opening a sequence")) {
                    application.exit(1);
                }
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed, &application,
            [&window, &application, wasReset](int index) {
                if (index != 0) {
                    return;
                }
                if (!wasReset("the first sequence frame")) {
                    application.exit(1);
                    return;
                }
                window.close();
                application.exit(0);
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window, [&window, first] {
            window.openDataset(first);
        });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--particle-slice-cells-smoke-test") {
        // The end-to-end probe for the slice-cell filter. The unit tests pin
        // the geometry and the signal routing separately; only this says the
        // window really builds the slabs for the plane on show and hands them
        // to the projection. Turning it on must thin the drawn points and
        // leave the loaded samples alone -- it filters the drawing, it does
        // not resample -- and unticking it must restore the projection.
        const std::filesystem::path path(argv[2]);
        auto* poll = new QTimer(&window);
        poll->setInterval(10);
        auto attempts = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, poll](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                window.setParticleSelectionForTest({"Tracer"}, 1.0, 37);
                poll->start();
            });
        QObject::connect(poll, &QTimer::timeout, &application,
            [&window, &application, poll, attempts] {
                if (++*attempts > 500) {
                    application.exit(1);
                    return;
                }
                if (window.particleLoadingForTest()
                    || window.particleSampleCountForTest() == 0
                    || window.particleOverlayPointCountForTest() == 0) {
                    return;
                }
                poll->stop();
                const auto samples = window.particleSampleCountForTest();
                const auto projected = window.particleOverlayPointCountForTest();
                window.setParticleSliceCellsOnlyForTest(true);
                const auto filtered = window.particleOverlayPointCountForTest();
                // Before the count: a reload clears the samples and redraws
                // synchronously, so it would reach the branch below as a
                // plain "drew nothing" and report the wrong cause.
                if (window.particleSampleCountForTest() != samples) {
                    qCritical("the slice-cell filter reloaded the samples: "
                              "%llu, was %llu",
                        static_cast<unsigned long long>(
                            window.particleSampleCountForTest()),
                        static_cast<unsigned long long>(samples));
                    application.exit(1);
                    return;
                }
                if (filtered == 0 || filtered >= projected) {
                    qCritical("the slice-cell filter drew %llu of %llu points",
                        static_cast<unsigned long long>(filtered),
                        static_cast<unsigned long long>(projected));
                    application.exit(1);
                    return;
                }
                window.setParticleSliceCellsOnlyForTest(false);
                if (window.particleOverlayPointCountForTest() != projected) {
                    qCritical("unticking the filter did not restore the "
                              "projection through the volume");
                    application.exit(1);
                    return;
                }
                window.close();
                application.exit(0);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window, [&window, path] {
            window.openDataset(path);
        });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--range-cache-smoke-test") {
        // Regression for sequence-frame-range-cache-goes-stale: cache the
        // full-domain Visible range on frame 0, step to frame 1 (whose field is
        // 10x-scaled), zoom, and confirm the color bar tracks frame 1 instead
        // of reusing frame 0's cached range. Two signals interleave, sequenced
        // by a phase held in a shared_ptr so it outlives this branch scope.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        struct RangeCacheState { int phase = 0; double frame0Max = 0.0; };
        auto state = std::make_shared<RangeCacheState>();
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window](int index) {
                if (index == 0) {
                    window.enableVisibleRasterForTest();  // cache frame 0 range
                } else {
                    window.zoomActiveViewForTest();        // re-slice frame 1
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [&window, &application, state] {
                const auto probes = window.contourViewProbesForTest();
                if (probes.empty()) {
                    application.exit(1);
                    return;
                }
                const auto displayMax = probes.front().displayMaximum;
                if (state->phase == 0) {
                    state->frame0Max = displayMax;   // frame 0 full-domain max
                    state->phase = 1;
                    window.stepSequence(1);
                } else {
                    // Frame 1 is scaled 10x, so its zoomed max must far exceed
                    // frame 0's cached max. Reusing the stale cache (the bug)
                    // would instead leave them equal.
                    application.exit(
                        displayMax > 2.0 * state->frame0Max ? 0 : 1);
                }
            });
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
