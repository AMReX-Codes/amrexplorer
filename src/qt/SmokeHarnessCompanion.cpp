#include "SmokeHarnessInternal.hpp"

#include "MainWindow.hpp"

#include <QAction>
#include <QCheckBox>
#include <QLabel>
#include <QRectF>
#include <QTimer>
#include <QTreeWidget>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string_view>

// Companion: two plotfiles in one window (see MainWindowCompanion.cpp). The
// fixtures are an "atmosphere" over an "ocean" touching at z = 0: the upper
// one spans x in [-0.5, 1] with 0.25 cells, the lower x in [0, 1] with
// 0.0625-tall cells, so in cell counts both are four rows tall and the lower
// tile sits two cells in from the left under the upper.

namespace amrvis::qt::smoke {

namespace {

bool near(double actual, double expected)
{
    return std::abs(actual - expected) <= 1.0e-6 * std::max(1.0, std::abs(expected));
}

bool near(const QRectF& actual, const QRectF& expected)
{
    return near(actual.x(), expected.x()) && near(actual.y(), expected.y())
        && near(actual.width(), expected.width())
        && near(actual.height(), expected.height());
}

} // namespace

Outcome dispatchCompanion(Context& context)
{
    auto& application = context.application;
    auto& window = context.window;
    const int argc = context.argc;
    char** argv = context.argv;

    if (argc == 4 && std::string_view(argv[1]) == "--companion-smoke-test") {
        const std::filesystem::path upper(argv[2]);
        const std::filesystem::path lower(argv[3]);
        constexpr int xz = 1;
        constexpr int xy = 2;
        auto phase = std::make_shared<int>(0);
        auto quietSettles = std::make_shared<int>(0);
        const auto fail = [&application](const char* message) {
            qCritical("%s", message);
            application.exit(1);
        };
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, lower](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                if (window.companionOpen() || window.panelTileCountForTest(xz) != 1) {
                    qCritical("a lone dataset did not show one tile per panel");
                    application.exit(1);
                    return;
                }
                // The menu action comes alive with a plotfile that can take
                // a companion (it starts disabled, checked below).
                const auto* openAction = window.findChild<QAction*>(
                    QStringLiteral("openCompanionAction"));
                if (openAction == nullptr || !openAction->isEnabled()) {
                    qCritical("Open Companion Plotfile is not offered for an open plotfile");
                    application.exit(1);
                    return;
                }
                // Zoom the active (XY) panel first: opening a companion must
                // put its raster back to the whole domain, since zoom is
                // view-only with two datasets.
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled, &application,
                    [&window, lower] { window.openCompanion(lower); },
                    Qt::SingleShotConnection);
                window.rubberBandZoomActiveViewForTest();
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::companionOpenFinished,
            &application, [&window, &application, fail, phase, quietSettles, upper, lower](bool success) {
                if (*phase == 5) {
                    // The refused second companion: the first stays.
                    if (success || !window.companionOpen()
                        || window.panelTileCountForTest(xz) != 2) {
                        fail("a refused companion did not leave the first in place");
                        return;
                    }
                    // Replacing the companion keeps "Same as primary" and the
                    // position in the ocean; only Close resets them.
                    *phase = 6;
                    window.openCompanion(lower);
                    return;
                }
                if (*phase == 6) {
                    const auto* follow = window.findChild<QCheckBox*>(
                        QStringLiteral("companionFollowPrimary"));
                    if (!success || !window.companionOpen()
                        || window.panelTileCountForTest(xz) != 2) {
                        fail("the replacement companion did not open");
                        return;
                    }
                    if (follow == nullptr || !follow->isChecked()
                        || window.companionColorBarVisibleForTest()) {
                        fail("replacing the companion dropped Same as primary");
                        return;
                    }
                    if (!near(window.slicePositionForTest(2), -0.1)
                        || window.panelTileVisibleForTest(xy, 0)
                        || !window.panelTileVisibleForTest(xy, 1)) {
                        fail("replacing the companion moved the slice position");
                        return;
                    }
                    window.closeCompanion();
                    if (follow->isChecked()) {
                        fail("closing the companion left Same as primary ticked");
                        return;
                    }
                    if (window.companionOpen() || window.panelTileCountForTest(xz) != 1
                        || !window.panelTileVisibleForTest(xy, 0)) {
                        fail("closing the companion did not restore one tile");
                        return;
                    }
                    // The position was in the ocean; it comes back inside
                    // the primary's domain.
                    if (window.slicePositionForTest(2) < 0.0) {
                        fail("closing the companion left z outside the primary");
                        return;
                    }
                    auto* tree = window.findChild<QTreeWidget*>(
                        QStringLiteral("metadataTree"));
                    if (tree == nullptr
                        || !tree->findItems(QStringLiteral("Companion"),
                            Qt::MatchExactly).isEmpty()) {
                        fail("closing the companion left it in the metadata dock");
                        return;
                    }
                    application.exit(0);
                    return;
                }
                if (!success) {
                    fail("the companion did not open");
                    return;
                }
                // Both tiles on the XZ panel, the ocean directly under the
                // atmosphere and two cells in from its left edge.
                if (!window.companionOpen() || window.panelTileCountForTest(xz) != 2) {
                    fail("the XZ panel does not show two tiles");
                    return;
                }
                // The metadata dock lists both datasets.
                auto* metadataTree = window.findChild<QTreeWidget*>(
                    QStringLiteral("metadataTree"));
                if (metadataTree == nullptr
                    || metadataTree->findItems(QStringLiteral("Companion"),
                        Qt::MatchExactly).isEmpty()
                    || metadataTree->findItems(QStringLiteral("water"),
                        Qt::MatchExactly | Qt::MatchRecursive).isEmpty()) {
                    fail("the metadata dock does not list the companion");
                    return;
                }
                // The companion is named by its directory, however the path
                // was written.
                auto* label = window.findChild<QLabel*>(QStringLiteral("companionLabel"));
                if (label == nullptr || label->text() != QStringLiteral("lower:")) {
                    qCritical("companion label: '%s'",
                        label == nullptr ? "(none)" : qUtf8Printable(label->text()));
                    fail("the companion is not named by its directory");
                    return;
                }
                // Each layer renders its own field.
                if (window.layerFieldNameForTest(0, xz) != QStringLiteral("air")
                    || window.layerFieldNameForTest(1, xz) != QStringLiteral("water")) {
                    fail("the layers do not show their own fields");
                    return;
                }
                // The XY panel shows the layer that holds z: the midpoint of
                // the upper domain to begin with.
                if (!window.panelTileVisibleForTest(xy, 0)
                    || window.panelTileVisibleForTest(xy, 1)) {
                    fail("the XY panel does not show the upper layer alone");
                    return;
                }
                // Move z into the ocean: the XY panel flips to the companion
                // once the slices settle -- the same settle that brings the
                // primary's full-domain rasters back after the zoom.
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, fail, phase, quietSettles, upper] {
                        if (*phase == 0) {
                            *phase = 1;
                            if (window.panelTileVisibleForTest(xy, 0)
                                || !window.panelTileVisibleForTest(xy, 1)) {
                                fail("the XY panel did not flip to the ocean below z = 0");
                                return;
                            }
                            // Both tiles on the XZ panel, the ocean directly
                            // under the atmosphere and two cells in from its
                            // left edge; the zoom made before the open is
                            // gone, so the rasters cover their whole domains.
                            if (!near(window.panelTileRectForTest(xz, 0), QRectF(0.0, 0.0, 6.0, 4.0))
                                || !near(window.panelTileRectForTest(xz, 1), QRectF(2.0, 4.0, 4.0, 4.0))
                                || !near(window.panelTileRectForTest(xy, 0), QRectF(0.0, 0.0, 6.0, 4.0))) {
                                qCritical("XZ tiles: upper %gx%g at (%g,%g), lower %gx%g at (%g,%g)",
                                    window.panelTileRectForTest(xz, 0).width(),
                                    window.panelTileRectForTest(xz, 0).height(),
                                    window.panelTileRectForTest(xz, 0).x(),
                                    window.panelTileRectForTest(xz, 0).y(),
                                    window.panelTileRectForTest(xz, 1).width(),
                                    window.panelTileRectForTest(xz, 1).height(),
                                    window.panelTileRectForTest(xz, 1).x(),
                                    window.panelTileRectForTest(xz, 1).y());
                                fail("the tiles are not stacked at their physical positions");
                                return;
                            }
                            // Stretching the companion along z stretches only its tile.
                            window.setCompanionPerpendicularScaleForTest(2.0);
                            if (!near(window.panelTileRectForTest(xz, 0), QRectF(0.0, 0.0, 6.0, 4.0))
                                || !near(window.panelTileRectForTest(xz, 1), QRectF(2.0, 4.0, 4.0, 8.0))) {
                                fail("a companion z factor did not stretch only the lower tile");
                                return;
                            }
                            window.setCompanionPerpendicularScaleForTest(1.0);
                            // A fixed scale keeps both tiles on the pair's canvas.
                            window.selectFixedScaleForTest(2);
                            if (window.panelTileCountForTest(xz) != 2
                                || !near(window.panelTileRectForTest(xz, 1),
                                    QRectF(2.0, 4.0, 4.0, 4.0))) {
                                fail("a fixed scale moved the tiles off the pair's canvas");
                                return;
                            }
                            // Log is shared: the primary's box is the only one
                            // shown, and its click reaches the companion.
                            QCheckBox* logBox = nullptr;
                            for (auto* box : window.findChildren<QCheckBox*>()) {
                                if (box->text() != QStringLiteral("Log")
                                    || !box->isVisibleTo(&window)) {
                                    continue;
                                }
                                if (logBox != nullptr) {
                                    fail("two Log boxes are shown");
                                    return;
                                }
                                logBox = box;
                            }
                            if (logBox == nullptr) {
                                fail("no Log box is shown");
                                return;
                            }
                            logBox->click();
                            // Checked at once: the synthetic fields touch zero,
                            // so the arrivals fall back to linear and clear the
                            // boxes again, as they do for one dataset. What
                            // the toggle must do is reach the companion's
                            // next requests.
                            if (!window.layerLogarithmicSelectedForTest(0)
                                || !window.layerLogarithmicSelectedForTest(1)) {
                                fail("the Log toggle did not reach both layers");
                                return;
                            }
                            // Visible range re-colors every panel through the
                            // range sync; both tiles must survive it.
                            window.enableVisibleRasterForTest();
                            return;
                        }
                        if (*phase == 4) {
                            // A settle while the pair should be at rest: the
                            // follower and the primary's Visible sync are
                            // feeding each other.
                            ++*quietSettles;
                            return;
                        }
                        if (*phase == 3) {
                            *phase = 4;
                            const auto primaryRange = window.layerDisplayRangeForTest(0, xz);
                            const auto companionRange = window.layerDisplayRangeForTest(1, xz);
                            if (window.companionColorBarVisibleForTest()
                                || !near(companionRange.first, primaryRange.first)
                                || !near(companionRange.second, primaryRange.second)) {
                                fail("Same as primary did not give the companion the primary's range");
                                return;
                            }
                            // With the primary in Visible mode and the companion
                            // following it, nothing may keep re-slicing.
                            QTimer::singleShot(600, &window,
                                [&window, fail, phase, quietSettles, upper] {
                                    if (*quietSettles != 0) {
                                        fail("range following kept re-slicing at rest");
                                        return;
                                    }
                                    // A second companion that cannot pair (the
                                    // primary's own path overlaps it everywhere)
                                    // must be refused without disturbing the one
                                    // on show.
                                    *phase = 5;
                                    window.openCompanion(upper);
                                });
                            return;
                        }
                        if (*phase != 1) {
                            return;
                        }
                        *phase = 2;
                        if (window.panelTileCountForTest(xz) != 2
                            || !near(window.panelTileRectForTest(xz, 1),
                                QRectF(2.0, 4.0, 4.0, 4.0))) {
                            fail("the visible-range sync dropped a tile");
                            return;
                        }
                        // "Same as primary": the companion takes the primary's
                        // displayed range and its colour bar goes away.
                        auto* follow = window.findChild<QCheckBox*>(
                            QStringLiteral("companionFollowPrimary"));
                        if (follow == nullptr || !window.companionColorBarVisibleForTest()) {
                            fail("the companion's follow box or colour bar is missing");
                            return;
                        }
                        *phase = 3;
                        follow->click();
                    });
                window.setSlicePositionForTest(2, -0.1);
            });
        QTimer::singleShot(20000, &application, [&application] {
            qCritical("companion smoke test timed out");
            application.exit(4);
        });
        const auto* openAction = window.findChild<QAction*>(
            QStringLiteral("openCompanionAction"));
        if (openAction == nullptr || openAction->isEnabled()) {
            qCritical("Open Companion Plotfile is offered before any plotfile is open");
            return {true, 1};
        }
        QTimer::singleShot(0, &window, [&window, upper] { window.openDataset(upper); });
        return {true, std::nullopt};
    }
    return {false, std::nullopt};
}

} // namespace amrvis::qt::smoke
