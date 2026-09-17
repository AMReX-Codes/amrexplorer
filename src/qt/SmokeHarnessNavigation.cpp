#include "SmokeHarnessInternal.hpp"
#include "MainWindow.hpp"
#include <amrexplorer/remote/Server.hpp>

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>
#include <memory>
#include <string_view>

namespace amrvis::qt::smoke {
namespace {
struct NavigationTest {
    int phase = 0;
    std::vector<QRectF> original;
    std::vector<QRectF> zoom;
    std::vector<QRectF> pan;
    std::array<double, 3> positions{};
    int plane = 0;
    std::optional<QPointF> anchor;
    std::optional<QPointF> guide;
};
bool same(const std::vector<QRectF>& a, const std::vector<QRectF>& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        // QGraphicsView centers through integer scroll bars: allow one pixel.
        const double epsilon = 0.005 * std::max(a[i].width(), a[i].height());
        if (std::abs(a[i].x() - b[i].x()) > epsilon
            || std::abs(a[i].y() - b[i].y()) > epsilon
            || std::abs(a[i].width() - b[i].width()) > epsilon
            || std::abs(a[i].height() - b[i].height()) > epsilon) {
            qCritical("panel %zu expected (%g,%g,%g,%g), got (%g,%g,%g,%g)", i,
                a[i].x(), a[i].y(), a[i].width(), a[i].height(),
                b[i].x(), b[i].y(), b[i].width(), b[i].height());
            return false;
        }
    }
    return true;
}
void key(ImageView* view, int value, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, value, modifiers);
    QApplication::sendEvent(view, &press);
    QKeyEvent release(QEvent::KeyRelease, value, modifiers);
    QApplication::sendEvent(view, &release);
}
}
Outcome dispatchNavigation(Context& context)
{
    if (context.argc < 3 || context.argc > 4) return {};
    const auto option = std::string_view(context.argv[1]);
    const bool companion = option == "--companion-navigation-smoke-test" && context.argc == 4;
    const bool remote = option == "--remote-navigation-smoke-test";
    const bool mapped = option == "--mapped-navigation-smoke-test";
    if (!remote && !mapped && !companion && option != "--navigation-smoke-test") return {};
    auto& window = context.window;
    auto& application = context.application;
    const std::filesystem::path path(context.argv[2]);
    const std::filesystem::path second(companion ? context.argv[3] : "");
    auto test = std::make_shared<NavigationTest>();
    auto* timer = new QTimer(&window);
    timer->setInterval(30);
    const auto action = [&window](bool forward) {
        window.findChild<QAction*>(forward ? QStringLiteral("navigationForwardAction")
                                          : QStringLiteral("navigationBackAction"))->trigger();
    };
    QObject::connect(&window, &MainWindow::initialSliceFinished, &window,
        [timer, &application, &window, mapped, companion, second](bool success) {
            if (mapped && success) window.setMappedGridForTest(true);
            if (companion && success) {
                window.openCompanion(second);
                return;
            }
            if (success) timer->start(); else application.exit(1);
        });
    if (companion) {
        QObject::connect(&window, &MainWindow::companionOpenFinished, &window,
            [timer, &application, &window](bool success) {
                if (!success) { application.exit(1); return; }
                window.setActiveViewForTest(1);
                timer->start();
            });
    }
    QObject::connect(timer, &QTimer::timeout, &window,
        [&window, &application, timer, test, action] {
            if (test->phase == 18) {
                if (!window.navigationWorkerWaitingForTest()) return;
            } else if (!window.navigationIdleForTest()) return;
            const auto require = [&](bool condition, const char* message) {
                if (!condition) {
                    qCritical("navigation phase %d: %s", test->phase, message);
                    timer->stop();
                    application.exit(1);
                }
                return condition;
            };
            switch (test->phase++) {
            case 0:
                test->original = window.navigationWindowsForTest();
                window.navigationZoomForTest();
                break;
            case 1:
                test->zoom = window.navigationWindowsForTest();
                if (!require(window.navigationCountForTest() == 1, "zoom was not one action")) return;
                action(false);
                break;
            case 2:
                if (!require(same(test->original, window.navigationWindowsForTest()), "Back did not restore initial view")) return;
                action(true);
                break;
            case 3:
                if (!require(same(test->zoom, window.navigationWindowsForTest()), "Forward did not restore zoom")) return;
                window.findChild<QAction*>(QStringLiteral("navigationBackAction"))->trigger();
                // Forward and Back before their requests start must still end at the original.
                action(true); action(false);
                break;
            case 4:
                if (!require(same(test->original, window.navigationWindowsForTest()), "rapid Back/Forward restored stale view")) return;
                action(true);
                break;
            case 5:
                if (!require(same(test->zoom, window.navigationWindowsForTest()), "rapid redo lost zoom")) return;
                key(window.navigationViewForTest(), Qt::Key_Right);
                break;
            case 6:
                test->pan = window.navigationWindowsForTest();
                action(false);
                break;
            case 7:
                if (!require(same(test->zoom, window.navigationWindowsForTest()), "pan Back lost framing")) return;
                action(true);
                break;
            case 8:
                if (!require(same(test->pan, window.navigationWindowsForTest()), "pan Forward lost framing")) return;
                window.selectFixedScaleForTest(4);
                break;
            case 9:
                action(false);
                break;
            case 10:
                if (!require(same(test->pan, window.navigationWindowsForTest()), "fixed scale Back lost framing")) return;
                // Begin a new branch and group a trackpad-like wheel burst.
                for (int i = 0; i < 3; ++i) {
                    auto* view = window.navigationViewForTest();
                    const QPointF p = view->viewport()->rect().center();
                    QWheelEvent wheel(p, view->viewport()->mapToGlobal(p.toPoint()), {}, {0, 120},
                        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                    QApplication::sendEvent(view->viewport(), &wheel);
                }
                break;
            case 11:
                action(false);
                break;
            case 12:
                if (!require(same(test->pan, window.navigationWindowsForTest()), "wheel burst required multiple Back actions")) return;
                // Reset and zoom back to the center so both crosshairs are visible.
                window.resetZoomAllViewsForTest();
                break;
            case 13:
                window.navigationZoomForTest();
                break;
            case 14:
                if (test->original.size() == 1 || window.activeViewIsMappedForTest()) {
                    test->phase = 17;
                    break;
                }
                window.setActiveViewForTest(test->plane);
                test->anchor = window.navigationAnchorForTest();
                test->zoom = window.navigationWindowsForTest();
                for (int axis = 0; axis < 3; ++axis)
                    test->positions[static_cast<std::size_t>(axis)] = window.slicePositionForTest(axis);
                key(window.navigationViewForTest(), test->plane == 0 ? Qt::Key_Down : Qt::Key_Right, Qt::ShiftModifier);
                break;
            case 15: {
                bool moved = false;
                for (int axis = 0; axis < 3; ++axis) {
                    const double position = window.slicePositionForTest(axis);
                    if (axis == test->plane && !require(position == test->positions[static_cast<std::size_t>(axis)], "scan changed the active normal")) return;
                    moved = moved || position != test->positions[static_cast<std::size_t>(axis)];
                }
                if (!moved) {
                    qCritical("scan plane %d enabled=%d positions=%g,%g,%g", test->plane,
                        window.findChild<QAction*>(QStringLiteral("fixedCrosshairAction"))->isEnabled(),
                        test->positions[0], test->positions[1], test->positions[2]);
                }
                if (!require(moved, "scan did not move the sibling slices")) return;
                const auto anchor = window.navigationAnchorForTest();
                if (!require(anchor && test->anchor && QLineF(*anchor, *test->anchor).length() < 0.005,
                        "scan moved the crosshair on screen")) return;
                action(false);
                break;
            }
            case 16:
                if (!require(same(test->zoom, window.navigationWindowsForTest()), "scan Back lost view")) return;
                for (int axis = 0; axis < 3; ++axis)
                    if (!require(window.slicePositionForTest(axis) == test->positions[static_cast<std::size_t>(axis)], "scan Back lost slice coordinates")) return;
                if (++test->plane < 3) test->phase = 14;
                else test->phase = 17;
                break;
            case 17:
                test->zoom = window.navigationWindowsForTest();
                // A cached refresh is parked inside the real worker while
                // navigation supersedes it. This must test arrival rejection,
                // not just the final state with all work already settled.
                window.armSliceGateForTest();
                window.navigationRefreshForTest();
                break;
            case 18:
                action(false); action(true);
                window.releaseSliceGateForTest();
                break;
            case 19:
                if (!require(same(test->zoom, window.navigationWindowsForTest()), "late render overwrote navigation")) return;
                if (test->original.size() == 1 || window.activeViewIsMappedForTest()) {
                    test->phase = 22;
                    break;
                }
                test->anchor = window.navigationAnchorForTest();
                for (int axis = 0; axis < 3; ++axis)
                    test->positions[static_cast<std::size_t>(axis)] = window.slicePositionForTest(axis);
                window.findChild<QAction*>(QStringLiteral("fixedCrosshairAction"))->setChecked(true);
                test->guide = window.navigationViewForTest()->crosshairViewportIntersection();
                window.shiftDragActiveViewForTest(window.navigationViewForTest()->viewport()->width() / 3, 0);
                if (test->original.size() == 3) {
                    const auto guide = window.navigationViewForTest()->crosshairViewportIntersection();
                    if (!require(guide && test->guide && QLineF(*guide, *test->guide).length() <= 1.0,
                            "guides moved while the panned raster was still pending")) return;
                }
                break;
            case 20: {
                const auto anchor = window.navigationAnchorForTest();
                if (!require(anchor && test->anchor && QLineF(*anchor, *test->anchor).length() < 0.005,
                        "drag moved the crosshair on screen")) return;
                bool moved = false;
                for (int axis = 0; axis < 3; ++axis)
                    moved = moved || window.slicePositionForTest(axis) != test->positions[static_cast<std::size_t>(axis)];
                if (!require(moved, "drag did not scan the sibling slices")) return;
                action(false);
                break;
            }
            case 21:
                if (!require(same(test->zoom, window.navigationWindowsForTest()), "drag Back lost framing")) return;
                for (int axis = 0; axis < 3; ++axis)
                    if (!require(window.slicePositionForTest(axis) == test->positions[static_cast<std::size_t>(axis)], "drag Back lost slice coordinates")) return;
                window.findChild<QAction*>(QStringLiteral("fixedCrosshairAction"))->setChecked(false);
                break;
            case 22:
                if (test->original.size() > 1) window.setActiveViewForTest(0);
                for (int notch = 0; notch < 8; ++notch) window.wheelActiveViewForTest(1);
                break;
            case 23: {
                test->zoom = window.navigationWindowsForTest();
                auto* bar = window.navigationViewForTest()->horizontalScrollBar();
                if (!require(bar->maximum() > bar->minimum(), "scroll test has no scroll range")) return;
                bar->triggerAction(bar->value() < bar->maximum()
                    ? QAbstractSlider::SliderSingleStepAdd : QAbstractSlider::SliderSingleStepSub);
                break;
            }
            case 24:
                action(false);
                break;
            case 25:
                if (!require(same(test->zoom, window.navigationWindowsForTest()), "scrollbar Back lost framing")) return;
                window.resize(window.width() + 100, window.height() + 60);
                action(true); action(false);
                break;
            case 26: {
                const auto actual = window.navigationWindowsForTest().front();
                const auto expected = test->zoom.front();
                const double epsilon = 0.01 * std::max(expected.width(), expected.height());
                if (!require(actual.adjusted(-epsilon, -epsilon, epsilon, epsilon).contains(expected),
                        "restore after resize lost part of the saved window")) return;
                break;
            }
            case 27:
                if (test->original.size() > 1) {
                    window.setSlicePositionForTest(0, window.slicePositionForTest(0) + 0.01);
                    if (!require(window.navigationCountForTest() == 0, "manual slice change kept history")) return;
                }
                timer->stop(); application.exit(0);
                break;
            default: application.exit(1);
            }
        });
    QTimer::singleShot(25000, &application, [&application, &window] {
        window.releaseSliceGateForTest();
        application.exit(4);
    });
    if (remote) {
        context.server = std::make_shared<amrvis::remote::Server>();
        context.serverThread.emplace([server = context.server] { server->run(); });
    }
    QTimer::singleShot(0, &window, [&window, path, server = context.server, remote] {
        if (remote) {
            attachSmokeServer(window, server);
            window.openRemoteDataset(path.string());
        } else {
            window.openDataset(path);
        }
    });
    return {true, std::nullopt};
}
} // namespace amrvis::qt::smoke
