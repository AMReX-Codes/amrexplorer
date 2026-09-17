#include "SmokeHarnessInternal.hpp"

#include "DerivedFieldStore.hpp"
#include "MainWindow.hpp"

#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QRectF>
#include <QSize>
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

// A selector row that is a field carries its id as item data; a greyed
// definition and the separator carry none.
bool rowIsField(const QComboBox& selector, const QString& name)
{
    const auto row = selector.findText(name);
    return row >= 0 && selector.itemData(row).isValid();
}

bool rowIsGreyed(const QComboBox& selector, const QString& name)
{
    const auto row = selector.findText(name);
    return row >= 0 && !selector.itemData(row).isValid()
        && selector.itemData(row, Qt::ToolTipRole).toString().contains(
            QStringLiteral("unavailable"));
}

// Derived fields over a pair: the window's list reaches the companion, each
// layer resolves what it can and greys the rest, Apply reloads both, and a
// reload keeps the companion's field by name.
void armCompanionDerivedChecks(amrvis::qt::MainWindow& window,
    QApplication& application, const std::filesystem::path& upper,
    const std::filesystem::path& lower)
{
    using amrvis::DerivedFieldDefinition;
    constexpr int xz = 1;
    auto phase = std::make_shared<int>(0);
    auto loads = std::make_shared<int>(0);
    auto companionOpens = std::make_shared<int>(0);
    auto ticks = std::make_shared<int>(0);
    auto quietSince = std::make_shared<int>(0);
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&window, &application, loads, lower](bool success) {
            if (!success) {
                application.exit(2);
                return;
            }
            if (++*loads == 1) {
                window.openCompanion(lower);
            }
        });
    QObject::connect(&window, &amrvis::qt::MainWindow::companionOpenFinished,
        &application, [&application, companionOpens](bool success) {
            if (!success) {
                qCritical("a companion open or reload failed");
                application.exit(2);
                return;
            }
            ++*companionOpens;
        });

    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, loads, companionOpens, ticks, quietSince,
            timer] {
            const auto finish = [&application, timer](int code) {
                timer->stop();
                application.exit(code);
            };
            const auto fail = [finish](const char* message) {
                qCritical("%s", message);
                finish(1);
            };
            ++*ticks;
            const bool settled = window.slicesInFlightForTest() == 0
                && !window.sliceRequestPendingForTest();
            auto* companionSelector = window.findChild<QComboBox*>(
                QStringLiteral("companionFieldSelector"));
            auto* primarySelector
                = window.findChild<QComboBox*>(QStringLiteral("fieldSelector"));
            auto* editor = window.findChild<QAction*>(
                QStringLiteral("expressionEditorAction"));
            if (companionSelector == nullptr || primarySelector == nullptr
                || editor == nullptr) {
                fail("the window is missing a selector or the editor action");
                return;
            }
            switch (*phase) {
            case 0:
                // The pair is up. The editor is available over it (the v1
                // cut lifted); a list reaches both layers.
                if (*companionOpens < 1 || !settled) {
                    return;
                }
                if (!window.companionOpen() || window.panelTileCountForTest(xz) != 2) {
                    fail("the companion did not open");
                    return;
                }
                if (!editor->isEnabled()) {
                    fail("the Expression Editor is unavailable with a companion open");
                    return;
                }
                *phase = 1;
                amrvis::qt::DerivedFieldStore::session().set(
                    {DerivedFieldDefinition{"wet", "water*2"},
                        DerivedFieldDefinition{"dry", "air*2"}});
                return;
            case 1:
                // The primary reloaded, then the companion: each lists what
                // it resolves and greys the other's definition.
                if (*loads < 2 || *companionOpens < 2 || !settled) {
                    return;
                }
                if (!rowIsField(*companionSelector, QStringLiteral("wet"))
                    || !rowIsGreyed(*companionSelector, QStringLiteral("dry"))) {
                    fail("the companion did not list wet as a field and dry greyed");
                    return;
                }
                if (!rowIsField(*primarySelector, QStringLiteral("dry"))
                    || !rowIsGreyed(*primarySelector, QStringLiteral("wet"))) {
                    fail("the primary did not list dry as a field and wet greyed");
                    return;
                }
                if (window.panelTileCountForTest(xz) != 2
                    || !near(window.panelTileRectForTest(xz, 1), QRectF(2.0, 4.0, 4.0, 4.0))
                    || window.backgroundErrorCountForTest() != 0) {
                    fail("the reloads disturbed the pair");
                    return;
                }
                *phase = 2;
                window.selectLayerFieldItemForTest(
                    1, companionSelector->findText(QStringLiteral("wet")));
                return;
            case 2:
                if (!settled) {
                    return;
                }
                if (window.layerFieldNameForTest(1, xz) != QStringLiteral("wet")) {
                    fail("the companion did not slice its derived field");
                    return;
                }
                // A changed definition reloads again; the field stays, by
                // name, across the reinstalled list -- and so do the pair's
                // zoom (this panel alone) and the companion's z factor.
                if (auto* sync = window.findChild<QAction*>(
                        QStringLiteral("syncRubberBandZoomAction"))) {
                    sync->setChecked(false);
                }
                window.rubberBandZoomPanelSceneForTest(xz, QRectF(1.0, 2.0, 4.0, 4.0));
                window.setCompanionPerpendicularScaleForTest(2.0);
                // One step up: the window is off the ocean's (now two-unit)
                // row edges, so its regions round outward past it.
                window.panStepActiveViewForTest(QPointF(0.0, 1.0));
                *phase = 3;
                amrvis::qt::DerivedFieldStore::session().set(
                    {DerivedFieldDefinition{"wet", "water*3"},
                        DerivedFieldDefinition{"dry", "air*2"}});
                return;
            case 3:
                if (*loads < 3 || *companionOpens < 3 || !settled) {
                    return;
                }
                if (window.layerSelectedFieldForTest(1) != QStringLiteral("wet")
                    || window.layerFieldNameForTest(1, xz) != QStringLiteral("wet")) {
                    fail("the reload did not keep the companion's field by name");
                    return;
                }
                // Three atmosphere rows over two ocean rows at twice their
                // height (the factor relaid the window out to the regions
                // before the step): the zoom and the factor both survived.
                if (window.rubberBandZoomedViewCountForTest() != 2
                    || !near(window.panelTileRectForTest(xz, 0), QRectF(1.0, 1.0, 4.0, 3.0))
                    || !near(window.panelTileRectForTest(xz, 1), QRectF(2.0, 4.0, 3.0, 4.0))) {
                    qCritical("XZ after reload: upper %gx%g at (%g,%g), lower %gx%g at (%g,%g), %zu zoomed",
                        window.panelTileRectForTest(xz, 0).width(),
                        window.panelTileRectForTest(xz, 0).height(),
                        window.panelTileRectForTest(xz, 0).x(),
                        window.panelTileRectForTest(xz, 0).y(),
                        window.panelTileRectForTest(xz, 1).width(),
                        window.panelTileRectForTest(xz, 1).height(),
                        window.panelTileRectForTest(xz, 1).x(),
                        window.panelTileRectForTest(xz, 1).y(),
                        window.rubberBandZoomedViewCountForTest());
                    fail("the reload dropped the pair's zoom or the companion's z factor");
                    return;
                }
                // The framed window is the panned one still, and so is the
                // export: neither rebuilt from the primary's part alone while
                // the companion's regions were being put back, nor from the
                // regions' outward-rounded union under an unchanged layout.
                if (!near(window.panelCanvasRectForTest(xz), QRectF(1.0, 1.0, 4.0, 6.0))
                    || window.panelExportSizeForTest(xz) != QSize(4, 6)) {
                    qCritical("after reload: canvas %gx%g at (%g,%g), export %dx%d",
                        window.panelCanvasRectForTest(xz).width(),
                        window.panelCanvasRectForTest(xz).height(),
                        window.panelCanvasRectForTest(xz).x(),
                        window.panelCanvasRectForTest(xz).y(),
                        window.panelExportSizeForTest(xz).width(),
                        window.panelExportSizeForTest(xz).height());
                    fail("the reload rebuilt the framed window from the primary alone");
                    return;
                }
                window.setCompanionPerpendicularScaleForTest(1.0);
                window.resetZoomAllViewsForTest();
                // A definition the companion can no longer resolve: greyed,
                // and the selection falls back to a field it has.
                *phase = 4;
                amrvis::qt::DerivedFieldStore::session().set(
                    {DerivedFieldDefinition{"wet", "nonesuch"}});
                return;
            case 4:
                if (*loads < 4 || *companionOpens < 4 || !settled) {
                    return;
                }
                if (!rowIsGreyed(*companionSelector, QStringLiteral("wet"))
                    || !companionSelector->currentData().isValid()
                    || window.layerFieldNameForTest(1, xz) != QStringLiteral("water")) {
                    fail("an unresolvable definition did not fall back to a stored field");
                    return;
                }
                // The same list again moves nothing: no reload of either.
                *phase = 5;
                *quietSince = *ticks;
                amrvis::qt::DerivedFieldStore::session().set(
                    {DerivedFieldDefinition{"wet", "nonesuch"}});
                return;
            case 5:
                if (*loads != 4 || *companionOpens != 4) {
                    fail("an unchanged list reloaded a layer");
                    return;
                }
                if (*ticks - *quietSince < 20) {
                    return;
                }
                finish(0);
                return;
            default:
                return;
            }
        });
    timer->start();
    QTimer::singleShot(20000, &application, [&application] {
        qCritical("companion derived-field smoke test timed out");
        application.exit(4);
    });
    QTimer::singleShot(0, &window, [&window, upper] { window.openDataset(upper); });
}

