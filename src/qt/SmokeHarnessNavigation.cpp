#include "SmokeHarnessInternal.hpp"
#include "MainWindow.hpp"
#include <amrexplorer/remote/Server.hpp>

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
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
// Covers the expected window, give or take a scroll bar appearing or going.
bool covers(const std::vector<QRectF>& expected, const std::vector<QRectF>& actual)
{
    if (expected.size() != actual.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& e = expected[i];
        const auto& a = actual[i];
        const double epsilon = 0.01 * std::max(e.width(), e.height());
        if (!a.adjusted(-epsilon, -epsilon, epsilon, epsilon).contains(e)
            || a.width() > 1.05 * e.width() || a.height() > 1.05 * e.height()) {
            qCritical("panel %zu expected about (%g,%g,%g,%g), got (%g,%g,%g,%g)", i,
                e.x(), e.y(), e.width(), e.height(), a.x(), a.y(), a.width(), a.height());
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
void mouse(QWidget* widget, QEvent::Type type, QPoint position, Qt::MouseButton button,
    Qt::MouseButtons buttons)
{
    QMouseEvent event(type, QPointF(position), widget->mapToGlobal(QPointF(position)),
        button, buttons, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
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
            if (test->phase == 15) {
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
            case 0: {
                // Window-wide shortcuts, not ones that need a focused panel.
                auto* back = window.findChild<QAction*>(QStringLiteral("navigationBackAction"));
                if (!require(back->shortcut() == QKeySequence(QKeySequence::Back)
                        && back->shortcutContext() == Qt::WindowShortcut, "Back is not a window shortcut")) return;
                test->original = window.navigationWindowsForTest();
                window.navigationZoomForTest();
                break;
            }
            case 1:
                test->zoom = window.navigationWindowsForTest();
                if (!require(window.navigationCountForTest() == 1, "zoom was not one action")) return;
                action(false);
                break;
            case 2:
                if (!require(same(test->original, window.navigationWindowsForTest()), "Back did not restore initial view")) return;
                // The mouse's forward button, as a quick double click reports it.
                mouse(window.navigationViewForTest()->viewport(), QEvent::MouseButtonDblClick,
                    {10, 10}, Qt::ForwardButton, Qt::ForwardButton);
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
                // Reset and let its raster arrive before zooming again.
                window.resetZoomAllViewsForTest();
                break;
            case 13:
                window.navigationZoomForTest();
                break;
            case 14:
                test->zoom = window.navigationWindowsForTest();
                // A cached refresh is parked inside the real worker while
                // navigation supersedes it. This must test arrival rejection,
                // not just the final state with all work already settled.
                window.armSliceGateForTest();
                window.navigationRefreshForTest();
                break;
            case 15:
                action(false); action(true);
                window.releaseSliceGateForTest();
                break;
            case 16:
                if (!require(same(test->zoom, window.navigationWindowsForTest()), "late render overwrote navigation")) return;
                if (test->original.size() == 1 || window.activeViewIsMappedForTest()) {
                    test->phase = 19;
                    break;
                }
                window.shiftDragActiveViewForTest(window.navigationViewForTest()->viewport()->width() / 3, 0);
                break;
            case 17:
                action(false);
                break;
            case 18:
                if (!require(same(test->zoom, window.navigationWindowsForTest()), "drag Back lost framing")) return;
                break;
            case 19:
                if (test->original.size() > 1) window.setActiveViewForTest(0);
                for (int notch = 0; notch < 8; ++notch) window.wheelActiveViewForTest(1);
                break;
            case 20: {
                test->zoom = window.navigationWindowsForTest();
                auto* bar = window.navigationViewForTest()->horizontalScrollBar();
                if (!require(bar->maximum() > bar->minimum(), "scroll test has no scroll range")) return;
                bar->triggerAction(bar->value() < bar->maximum()
                    ? QAbstractSlider::SliderSingleStepAdd : QAbstractSlider::SliderSingleStepSub);
                break;
            }
            case 21:
                action(false);
                break;
            case 22:
                if (!require(same(test->zoom, window.navigationWindowsForTest()), "scrollbar Back lost framing")) return;
                window.resize(window.width() + 100, window.height() + 60);
                action(true); action(false);
                break;
            case 23: {
                const auto actual = window.navigationWindowsForTest().front();
                const auto expected = test->zoom.front();
                const double epsilon = 0.01 * std::max(expected.width(), expected.height());
                if (!require(actual.adjusted(-epsilon, -epsilon, epsilon, epsilon).contains(expected),
                        "restore after resize lost part of the saved window")) return;
                break;
            }
            case 24:
                // Wheel before Back's raster arrives: the arrival keeps the wheel zoom.
                action(false);
                window.wheelActiveViewForTest(1);
                test->zoom = window.navigationWindowsForTest();
                break;
            case 25:
                if (!require(covers(test->zoom, window.navigationWindowsForTest()), "Back's arrival undid a newer wheel zoom")) return;
                // Zoomed out enough that the drag below spans raster pixels.
                window.resetZoomAllViewsForTest();
                break;
            case 26:
                if (test->original.size() > 1) {
                    // A slice change clears history but not a drag in progress.
                    auto* port = window.navigationViewForTest()->viewport();
                    const QPoint from(port->width() / 4, port->height() / 4);
                    const QPoint to(3 * port->width() / 4, 3 * port->height() / 4);
                    mouse(port, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
                    mouse(port, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
                    window.setSlicePositionForTest(0, window.slicePositionForTest(0) + 0.01);
                    if (!require(window.navigationCountForTest() == 0, "manual slice change kept history")) return;
                    mouse(port, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
                    if (!require(window.navigationCountForTest() == 1, "slice change canceled a drag")) return;
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