// Mapped grids over a pair: each dataset that carries node positions is
// drawn on them, on the pair's canvas about the interface (the fixtures'
// nodes stay inside their domains, so the tiles keep the pair's rects); a
// flat companion keeps its logical raster beside the warped primary.
void armMappedCompanionChecks(amrvis::qt::MainWindow& window,
    QApplication& application, const std::filesystem::path& upper,
    const std::filesystem::path& lower, bool lowerMapped)
{
    constexpr int xz = 1;
    constexpr int xy = 2;
    auto phase = std::make_shared<int>(-1);
    auto windowsBefore = std::make_shared<std::array<QRectF, 2>>();
    auto scaleBefore = std::make_shared<double>(0.0);
    const auto fail = [&window, &application](const char* message) {
        for (int tile = 0; tile < 2; ++tile) {
            const auto rect = window.panelTileRectForTest(xz, tile);
            const auto panel = window.mappedPanelForTest(xz, tile);
            qCritical("xz tile %d: [%g, %g, %g x %g] warped %d window [%g, %g, %g x %g] "
                      "device [%g, %g, %g x %g] image %d x %d fit %d",
                tile, rect.x(), rect.y(), rect.width(), rect.height(), panel.warped,
                panel.window.x(), panel.window.y(), panel.window.width(),
                panel.window.height(), panel.tileDevice.x(), panel.tileDevice.y(),
                panel.tileDevice.width(), panel.tileDevice.height(),
                panel.image.width(), panel.image.height(), panel.fit);
        }
        qCritical("%s", message);
        application.exit(1);
    };
    // A warped tile drawn for the screen: its pixmap is the device pixels it
    // covers, aligned to them (see --mapped-grid-smoke-test).
    const auto drawnForScreen = [&window](int layer) {
        const auto panel = window.mappedPanelForTest(xz, layer);
        const auto whole = [](double value) {
            return std::abs(value - std::round(value)) < 1e-6;
        };
        return panel.warped && !panel.window.isEmpty()
            && std::abs(panel.tileDevice.width() - panel.image.width()) < 1e-6
            && std::abs(panel.tileDevice.height() - panel.image.height()) < 1e-6
            && whole(panel.tileDevice.left()) && whole(panel.tileDevice.top());
    };
    // A warped tile is drawn for whole device pixels, so its rect holds its
    // band and reaches at most a pixel past it; a flat tile is its band.
    const auto onBand = [](const QRectF& tile, const QRectF& band) {
        constexpr double slack = 0.05;
        return tile.left() <= band.left() + 1e-6 && tile.left() > band.left() - slack
            && tile.top() <= band.top() + 1e-6 && tile.top() > band.top() - slack
            && tile.right() >= band.right() - 1e-6 && tile.right() < band.right() + slack
            && tile.bottom() >= band.bottom() - 1e-6 && tile.bottom() < band.bottom() + slack;
    };
    // Zoomed, a warped tile is drawn for what the viewport shows of it: it
    // holds the framed window's part over its band and stays on the band.
    const auto holds = [](const QRectF& tile, const QRectF& part) {
        return tile.left() <= part.left() + 1e-6 && tile.top() <= part.top() + 1e-6
            && tile.right() >= part.right() - 1e-6 && tile.bottom() >= part.bottom() - 1e-6;
    };
    const auto within = [](const QRectF& tile, const QRectF& band) {
        constexpr double slack = 0.05;
        return tile.left() > band.left() - slack && tile.top() > band.top() - slack
            && tile.right() < band.right() + slack && tile.bottom() < band.bottom() + slack;
    };
    const auto probed = [&window](int layer) {
        const auto image = window.mappedPanelForTest(xz, layer).image;
        const auto readout = window.probeReadoutPanelForTest(
            xz, layer, image.width() / 2, image.height() / 2);
        return readout.contains(QStringLiteral("value"))
            && readout.contains(QStringLiteral("cell"));
    };
    // A plain right click on a tile, as the mouse would deliver it.
    const auto rightClick = [&window](int layer) {
        const auto panel = window.mappedPanelForTest(xz, layer);
        const auto ratio = window.devicePixelRatioF();
        window.rightClickActiveViewForTest(QPoint(
            static_cast<int>((panel.tileDevice.left() + 0.4 * panel.tileDevice.width()) / ratio),
            static_cast<int>((panel.tileDevice.top() + 0.5 * panel.tileDevice.height()) / ratio)));
    };
    const auto runPhase = [&window, &application, phase, windowsBefore, scaleBefore, fail,
                              drawnForScreen, probed, onBand, holds, within, rightClick,
                              lowerMapped] {
        if (*phase < 0 || window.sliceRequestPendingForTest()
            || window.slicesInFlightForTest() > 0) {
            return;
        }
        const auto tile0 = window.panelTileRectForTest(xz, 0);
        const auto tile1 = window.panelTileRectForTest(xz, 1);
        switch (*phase) {
        case 0:
            if (!window.companionOpen() || window.panelTileCountForTest(xz) != 2) {
                fail("the companion did not open beside a mapped primary");
                return;
            }
            if (!window.displayIsMappedForTest() || !window.mappedGridMenuEnabledForTest()) {
                fail("the mapped grid is not shown with a companion open");
                return;
            }
            if (!drawnForScreen(0)
                || (lowerMapped ? !drawnForScreen(1)
                                : window.mappedPanelForTest(xz, 1).warped)) {
                fail("the pair's tiles are not the warps they should be");
                return;
            }
            if (!onBand(tile0, QRectF(0.0, 0.0, 6.0, 4.0))
                || !onBand(tile1, QRectF(2.0, 4.0, 4.0, 1.0))) {
                fail("the pair's tiles are not on their bands in physical size");
                return;
            }
            if (window.backgroundErrorCountForTest() != 0) {
                fail("the mapped pair reported an error");
                return;
            }
            *phase = 1;
            window.setCompanionPerpendicularScaleForTest(4.0);
            break;
        case 1:
            if (!onBand(tile0, QRectF(0.0, 0.0, 6.0, 4.0))
                || !onBand(tile1, QRectF(2.0, 4.0, 4.0, 4.0)) || !drawnForScreen(0)
                || (lowerMapped && !drawnForScreen(1))) {
                fail("a companion z factor did not stretch only the lower band");
                return;
            }
            *phase = 2;
            window.setSlicePositionForTest(2, -0.1);
            // The companion's slice for the new position is on its way: the
            // primary's tile stays on show until it lands, and is not sliced
            // again meanwhile (a slice at its face would replace it first).
            if (!window.panelTileVisibleForTest(xy, 0) || window.panelTileVisibleForTest(xy, 1)
                || !window.layerSliceOnItsWayForTest(xy, 1)
                || window.layerSliceOnItsWayForTest(xy, 0)) {
                fail("the XY panel switched to a stale companion tile");
                return;
            }
            break;
        case 2:
            if (window.panelTileVisibleForTest(xy, 0) || !window.panelTileVisibleForTest(xy, 1)
                || window.mappedPanelForTest(xy, 1).warped != lowerMapped) {
                fail("a slice position in the companion did not show its tile on XY");
                return;
            }
            *phase = 3;
            window.setSlicePositionForTest(2, 0.5);
            if (window.panelTileVisibleForTest(xy, 0) || !window.panelTileVisibleForTest(xy, 1)
                || !window.layerSliceOnItsWayForTest(xy, 0)
                || window.layerSliceOnItsWayForTest(xy, 1)) {
                fail("the XY panel switched to a stale primary tile");
                return;
            }
            break;
        case 3:
            if (!window.panelTileVisibleForTest(xy, 0) || window.panelTileVisibleForTest(xy, 1)
                || !window.mappedPanelForTest(xy, 0).warped) {
                fail("a slice position back in the primary did not show its warp on XY");
                return;
            }
            if (!probed(0) || !probed(1)) {
                fail("the probe does not read both tiles of a mapped pair");
                return;
            }
            // A wheel notch zooms the view: each warp is drawn again for
            // what it then shows, from the planes on hand.
            for (int layer = 0; layer < 2; ++layer) {
                (*windowsBefore)[static_cast<std::size_t>(layer)]
                    = window.mappedPanelForTest(xz, layer).window;
            }
            *phase = 4;
            window.setActiveViewForTest(xz);
            window.wheelActiveViewForTest(1);
            break;
        case 4: {
            const auto panel0 = window.mappedPanelForTest(xz, 0);
            const auto panel1 = window.mappedPanelForTest(xz, 1);
            if (panel0.fit || !drawnForScreen(0) || panel0.resliced
                || panel0.window == (*windowsBefore)[0]
                || (lowerMapped
                    && (!drawnForScreen(1) || panel1.resliced
                        || panel1.window == (*windowsBefore)[1]))) {
                fail("a wheel zoom did not redraw the warps for the screen");
                return;
            }
            // A rubber band across the interface frames its parts over the
            // layers: the warps follow the view, a flat companion re-slices
            // to its part.
            *phase = 5;
            window.rubberBandZoomPanelSceneForTest(xz, QRectF(1.0, 2.0, 4.0, 4.0));
            break;
        }
        case 5:
            if (!near(window.panelCanvasRectForTest(xz), QRectF(1.0, 2.0, 4.0, 4.0))
                || !drawnForScreen(0) || !holds(tile0, QRectF(1.0, 2.0, 4.0, 2.0))
                || !within(tile0, QRectF(0.0, 0.0, 6.0, 4.0))
                || window.mappedPanelForTest(xz, 0).resliced) {
                fail("a rubber band over a mapped pair did not frame the warp's part");
                return;
            }
            if (lowerMapped
                    ? (!drawnForScreen(1) || !holds(tile1, QRectF(2.0, 4.0, 3.0, 2.0))
                        || !within(tile1, QRectF(2.0, 4.0, 4.0, 4.0)))
                    : (!near(tile1, QRectF(2.0, 4.0, 3.0, 2.0))
                        || !window.mappedPanelForTest(xz, 1).resliced)) {
                fail("a rubber band over a mapped pair did not place the companion's part");
                return;
            }
            // A pan step moves the framed window against the drag, one scene
            // unit here, to the canvas edge; the warps follow.
            *phase = 6;
            window.panStepActiveViewForTest(QPointF(1.0, 0.0));
            break;
        case 6:
            if (!near(window.panelCanvasRectForTest(xz), QRectF(0.0, 2.0, 4.0, 4.0))
                || !drawnForScreen(0) || !holds(tile0, QRectF(0.0, 2.0, 4.0, 2.0))
                || !within(tile0, QRectF(0.0, 0.0, 6.0, 4.0))
                || (lowerMapped
                    && (!drawnForScreen(1) || !holds(tile1, QRectF(2.0, 4.0, 2.0, 2.0))
                        || !within(tile1, QRectF(2.0, 4.0, 4.0, 4.0))))) {
                fail("a pan step over a mapped pair did not move the framed window");
                return;
            }
            *phase = 7;
            window.resetZoomAllViewsForTest();
            break;
        case 7:
            if (!near(window.panelCanvasRectForTest(xz), QRectF(0.0, 0.0, 6.0, 8.0))
                || !drawnForScreen(0) || (lowerMapped && !drawnForScreen(1))) {
                fail("Fit over a mapped pair did not frame the whole canvas");
                return;
            }
            if (window.activeViewLineToolEnabledForTest()) {
                fail("the line tool is offered over a warped tile");
                return;
            }
            // Right clicks on one tile then the other move the slices; the
            // active layer changes on the same panel, and the view's scale
            // must not (a border that grew would redraw every warp).
            *scaleBefore = window.mappedPanelForTest(xz).scale;
            *phase = 8;
            rightClick(1);
            break;
        case 8:
            if (!(window.slicePositionForTest(2) < 0.0)
                || !near(window.mappedPanelForTest(xz).scale, *scaleBefore)
                || !drawnForScreen(0) || (lowerMapped && !drawnForScreen(1))) {
                fail("a right click on the companion moved the view's scale");
                return;
            }
            *phase = 9;
            rightClick(0);
            break;
        case 9:
            if (!(window.slicePositionForTest(2) > 0.0)
                || !near(window.mappedPanelForTest(xz).scale, *scaleBefore)
                || !drawnForScreen(0) || (lowerMapped && !drawnForScreen(1))) {
                fail("a right click on the primary moved the view's scale");
                return;
            }
            *phase = 10;
            window.closeCompanion();
            break;
        case 10: {
            const auto panel = window.mappedPanelForTest(xz);
            if (window.companionOpen() || window.panelTileCountForTest(xz) != 1
                || !drawnForScreen(0) || !panel.fit
                || !window.mappedGridMenuEnabledForTest()) {
                fail("closing the companion did not leave the primary's warp alone");
                return;
            }
            application.exit(0);
            break;
        }
        default:
            break;
        }
    };
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&window, &application, fail, lower](bool success) {
            if (!success) {
                application.exit(2);
                return;
            }
            if (!window.mappedGridMenuEnabledForTest()) {
                fail("Mapped Grid is not offered for the mapped upper plotfile");
                return;
            }
            // The primary on its grid first; the companion opens once its
            // warp has settled.
            QObject::connect(&window, &amrvis::qt::MainWindow::interactiveSlicesSettled,
                &application, [&window, lower] { window.openCompanion(lower); },
                Qt::SingleShotConnection);
            window.setMappedGridForTest(true);
        });
    QObject::connect(&window, &amrvis::qt::MainWindow::companionOpenFinished,
        &application, [&window, &application, phase, runPhase](bool success) {
            if (!success) {
                application.exit(3);
                return;
            }
            *phase = 0;
            QTimer::singleShot(0, &window, runPhase);
        });
    QObject::connect(&window, &amrvis::qt::MainWindow::interactiveSlicesSettled,
        &application, [&window, runPhase] { QTimer::singleShot(0, &window, runPhase); });
    QTimer::singleShot(60000, &application, [&application, phase] {
        qCritical("mapped companion smoke stalled in phase %d", *phase);
        application.exit(4);
    });
    QTimer::singleShot(0, &window, [&window, upper] { window.openDataset(upper); });
}

// Real zoom over a pair: a selection straddling the interface gives each
// layer the part in its own domain, both re-slice for it and land at their
// places, the panel frames the selection, and Reset Zoom puts both back.
void armCompanionZoomChecks(amrvis::qt::MainWindow& window,
    QApplication& application, const std::filesystem::path& upper,
    const std::filesystem::path& lower)
{
    constexpr int yz = 0;
    constexpr int xz = 1;
    constexpr int xy = 2;
    auto phase = std::make_shared<int>(0);
    auto companionOpens = std::make_shared<int>(0);
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&window, &application, lower](bool success) {
            if (!success) {
                application.exit(2);
                return;
            }
            window.openCompanion(lower);
        });
    QObject::connect(&window, &amrvis::qt::MainWindow::companionOpenFinished,
        &application, [&application, companionOpens](bool success) {
            if (!success) {
                qCritical("the companion did not open");
                application.exit(2);
                return;
            }
            ++*companionOpens;
        });
    const auto tilesAre = [&window](const QRectF& upperRect, const QRectF& lowerRect,
                              const QSize& upperSize, const QSize& lowerSize) {
        return near(window.panelTileRectForTest(xz, 0), upperRect)
            && near(window.panelTileRectForTest(xz, 1), lowerRect)
            && window.panelTileImageSizeForTest(xz, 0) == upperSize
            && window.panelTileImageSizeForTest(xz, 1) == lowerSize;
    };
    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, companionOpens, tilesAre, timer] {
            const auto finish = [&application, timer](int code) {
                timer->stop();
                application.exit(code);
            };
            const auto fail = [finish](const char* message) {
                qCritical("%s", message);
                finish(1);
            };
            const bool settled = window.slicesInFlightForTest() == 0
                && !window.sliceRequestPendingForTest();
            if (*companionOpens < 1 || !settled) {
                return;
            }
            auto* sync = window.findChild<QAction*>(
                QStringLiteral("syncRubberBandZoomAction"));
            if (sync == nullptr || !sync->isEnabled()) {
                fail("Sync Rubber-band Zoom is missing or disabled over a pair");
                return;
            }
            switch (*phase) {
            case 0:
                // The pair at rest: whole-domain tiles, one raster pixel per
                // cell, the canvas their union.
                if (!tilesAre(QRectF(0.0, 0.0, 6.0, 4.0), QRectF(2.0, 4.0, 4.0, 4.0),
                        QSize(6, 4), QSize(4, 4))
                    || !near(window.panelCanvasRectForTest(xz), QRectF(0.0, 0.0, 6.0, 8.0))) {
                    fail("the pair did not start at whole-domain tiles");
                    return;
                }
                // Two atmosphere rows above the interface and two ocean rows
                // below, four columns wide, starting west of the ocean. This
                // panel alone first.
                *phase = 1;
                sync->setChecked(false);
                window.rubberBandZoomPanelSceneForTest(xz, QRectF(1.0, 2.0, 4.0, 4.0));
                return;
            case 1:
                if (!tilesAre(QRectF(1.0, 2.0, 4.0, 2.0), QRectF(2.0, 4.0, 3.0, 2.0),
                        QSize(4, 2), QSize(3, 2))) {
                    qCritical("XZ tiles: upper %gx%g at (%g,%g) %dx%d, lower %gx%g at (%g,%g) %dx%d",
                        window.panelTileRectForTest(xz, 0).width(),
                        window.panelTileRectForTest(xz, 0).height(),
                        window.panelTileRectForTest(xz, 0).x(),
                        window.panelTileRectForTest(xz, 0).y(),
                        window.panelTileImageSizeForTest(xz, 0).width(),
                        window.panelTileImageSizeForTest(xz, 0).height(),
                        window.panelTileRectForTest(xz, 1).width(),
                        window.panelTileRectForTest(xz, 1).height(),
                        window.panelTileRectForTest(xz, 1).x(),
                        window.panelTileRectForTest(xz, 1).y(),
                        window.panelTileImageSizeForTest(xz, 1).width(),
                        window.panelTileImageSizeForTest(xz, 1).height());
                    fail("the selection did not re-slice each layer within its domain");
                    return;
                }
                if (!near(window.panelCanvasRectForTest(xz), QRectF(1.0, 2.0, 4.0, 4.0))
                    || window.rubberBandZoomedViewCountForTest() != 2
                    || !window.panelTileVisibleForTest(xz, 0)
                    || !window.panelTileVisibleForTest(xz, 1)) {
                    fail("the panel did not frame the selection over both tiles");
                    return;
                }
                // A selection inside the atmosphere alone: the ocean, missed,
                // goes back to its whole domain beside the framed window, and
                // an export renders the window, not the union of both tiles.
                *phase = 6;
                window.rubberBandZoomPanelSceneForTest(xz, QRectF(1.0, 1.0, 2.0, 2.0));
                return;
            case 6:
                if (!tilesAre(QRectF(1.0, 1.0, 2.0, 2.0), QRectF(2.0, 4.0, 4.0, 4.0),
                        QSize(2, 2), QSize(4, 4))
                    || window.panelExportSizeForTest(xz) != QSize(2, 2)) {
                    qCritical("export footprint %dx%d",
                        window.panelExportSizeForTest(xz).width(),
                        window.panelExportSizeForTest(xz).height());
                    fail("a zoomed pair's export was not cut to the framed window");
                    return;
                }
                // Synchronized: the other panels take the selection's extent
                // along the axis they share with this one -- x for XY, z for
                // YZ -- and keep their other axis whole. On XY only the layer
                // on show (the atmosphere, z being in it) takes part.
                *phase = 2;
                sync->setChecked(true);
                window.rubberBandZoomPanelSceneForTest(xz, QRectF(1.0, 2.0, 4.0, 4.0));
                return;
            case 2:
                if (!near(window.panelTileRectForTest(xy, 0), QRectF(1.0, 0.0, 4.0, 4.0))
                    || !near(window.panelTileRectForTest(xy, 1), QRectF(2.0, 0.0, 4.0, 4.0))
                    || window.panelTileImageSizeForTest(xy, 0) != QSize(4, 4)
                    || !near(window.panelTileRectForTest(yz, 0), QRectF(0.0, 2.0, 4.0, 2.0))
                    || !near(window.panelTileRectForTest(yz, 1), QRectF(0.0, 4.0, 4.0, 2.0))
                    || window.rubberBandZoomedViewCountForTest() != 5) {
                    fail("the synchronized selection did not reach the other panels");
                    return;
                }
                // An arrow step moves the framed window one scene unit -- one
                // cell of either layer here -- and both follow within their
                // domains: the ocean, which the window had entered two columns
                // in, now shows four.
                *phase = 3;
                window.rubberBandZoomPanelSceneForTest(xz, QRectF(1.0, 2.0, 4.0, 4.0));
                window.panStepActiveViewForTest(QPointF(-1.0, 0.0));
                return;
            case 3:
                if (!tilesAre(QRectF(2.0, 2.0, 4.0, 2.0), QRectF(2.0, 4.0, 4.0, 2.0),
                        QSize(4, 2), QSize(4, 2))
                    || !near(window.panelCanvasRectForTest(xz), QRectF(2.0, 2.0, 4.0, 4.0))) {
                    qCritical("XZ after a step: upper %gx%g at (%g,%g), lower %gx%g at (%g,%g)",
                        window.panelTileRectForTest(xz, 0).width(),
                        window.panelTileRectForTest(xz, 0).height(),
                        window.panelTileRectForTest(xz, 0).x(),
                        window.panelTileRectForTest(xz, 0).y(),
                        window.panelTileRectForTest(xz, 1).width(),
                        window.panelTileRectForTest(xz, 1).height(),
                        window.panelTileRectForTest(xz, 1).x(),
                        window.panelTileRectForTest(xz, 1).y());
                    fail("an arrow step did not shift the window for both layers");
                    return;
                }
                // A pan keeps the scale: at a fixed 2x a step moves the window
                // back and the view stays at 2x rather than refitting.
                *phase = 7;
                window.selectFixedScaleForTest(2);
                window.panStepActiveViewForTest(QPointF(1.0, 0.0));
                return;
            case 7:
                if (!tilesAre(QRectF(1.0, 2.0, 4.0, 2.0), QRectF(2.0, 4.0, 3.0, 2.0),
                        QSize(4, 2), QSize(3, 2))
                    || !window.fixedScaleStateMatchesForTest(2)) {
                    qCritical("scale after a fixed-scale pan: %g", window.activeViewScaleForTest());
                    fail("a pan over a pair changed the fixed scale");
                    return;
                }
                // A window in the ocean's band alone stops at the ocean's
                // western edge with its size kept, instead of being cut a
                // cell a step until nothing is left.
                *phase = 8;
                window.rubberBandZoomPanelSceneForTest(xz, QRectF(3.0, 5.0, 2.0, 2.0));
                return;
            case 8:
                if (!near(window.panelTileRectForTest(xz, 1), QRectF(3.0, 5.0, 2.0, 2.0))
                    || window.panelTileImageSizeForTest(xz, 1) != QSize(2, 2)
                    || !near(window.panelTileRectForTest(xz, 0), QRectF(0.0, 0.0, 6.0, 4.0))) {
                    fail("a selection in the ocean alone did not zoom the ocean only");
                    return;
                }
                *phase = 9;
                window.panStepActiveViewForTest(QPointF(1.0, 0.0));
                window.panStepActiveViewForTest(QPointF(1.0, 0.0));
                window.panStepActiveViewForTest(QPointF(1.0, 0.0));
                return;
            case 9:
                if (!near(window.panelTileRectForTest(xz, 1), QRectF(2.0, 5.0, 2.0, 2.0))
                    || window.panelTileImageSizeForTest(xz, 1) != QSize(2, 2)
                    || !near(window.panelCanvasRectForTest(xz), QRectF(2.0, 5.0, 2.0, 2.0))) {
                    qCritical("ocean tile after steps: %gx%g at (%g,%g)",
                        window.panelTileRectForTest(xz, 1).width(),
                        window.panelTileRectForTest(xz, 1).height(),
                        window.panelTileRectForTest(xz, 1).x(),
                        window.panelTileRectForTest(xz, 1).y());
                    fail("panning past the ocean's edge shrank the window");
                    return;
                }
                // Up two steps: the window crosses the interface, one row of
                // each layer under it.
                *phase = 10;
                window.panStepActiveViewForTest(QPointF(0.0, 1.0));
                window.panStepActiveViewForTest(QPointF(0.0, 1.0));
                return;
            case 10:
                if (!tilesAre(QRectF(2.0, 3.0, 2.0, 1.0), QRectF(2.0, 4.0, 2.0, 1.0),
                        QSize(2, 1), QSize(2, 1))
                    || !near(window.panelCanvasRectForTest(xz), QRectF(2.0, 3.0, 2.0, 2.0))) {
                    qCritical("after crossing: upper %gx%g at (%g,%g), lower %gx%g at (%g,%g)",
                        window.panelTileRectForTest(xz, 0).width(),
                        window.panelTileRectForTest(xz, 0).height(),
                        window.panelTileRectForTest(xz, 0).x(),
                        window.panelTileRectForTest(xz, 0).y(),
                        window.panelTileRectForTest(xz, 1).width(),
                        window.panelTileRectForTest(xz, 1).height(),
                        window.panelTileRectForTest(xz, 1).x(),
                        window.panelTileRectForTest(xz, 1).y());
                    fail("a pan did not cross the interface along the stacking axis");
                    return;
                }
                // A straddling window pushed down into the ocean's band alone,
                // where the domain is narrower: it moves inside the ocean's
                // edge at its size rather than losing a column, and coming
                // back up it is as wide as it was.
                *phase = 11;
                window.rubberBandZoomPanelSceneForTest(xz, QRectF(1.0, 3.0, 4.0, 2.0));
                return;
            case 11:
                if (!tilesAre(QRectF(1.0, 3.0, 4.0, 1.0), QRectF(2.0, 4.0, 3.0, 1.0),
                        QSize(4, 1), QSize(3, 1))) {
                    fail("a straddling one-row selection did not land");
                    return;
                }
                *phase = 12;
                window.panStepActiveViewForTest(QPointF(0.0, -1.0));
                return;
            case 12:
                if (!near(window.panelTileRectForTest(xz, 1), QRectF(2.0, 4.0, 4.0, 2.0))
                    || window.panelTileImageSizeForTest(xz, 1) != QSize(4, 2)
                    || !near(window.panelCanvasRectForTest(xz), QRectF(2.0, 4.0, 4.0, 2.0))) {
                    qCritical("into the ocean: lower %gx%g at (%g,%g), canvas %gx%g at (%g,%g)",
                        window.panelTileRectForTest(xz, 1).width(),
                        window.panelTileRectForTest(xz, 1).height(),
                        window.panelTileRectForTest(xz, 1).x(),
                        window.panelTileRectForTest(xz, 1).y(),
                        window.panelCanvasRectForTest(xz).width(),
                        window.panelCanvasRectForTest(xz).height(),
                        window.panelCanvasRectForTest(xz).x(),
                        window.panelCanvasRectForTest(xz).y());
                    fail("panning into the narrower domain shrank the window");
                    return;
                }
                *phase = 13;
                window.panStepActiveViewForTest(QPointF(0.0, 1.0));
                return;
            case 13:
                if (!tilesAre(QRectF(2.0, 3.0, 4.0, 1.0), QRectF(2.0, 4.0, 4.0, 1.0),
                        QSize(4, 1), QSize(4, 1))
                    || !near(window.panelCanvasRectForTest(xz), QRectF(2.0, 3.0, 4.0, 2.0))) {
                    fail("panning back out of the narrower domain kept a shrunk window");
                    return;
                }
                *phase = 4;
                window.resetZoomAllViewsForTest();
                return;
            case 4:
                if (!tilesAre(QRectF(0.0, 0.0, 6.0, 4.0), QRectF(2.0, 4.0, 4.0, 4.0),
                        QSize(6, 4), QSize(4, 4))
                    || !near(window.panelCanvasRectForTest(xz), QRectF(0.0, 0.0, 6.0, 8.0))
                    || window.rubberBandZoomedViewCountForTest() != 0) {
                    fail("Reset Zoom did not put both layers back");
                    return;
                }
                // Physical Size: the primary's cells (0.25 on every axis) set
                // the unit, so at 1x its tile is as it was and the ocean's
                // 0.0625 rows are a quarter unit each -- one unit for all four.
                window.setAspectModeForTest(amrvis::qt::AspectMode::PhysicalSize);
                window.selectFixedScaleForTest(1);
                if (!near(window.panelTileRectForTest(xz, 0), QRectF(0.0, 0.0, 6.0, 4.0))
                    || !near(window.panelTileRectForTest(xz, 1), QRectF(2.0, 4.0, 4.0, 1.0))
                    || !near(window.panelTransformScaleForTest(xz).first, 1.0)
                    || !near(window.panelTransformScaleForTest(xy).first, 1.0)) {
                    qCritical("physical 1x: lower %gx%g, XZ scale %g, XY scale %g",
                        window.panelTileRectForTest(xz, 1).width(),
                        window.panelTileRectForTest(xz, 1).height(),
                        window.panelTransformScaleForTest(xz).first,
                        window.panelTransformScaleForTest(xy).first);
                    fail("in Physical Size the primary does not set the pair's 1x");
                    return;
                }
                window.setAspectModeForTest(amrvis::qt::AspectMode::CellCounts);
                // On the XY panel only the atmosphere is on show (z is in
                // it): a selection there is its alone, the hidden ocean
                // neither snaps the frame out nor re-slices.
                *phase = 14;
                sync->setChecked(false);
                window.rubberBandZoomPanelSceneForTest(xy, QRectF(1.0, 1.0, 2.0, 2.0));
                return;
            case 14:
                if (!near(window.panelTileRectForTest(xy, 0), QRectF(1.0, 1.0, 2.0, 2.0))
                    || window.panelTileImageSizeForTest(xy, 0) != QSize(2, 2)
                    || !near(window.panelTileRectForTest(xy, 1), QRectF(2.0, 0.0, 4.0, 4.0))
                    || !near(window.panelCanvasRectForTest(xy), QRectF(1.0, 1.0, 2.0, 2.0))
                    || window.rubberBandZoomedViewCountForTest() != 1) {
                    qCritical("XY zoom: lower %gx%g at (%g,%g), %zu zoomed",
                        window.panelTileRectForTest(xy, 1).width(),
                        window.panelTileRectForTest(xy, 1).height(),
                        window.panelTileRectForTest(xy, 1).x(),
                        window.panelTileRectForTest(xy, 1).y(),
                        window.rubberBandZoomedViewCountForTest());
                    fail("the hidden layer took part in the XY panel's zoom");
                    return;
                }
                // Into the ocean: coming on show under the framed window, it
                // takes the window's part of its domain.
                *phase = 15;
                window.setSlicePositionForTest(2, -0.1);
                return;
            case 15:
                if (!window.panelTileVisibleForTest(xy, 1)
                    || !near(window.panelTileRectForTest(xy, 1), QRectF(2.0, 1.0, 1.0, 2.0))
                    || window.panelTileImageSizeForTest(xy, 1) != QSize(1, 2)) {
                    qCritical("XY after crossing: lower %gx%g at (%g,%g)",
                        window.panelTileRectForTest(xy, 1).width(),
                        window.panelTileRectForTest(xy, 1).height(),
                        window.panelTileRectForTest(xy, 1).x(),
                        window.panelTileRectForTest(xy, 1).y());
                    fail("the layer coming on show did not take the framed window");
                    return;
                }
                // Back in the atmosphere, a window west of the ocean's domain;
                // then into the ocean again: nothing of it lies under the
                // window, and the export is the empty window, not its tile.
                *phase = 16;
                window.setSlicePositionForTest(2, 0.5);
                return;
            case 16:
                *phase = 17;
                window.rubberBandZoomPanelSceneForTest(xy, QRectF(0.0, 1.0, 1.0, 2.0));
                return;
            case 17:
                if (!near(window.panelTileRectForTest(xy, 0), QRectF(0.0, 1.0, 1.0, 2.0))) {
                    fail("a selection west of the ocean did not zoom the atmosphere");
                    return;
                }
                *phase = 18;
                window.setSlicePositionForTest(2, -0.1);
                return;
            case 18:
                if (!window.panelTileVisibleForTest(xy, 1)
                    || !near(window.panelTileRectForTest(xy, 1), QRectF(2.0, 0.0, 4.0, 4.0))
                    || window.panelExportSizeForTest(xy) != QSize(1, 2)) {
                    qCritical("empty window export %dx%d",
                        window.panelExportSizeForTest(xy).width(),
                        window.panelExportSizeForTest(xy).height());
                    fail("an empty framed window exported the tile outside it");
                    return;
                }
                finish(0);
                return;
            default:
                return;
            }
        });
    timer->start();
    QTimer::singleShot(20000, &application, [&application] {
        qCritical("companion zoom smoke test timed out");
        application.exit(4);
    });
    QTimer::singleShot(0, &window, [&window, upper] { window.openDataset(upper); });
}

// The mixed pairs: a remote companion beside a local primary over the
// window's remote session, then a local companion beside a remote primary.
// Each layer is asked which it is; the pair lays out and zooms the same.
void armMixedCompanionChecks(Context& context, const std::string& upper,
    const std::string& lower)
{
    auto& window = context.window;
    auto& application = context.application;
    constexpr int xz = 1;
    auto phase = std::make_shared<int>(0);
    auto loads = std::make_shared<int>(0);
    auto companionOpens = std::make_shared<int>(0);
    context.server = std::make_shared<amrvis::remote::Server>();
    context.serverThread.emplace([server = context.server] { server->run(); });
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&application, loads](bool success) {
            if (!success) {
                application.exit(2);
                return;
            }
            ++*loads;
        });
    QObject::connect(&window, &amrvis::qt::MainWindow::companionOpenFinished,
        &application, [&application, companionOpens](bool success) {
            if (!success) {
                qCritical("a mixed companion did not open");
                application.exit(2);
                return;
            }
            ++*companionOpens;
        });
    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, loads, companionOpens, timer, upper, lower] {
            const auto finish = [&application, timer](int code) {
                timer->stop();
                application.exit(code);
            };
            const auto fail = [finish](const char* message) {
                qCritical("%s", message);
                finish(1);
            };
            const bool settled = window.slicesInFlightForTest() == 0
                && !window.sliceRequestPendingForTest();
            if (!settled) {
                return;
            }
            const auto* remoteAction = window.findChild<QAction*>(
                QStringLiteral("openRemoteCompanionAction"));
            switch (*phase) {
            case 0:
                // A local primary: both companion actions are offered; the
                // remote one opens over the window's remote session.
                if (*loads < 1) {
                    return;
                }
                if (remoteAction == nullptr || !remoteAction->isEnabled()) {
                    fail("Open Remote Companion Plotfile is not offered for a local plotfile");
                    return;
                }
                *phase = 1;
                window.openRemoteCompanion(lower);
                return;
            case 1:
                if (*companionOpens < 1) {
                    return;
                }
                if (!window.companionOpen() || window.panelTileCountForTest(xz) != 2
                    || window.layerSessionIsRemoteForTest(0)
                    || !window.layerSessionIsRemoteForTest(1)
                    || !near(window.panelTileRectForTest(xz, 0), QRectF(0.0, 0.0, 6.0, 4.0))
                    || !near(window.panelTileRectForTest(xz, 1), QRectF(2.0, 4.0, 4.0, 4.0))
                    || window.layerFieldNameForTest(1, xz) != QStringLiteral("water")) {
                    fail("a remote companion did not open beside the local primary");
                    return;
                }
                // Visible range on both, whole-domain first so each layer
                // stores its own full-domain range: the layers' ids can
                // coincide (a local primary's is the window's generation, a
                // remote companion's the server's counter), and a zoomed
                // slice must meet its own cached range, not the other's.
                {
                    auto* primaryMode = window.findChild<QComboBox*>(
                        QStringLiteral("rangeModeSelector"));
                    auto* companionMode = window.findChild<QComboBox*>(
                        QStringLiteral("companionrangeModeSelector"));
                    if (primaryMode == nullptr || companionMode == nullptr) {
                        fail("a range mode selector is missing");
                        return;
                    }
                    const auto visible = static_cast<int>(amrvis::RangeMode::Visible);
                    primaryMode->setCurrentIndex(primaryMode->findData(visible));
                    companionMode->setCurrentIndex(companionMode->findData(visible));
                }
                *phase = 6;
                return;
            case 6:
                // A selection across the interface: the local layer snaps to
                // its cells, the remote one keeps the exact window; here both
                // land on cell edges.
                if (auto* sync = window.findChild<QAction*>(
                        QStringLiteral("syncRubberBandZoomAction"))) {
                    sync->setChecked(false);
                }
                *phase = 2;
                window.rubberBandZoomPanelSceneForTest(xz, QRectF(1.0, 2.0, 4.0, 4.0));
                return;
            case 2: {
                if (!near(window.panelTileRectForTest(xz, 0), QRectF(1.0, 2.0, 4.0, 2.0))
                    || !near(window.panelTileRectForTest(xz, 1), QRectF(2.0, 4.0, 3.0, 2.0))
                    || window.panelTileImageSizeForTest(xz, 0) != QSize(4, 2)) {
                    fail("a mixed pair did not zoom each layer within its domain");
                    return;
                }
                const auto air = window.layerDisplayRangeForTest(0, xz);
                const auto water = window.layerDisplayRangeForTest(1, xz);
                if (air == water) {
                    qCritical("both layers show [%g, %g]", air.first, air.second);
                    fail("the companion took the primary's cached range");
                    return;
                }
                // The other way round: a remote primary (which closes the
                // companion, as any open does) and a local companion.
                *phase = 3;
                window.openRemoteDataset(upper);
                return;
            }
            case 3:
                if (*loads < 2) {
                    return;
                }
                if (window.companionOpen()) {
                    fail("opening another dataset kept the companion");
                    return;
                }
                // A remote fixed scale first: its whole-domain virtual canvas
                // must not survive into the pair (the export would follow it).
                *phase = 4;
                window.selectFixedScaleForTest(1);
                window.openCompanion(std::filesystem::path(lower));
                return;
            case 4:
                if (*companionOpens < 2) {
                    return;
                }
                if (!window.companionOpen() || window.panelTileCountForTest(xz) != 2
                    || !window.layerSessionIsRemoteForTest(0)
                    || window.layerSessionIsRemoteForTest(1)
                    || !near(window.panelTileRectForTest(xz, 1), QRectF(2.0, 4.0, 4.0, 4.0))
                    || window.backgroundErrorCountForTest() != 0) {
                    fail("a local companion did not open beside the remote primary");
                    return;
                }
                *phase = 5;
                window.rubberBandZoomPanelSceneForTest(xz, QRectF(1.0, 1.0, 2.0, 2.0));
                return;
            case 5:
                if (window.activeViewVirtualCanvasActiveForTest()
                    || window.panelExportSizeForTest(xz) != QSize(2, 2)) {
                    qCritical("export footprint %dx%d, virtual canvas %d",
                        window.panelExportSizeForTest(xz).width(),
                        window.panelExportSizeForTest(xz).height(),
                        window.activeViewVirtualCanvasActiveForTest() ? 1 : 0);
                    fail("a remote fixed scale's virtual canvas outlived the pair");
                    return;
                }
                // A fixed scale chosen over the pair has no virtual canvas;
                // then this panel zoomed again, so the other two are at the
                // fixed scale and this one is not.
                window.selectFixedScaleForTest(1);
                if (window.activeViewVirtualCanvasActiveForTest()) {
                    fail("a fixed scale over a pair made a virtual canvas");
                    return;
                }
                *phase = 7;
                window.rubberBandZoomPanelSceneForTest(xz, QRectF(1.0, 1.0, 2.0, 2.0));
                return;
            case 7:
                // Closing the companion puts each panel still at the fixed
                // scale back on its virtual canvas, and leaves the zoomed one
                // zoomed.
                window.closeCompanion();
                if (window.companionOpen() || !window.panelVirtualCanvasActiveForTest(0)
                    || !window.panelVirtualCanvasActiveForTest(2)
                    || window.panelVirtualCanvasActiveForTest(xz)
                    || !window.activeViewIsZoomedForTest()) {
                    fail("closing the companion did not restore the remote fixed scale "
                         "per panel");
                    return;
                }
                finish(0);
                return;
            default:
                return;
            }
        });
    timer->start();
    QTimer::singleShot(20000, &application, [&application] {
        qCritical("mixed companion smoke test timed out");
        application.exit(4);
    });
    QTimer::singleShot(0, &window, [&window, upper, server = context.server] {
        attachSmokeServer(window, server);
        window.openDataset(std::filesystem::path(upper));
    });
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
                // Zoom the active (XY) panel first: opening a companion starts
                // both datasets at the whole domain, so the raster's region
                // must go (a pair's zoom is the panel's; see the zoom smoke).
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
                    // position in the ocean; only Close resets them. The z
                    // factor is the replaced companion's own and does not carry.
                    *phase = 6;
                    window.setCompanionPerpendicularScaleForTest(2.0);
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
                    if (!near(window.panelTileRectForTest(xz, 1), QRectF(2.0, 4.0, 4.0, 4.0))) {
                        fail("replacing the companion kept the old one's z factor");
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
                            // gone, so both rasters cover their whole domains.
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
                            // following it, nothing may keep re-slicing. This
                            // relies on a settle meaning the queue is empty too
                            // (settleIfDrained): a settle sent with the
                            // follower's request still behind the debounce
                            // made this fail on slow CI runners.
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
    if (argc == 4 && std::string_view(argv[1]) == "--mixed-companion-smoke-test") {
        armMixedCompanionChecks(context, argv[2], argv[3]);
        return {true, std::nullopt};
    }
    if (argc == 4 && std::string_view(argv[1]) == "--mapped-companion-smoke-test") {
        armMappedCompanionChecks(window, application,
            std::filesystem::path(argv[2]), std::filesystem::path(argv[3]), true);
        return {true, std::nullopt};
    }
    if (argc == 4 && std::string_view(argv[1]) == "--mixed-mapped-companion-smoke-test") {
        armMappedCompanionChecks(window, application,
            std::filesystem::path(argv[2]), std::filesystem::path(argv[3]), false);
        return {true, std::nullopt};
    }
    if (argc == 4 && std::string_view(argv[1]) == "--companion-zoom-smoke-test") {
        armCompanionZoomChecks(window, application,
            std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
        return {true, std::nullopt};
    }
    if (argc == 4 && std::string_view(argv[1]) == "--companion-derived-smoke-test") {
        armCompanionDerivedChecks(window, application,
            std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
        return {true, std::nullopt};
    }
    if (argc == 4 && std::string_view(argv[1]) == "--remote-companion-smoke-test") {
        // Both plotfiles over the in-process loopback server: a remote primary
        // takes a companion from the same server, over its own connection,
        // and the pair behaves as the local one does; a local path against it
        // is refused and leaves the companion on show.
        const std::string upper = argv[2];
        const std::string lower = argv[3];
        constexpr int xz = 1;
        constexpr int xy = 2;
        auto phase = std::make_shared<int>(0);
        auto loads = std::make_shared<int>(0);
        auto errorsBefore = std::make_shared<int>(0);
        const auto fail = [&application](const char* message) {
            qCritical("%s", message);
            application.exit(1);
        };
        context.server = std::make_shared<amrvis::remote::Server>();
        context.serverThread.emplace(
            [server = context.server] { server->run(); });
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, loads, lower](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                // Only the first load opens the companion; a derived-field
                // reload of the primary lands here too.
                if (++*loads != 1) {
                    return;
                }
                const auto* openAction = window.findChild<QAction*>(
                    QStringLiteral("openCompanionAction"));
                if (openAction == nullptr || !openAction->isEnabled()) {
                    qCritical("Open Companion Plotfile is not offered for a remote plotfile");
                    application.exit(1);
                    return;
                }
                window.openRemoteCompanion(lower);
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::companionOpenFinished,
            &application,
            [&window, &application, fail, phase, errorsBefore, lower](bool success) {
                if (*phase == 0) {
                    if (!success || !window.companionOpen()
                        || window.panelTileCountForTest(xz) != 2) {
                        fail("the remote companion did not open");
                        return;
                    }
                    // The layout is geometry, not raster size: the same rects
                    // as the local pair, over viewport-bounded rasters.
                    if (!near(window.panelTileRectForTest(xz, 0), QRectF(0.0, 0.0, 6.0, 4.0))
                        || !near(window.panelTileRectForTest(xz, 1),
                            QRectF(2.0, 4.0, 4.0, 4.0))) {
                        fail("the remote tiles are not at their physical positions");
                        return;
                    }
                    if (window.layerFieldNameForTest(0, xz) != QStringLiteral("air")
                        || window.layerFieldNameForTest(1, xz) != QStringLiteral("water")) {
                        fail("the remote layers do not show their own fields");
                        return;
                    }
                    if (!window.layerSessionIsRemoteForTest(0)
                        || !window.layerSessionIsRemoteForTest(1)) {
                        fail("the companion did not open as a remote session");
                        return;
                    }
                    *phase = 1;
                    QObject::connect(&window,
                        &amrvis::qt::MainWindow::interactiveSlicesSettled, &application,
                        [&window, fail, phase, errorsBefore, lower] {
                            // In the ocean the XY panel shows the companion.
                            if (window.panelTileVisibleForTest(xy, 0)
                                || !window.panelTileVisibleForTest(xy, 1)) {
                                fail("the XY panel did not switch to the remote companion");
                                return;
                            }
                            // A fixed scale keeps both tiles on the pair's
                            // canvas: no demand-driven virtual canvas over two
                            // datasets.
                            window.selectFixedScaleForTest(2);
                            if (window.panelTileCountForTest(xz) != 2
                                || !near(window.panelTileRectForTest(xz, 1),
                                    QRectF(2.0, 4.0, 4.0, 4.0))
                                || window.activeViewVirtualCanvasActiveForTest()) {
                                fail("a fixed scale moved the remote pair off its canvas");
                                return;
                            }
                            // The same directory as a local path: a local
                            // companion beside the remote primary replaces the
                            // remote one.
                            *phase = 2;
                            *errorsBefore = window.backgroundErrorCountForTest();
                            window.openCompanion(std::filesystem::path(lower));
                        },
                        Qt::SingleShotConnection);
                    window.setSlicePositionForTest(2, -0.1);
                    return;
                }
                if (*phase == 2) {
                    if (!success || !window.companionOpen()
                        || window.panelTileCountForTest(xz) != 2
                        || window.layerSessionIsRemoteForTest(1)
                        || !window.layerSessionIsRemoteForTest(0)
                        || window.backgroundErrorCountForTest() != *errorsBefore) {
                        fail("a local companion did not open beside the remote primary");
                        return;
                    }
                    // Back to a remote companion for the rest.
                    *phase = 5;
                    window.openRemoteCompanion(lower);
                    return;
                }
                if (*phase == 5) {
                    if (!success || !window.layerSessionIsRemoteForTest(1)) {
                        fail("the remote companion did not come back");
                        return;
                    }
                    // A definition ships to the second server dataset too:
                    // the primary reloads, then the companion, which lists it.
                    *phase = 3;
                    amrvis::qt::DerivedFieldStore::session().set(
                        {amrvis::DerivedFieldDefinition{"wet", "water*2"}});
                    return;
                }
                if (*phase != 3) {
                    return;
                }
                const auto* companionSelector = window.findChild<QComboBox*>(
                    QStringLiteral("companionFieldSelector"));
                if (!success || companionSelector == nullptr
                    || !rowIsField(*companionSelector, QStringLiteral("wet"))
                    || !window.layerSessionIsRemoteForTest(1)) {
                    fail("the derived field did not reach the remote companion");
                    return;
                }
                // A selection straddling the interface over remote layers:
                // each keeps the exact window within its domain, the panel
                // frames it without scroll bars, and one round of slices
                // settles it -- no re-fetch from a viewport that shrank.
                *phase = 4;
                auto* sync = window.findChild<QAction*>(
                    QStringLiteral("syncRubberBandZoomAction"));
                if (sync != nullptr) {
                    sync->setChecked(false);
                }
                auto settles = std::make_shared<int>(0);
                QObject::connect(&window, &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [settles] { ++*settles; });
                window.rubberBandZoomPanelSceneForTest(xz, QRectF(1.0, 2.0, 4.0, 4.0));
                QTimer::singleShot(800, &application,
                    [&window, &application, fail, settles, errorsBefore] {
                        if (*settles != 1) {
                            qCritical("%d settles after a remote paired zoom", *settles);
                            fail("a remote paired zoom did not settle in one round");
                            return;
                        }
                        if (!near(window.panelTileRectForTest(xz, 0), QRectF(1.0, 2.0, 4.0, 2.0))
                            || !near(window.panelTileRectForTest(xz, 1), QRectF(2.0, 4.0, 3.0, 2.0))
                            || window.rubberBandZoomedViewCountForTest() != 2
                            || window.activeViewScrollBarsVisibleForTest()
                            || window.backgroundErrorCountForTest() != *errorsBefore) {
                            fail("a remote paired zoom did not land both layers in place");
                            return;
                        }
                        window.closeCompanion();
                        if (window.companionOpen() || window.panelTileCountForTest(xz) != 1) {
                            fail("closing the remote companion did not restore one tile");
                            return;
                        }
                        QTimer::singleShot(0, &application, [&application] { application.exit(0); });
                    });
            });
        QTimer::singleShot(30000, &application, [&application] {
            qCritical("remote companion smoke test timed out");
            application.exit(4);
        });
        QTimer::singleShot(0, &window, [&window, upper, server = context.server] {
            attachSmokeServer(window, server);
            window.openRemoteDataset(upper);
        });
        return {true, std::nullopt};
    }
    return {false, std::nullopt};
}

} // namespace amrvis::qt::smoke
