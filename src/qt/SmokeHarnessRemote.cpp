#include "SmokeHarnessInternal.hpp"

#include "FabSelectorDock.hpp"
#include "MainWindow.hpp"
#include "RemoteEndpoint.hpp"

#include <QPainter>
#include <QTimer>

#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Remote: the remote loopback-server scenarios: fixed-scale local-versus-
// remote agreement, canvas wheel, flicker, remote slicing/geometry/rubber-
// band/grid boxes, remote sequences. Every branch drives MainWindow through
// its ForTest accessors and arms connections and timers for main() to run;
// see SmokeHarness.hpp.

namespace amrvis::qt::smoke {

namespace {

// What one view shows, for a local-versus-remote comparison: the physical
// window the probe reports, the viewport pixels, and the raster size.
struct ViewCapture {
    QRectF window;
    QImage viewport;
    std::array<int, 2> image{};
};

void printDataWindow(std::ostream& stream, const QRectF& window)
{
    stream << std::setprecision(17) << window.x() << ',' << window.y() << ','
           << window.width() << ',' << window.height();
}

// Local and remote must agree on the physical window, but not to the last bit:
// a local fixed scale scrolls a whole-domain raster while a remote one scrolls
// a whole-domain virtual canvas of finest cells, and both scroll in whole
// viewport pixels, so their integer scroll positions may differ by a rounding
// step. Two viewport pixels of slack covers that; the coordinate-space
// confusion this guards against is off by many cells.
bool dataWindowsAgree(const QRectF& local, const QRectF& remote,
    const std::array<int, 2>& viewportPixels)
{
    const auto slack = [](double extent, int pixels) {
        return pixels > 0 ? 2.0 * std::fabs(extent) / pixels : 0.0;
    };
    const auto slackX = std::max(slack(local.width(), viewportPixels[0]),
        1.0e-9 * std::fabs(local.width()));
    const auto slackY = std::max(slack(local.height(), viewportPixels[1]),
        1.0e-9 * std::fabs(local.height()));
    return std::fabs(local.left() - remote.left()) <= slackX
        && std::fabs(local.right() - remote.right()) <= slackX
        && std::fabs(local.top() - remote.top()) <= slackY
        && std::fabs(local.bottom() - remote.bottom()) <= slackY;
}

// Rendering happens client-side for both datasets, so equal data must paint
// equal viewport pixels. Reports the first difference (raster order) and how
// many pixels differ, or nullopt when the two viewports are identical.
struct ViewportDifference {
    QString summary;
    std::size_t differingPixels = 0;
};

std::optional<ViewportDifference> viewportDifference(
    const QImage& local, const QImage& remote)
{
    if (local.size() != remote.size() || local.isNull() || remote.isNull()) {
        return ViewportDifference{
            QStringLiteral("viewport sizes differ (local %1x%2, remote %3x%4)")
                .arg(local.width())
                .arg(local.height())
                .arg(remote.width())
                .arg(remote.height()),
            0};
    }
    const auto left = local.convertToFormat(QImage::Format_ARGB32);
    const auto right = remote.convertToFormat(QImage::Format_ARGB32);
    std::optional<ViewportDifference> difference;
    std::size_t differing = 0;
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            const auto localPixel = left.pixel(x, y);
            const auto remotePixel = right.pixel(x, y);
            if (localPixel == remotePixel) {
                continue;
            }
            ++differing;
            if (!difference) {
                difference = ViewportDifference{
                    QStringLiteral("first differing pixel at (%1,%2): "
                                   "local=%3 remote=%4")
                        .arg(x)
                        .arg(y)
                        .arg(localPixel, 8, 16, QLatin1Char('0'))
                        .arg(remotePixel, 8, 16, QLatin1Char('0')),
                    0};
            }
        }
    }
    if (difference) {
        difference->differingPixels = differing;
    }
    return difference;
}

} // namespace

Outcome dispatchRemote(Context& context)
{
    // The branches read these names as main() declared them; binding them
    // here keeps the moved code verbatim.
    auto& application = context.application;
    auto& window = context.window;
    const int argc = context.argc;
    char** argv = context.argv;
    auto& smokeServer = context.server;
    auto& smokeServerThread = context.serverThread;

    if ((argc == 8 || argc == 10)
        && std::string_view(argv[1])
            == "--fixed-scale-local-remote-repro") {
        if (std::string_view(argv[2]) != "--connect"
            || std::string_view(argv[4]) != "--token-stdin"
            || std::string_view(argv[5]) != "--remote-path"
            || (argc == 10 && std::string_view(argv[7]) != "--screenshot")) {
            qCritical("usage: amrexplorer --fixed-scale-local-remote-repro "
                "--connect HOST:PORT --token-stdin --remote-path "
                "REMOTE_PLOTFILE [--screenshot OUTPUT.png] LOCAL_PLOTFILE");
            return {true, 2};
        }
        const auto endpoint = amrvis::qt::parseRemoteEndpoint(argv[3]);
        std::string token;
        if (!endpoint || !std::getline(std::cin, token) || token.empty()) {
            qCritical("invalid endpoint or empty token");
            return {true, 2};
        }
        if (!token.empty() && token.back() == '\r') {
            token.pop_back();
        }
        struct FixedScaleProbe {
            int phase = 0;
            bool localShowsWholeImage = false;
            std::array<int, 2> localImage{};
            std::array<int, 2> localViewport{};
            QRectF localWindow;
            QImage localViewportImage;
        };
        auto probe = std::make_shared<FixedScaleProbe>();
        const auto remotePath = std::string(argv[6]);
        const auto screenshotPath = argc == 10
            ? QString::fromLocal8Bit(argv[8]) : QString();
        const auto localPath = std::filesystem::path(argv[argc - 1]);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application,
            [&window, &application, endpoint = *endpoint,
                token = std::move(token), remotePath, screenshotPath,
                probe](bool success) {
                if (!success) {
                    application.exit(probe->phase == 0 ? 2 : 3);
                    return;
                }
                window.selectFixedScaleForTest(1);
                if (probe->phase == 0) {
                    probe->phase = 1;
                    // Measure only after the as-needed scroll bars this scale
                    // may have demanded are laid out: they shrink the viewport,
                    // and the remote phase necessarily measures on the far side
                    // of that layout pass. On a domain that fits there are no
                    // bars and this only costs a tick.
                    QTimer::singleShot(100, &window,
                        [&window, probe, endpoint, token, remotePath] {
                            probe->localShowsWholeImage
                                = window.activeViewShowsWholeImageForTest();
                            probe->localImage
                                = window.activeViewImageSizeForTest();
                            probe->localViewport
                                = window.activeViewViewportSizeForTest();
                            probe->localWindow
                                = window.activeViewVisibleDataWindowForTest();
                            probe->localViewportImage
                                = window.activeViewViewportImageForTest();
                            try {
                                window.useRemoteConnection(
                                    std::make_shared<
                                        amrvis::remote::Connection>(
                                        endpoint.host, endpoint.port,
                                        amrvis::remote::ConnectionOptions{
                                            .clientName
                                            = "AMReXplorer Qt repro",
                                            .sessionToken = token}),
                                    QStringLiteral("%1:%2")
                                        .arg(QString::fromStdString(
                                            endpoint.host))
                                        .arg(endpoint.port));
                            } catch (const std::exception& error) {
                                std::cerr << "cannot connect to "
                                          << endpoint.host << ':'
                                          << endpoint.port << ": "
                                          << error.what() << '\n';
                                QCoreApplication::exit(2);
                                return;
                            }
                            window.openRemoteDataset(remotePath);
                        });
                    return;
                }
                const auto measure = [&window, &application, probe,
                                         screenshotPath] {
                    const auto remoteShowsWholeImage
                        = window.activeViewShowsWholeImageForTest();
                    const auto remoteImage
                        = window.activeViewImageSizeForTest();
                    const auto remoteViewport
                        = window.activeViewViewportSizeForTest();
                    const auto remoteWindow
                        = window.activeViewVisibleDataWindowForTest();
                    const auto remoteViewportImage
                        = window.activeViewViewportImageForTest();
                    std::cout << "local 1x: image=" << probe->localImage[0]
                              << 'x' << probe->localImage[1]
                              << " viewport=" << probe->localViewport[0]
                              << 'x' << probe->localViewport[1]
                              << " whole-image-visible="
                              << (probe->localShowsWholeImage ? "yes" : "no")
                              << " data-window=";
                    printDataWindow(std::cout, probe->localWindow);
                    std::cout << "\nremote 1x: image=" << remoteImage[0]
                              << 'x' << remoteImage[1]
                              << " viewport=" << remoteViewport[0]
                              << 'x' << remoteViewport[1]
                              << " whole-image-visible="
                              << (remoteShowsWholeImage ? "yes" : "no")
                              << " data-window=";
                    printDataWindow(std::cout, remoteWindow);
                    std::cout << '\n';
                    // The verdict is the visible physical window plus the
                    // painted pixels -- a probe reporting the wrong window, or
                    // a raster that differs where it is visible, is exactly
                    // what this tool exists to catch. Backing-raster
                    // dimensions are printed but deliberately not compared:
                    // local keeps the whole domain while remote fetches only
                    // the visible window, so they differ by design on any
                    // domain larger than the viewport. Equal windows over
                    // equal pixels already pin the scale -- a factor the two
                    // sides disagreed on would move the window.
                    const auto viewportMatches
                        = probe->localViewport == remoteViewport;
                    const auto windowMatches = viewportMatches
                        && dataWindowsAgree(probe->localWindow, remoteWindow,
                            probe->localViewport);
                    const auto contentDifference = viewportDifference(
                        probe->localViewportImage, remoteViewportImage);
                    if (!screenshotPath.isEmpty()) {
                        constexpr int headingHeight = 36;
                        constexpr int gap = 12;
                        const auto width = probe->localViewportImage.width()
                            + remoteViewportImage.width() + gap;
                        const auto height = headingHeight + std::max(
                            probe->localViewportImage.height(),
                            remoteViewportImage.height());
                        QImage comparison(width, height,
                            QImage::Format_ARGB32_Premultiplied);
                        comparison.fill(QColor(30, 30, 30));
                        QPainter painter(&comparison);
                        painter.setPen(Qt::white);
                        painter.drawText(QRect(0, 0,
                            probe->localViewportImage.width(), headingHeight),
                            Qt::AlignCenter, QStringLiteral("Local - 1x"));
                        painter.drawText(QRect(
                            probe->localViewportImage.width() + gap, 0,
                            remoteViewportImage.width(), headingHeight),
                            Qt::AlignCenter, QStringLiteral("Remote - 1x"));
                        painter.drawImage(0, headingHeight,
                            probe->localViewportImage);
                        painter.drawImage(
                            probe->localViewportImage.width() + gap,
                            headingHeight, remoteViewportImage);
                        painter.end();
                        if (!comparison.save(screenshotPath, "PNG")) {
                            std::cerr
                                << "failed to save screenshot comparison to "
                                << screenshotPath.toStdString() << '\n';
                            application.exit(5);
                            return;
                        }
                        std::cout << "screenshot="
                                  << screenshotPath.toStdString() << '\n';
                    }
                    if (contentDifference) {
                        std::cout << "raster-content: "
                                  << contentDifference->summary.toStdString()
                                  << " (" << contentDifference->differingPixels
                                  << " differing pixels)\n";
                    } else {
                        std::cout << "raster-content: identical\n";
                    }
                    const auto matches = windowMatches && !contentDifference;
                    std::cout << (matches ? "MATCH" : "MISMATCH")
                              << ": remote 1x ";
                    if (matches) {
                        std::cout << "shows the same visible data window and "
                                     "paints the same viewport pixels as "
                                     "local 1x";
                    } else if (!viewportMatches) {
                        std::cout << "was measured against a different viewport "
                                     "than local 1x, so the two are not "
                                     "comparable";
                    } else if (!windowMatches) {
                        std::cout << "does not show the same physical data "
                                     "window as local 1x";
                    } else {
                        std::cout << "does not paint the same viewport pixels "
                                     "as local 1x";
                    }
                    std::cout << '\n';
                    application.exit(matches ? 0 : 1);
                };
                // The remote fixed scale is demand-driven: selecting it can
                // queue a native-resolution refetch of the visible window, and
                // measuring a raster that is still being replaced reports a
                // difference that is only a race. Poll instead of guessing a
                // grace period: measure once nothing is on a worker and no
                // settle arrived during the last tick, so a request that only
                // queues its successor is waited out too. A switch that needed
                // no new raster satisfies this on the first tick.
                auto* const poll = new QTimer(&window);
                poll->setInterval(100);
                auto ticks = std::make_shared<int>(0);
                auto settles = std::make_shared<int>(0);
                auto settlesLastTick = std::make_shared<int>(-1);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [settles] { ++*settles; });
                QObject::connect(poll, &QTimer::timeout, &application,
                    [&application, &window, measure, poll, ticks, settles,
                        settlesLastTick] {
                        const auto quiet = window.slicesInFlightForTest() == 0
                            && *settles == *settlesLastTick;
                        *settlesLastTick = *settles;
                        if (quiet) {
                            poll->stop();
                            measure();
                            return;
                        }
                        if (++*ticks >= 200) {
                            poll->stop();
                            std::cerr << "the remote view never stopped "
                                         "fetching; refusing to compare an "
                                         "in-flight raster\n";
                            application.exit(6);
                        }
                    });
                poll->start();
            });
        QTimer::singleShot(30000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window,
            [&window, localPath] { window.openDataset(localPath); });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--remote-fixed-scale-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        auto fixedScalePhase = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, fixedScalePhase](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, fixedScalePhase] {
                        if (!window.fixedScaleStateMatchesForTest(1)
                            || !window.activeViewUsesNativeOutputForTest()) {
                            application.exit(1);
                            return;
                        }
                        const auto phase = (*fixedScalePhase)++;
                        if (phase == 0
                            && window.activeViewIsZoomedForTest()) {
                            window.panActiveViewForTest(-8.0, 0.0);
                            return;
                        }
                        if (phase == 1
                            && window.activeViewIsZoomedForTest()) {
                            window.resize(
                                window.width() + 80, window.height() + 40);
                            return;
                        }
                        application.exit(0);
                    });
                window.selectFixedScaleForTest(1);
                if (window.activeViewUsesNativeOutputForTest()) {
                    QTimer::singleShot(0, &application,
                        [&window, &application] {
                            application.exit(
                                window.fixedScaleStateMatchesForTest(1)
                                ? 0 : 1);
                        });
                }
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--remote-cell-aspect-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        // Regression for remote-fit-anisotropic-cells. The fixture's cells
        // are 64 times taller than wide (a 64x1024-cell domain that is 1x1024
        // in physical units); the display draws one square pixel per cell,
        // as the local path does, so a remote raster must be sized to the
        // region's aspect in cells. Sized to the physical aspect instead, Fit
        // fetched a one-pixel-wide strip, and a wheel zoom from 1x -- Custom
        // mode over the fetched window -- a two-column raster stretched
        // across the view. Both are checked: Fit first, then 1x wheeled in
        // until the window is a sub-window of the domain.
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, phase](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                if (!window.activeViewRasterHasCellAspectForTest()) {
                    qCritical("the Fit raster is not at the cell aspect");
                    application.exit(1);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, phase] {
                        if (*phase == 0) {
                            *phase = 1;
                            if (!window.activeViewRasterHasCellAspectForTest()) {
                                qCritical("the 1x raster is not at the cell "
                                          "aspect");
                                application.exit(1);
                                return;
                            }
                            // Enough notches that the domain outgrows the
                            // viewport on the long axis, so the demand fetch
                            // is a sub-window and Custom mode sizes it.
                            for (int notch = 0; notch < 4; ++notch) {
                                window.wheelActiveViewForTest(1);
                            }
                            return;
                        }
                        if (*phase != 1) {
                            return;
                        }
                        *phase = 2;
                        if (!window.activeViewIsZoomedForTest()) {
                            qCritical("the wheel zoom fetched no sub-window");
                            application.exit(1);
                            return;
                        }
                        application.exit(
                            window.activeViewRasterHasCellAspectForTest()
                                ? 0 : 1);
                    });
                window.selectFixedScaleForTest(1);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--remote-canvas-wheel-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        // Regression for virtual-canvas-survives-wheel-zoom. A wheel notch over
        // a remote fixed scale leaves the whole-domain virtual canvas installed
        // while switching the transform mode to Custom, so the next slice
        // arrival with a changed density or owner reaches preservedDataWindow --
        // which reads scene units as raster pixels of the cached plane, and on
        // a canvas they are finest cells over the whole domain. The window it
        // computed was then fed to zoomToRect. The view must stay where the
        // wheel put it: still on the canvas, still showing a window inside the
        // domain, and still centred where it was zoomed about.
        auto phase = std::make_shared<int>(0);
        auto before = std::make_shared<QRectF>();
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, phase, before](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application,
                    [&window, &application, phase, before] {
                        if (*phase == 0) {
                            *phase = 1;
                            if (!window
                                    .activeViewVirtualCanvasActiveForTest()) {
                                qCritical("no virtual canvas at fixed scale");
                                application.exit(1);
                                return;
                            }
                            *before = window
                                .activeViewVisibleDataWindowForTest();
                            window.wheelActiveViewForTest(1);
                            return;
                        }
                        if (*phase != 1) {
                            return;
                        }
                        *phase = 2;
                        const auto after
                            = window.activeViewVisibleDataWindowForTest();
                        // The canvas survives the zoom by design: it is what
                        // lets the demand fetch keep working, and dropping it
                        // would strand the scroll bars mid-domain.
                        if (!window.activeViewVirtualCanvasActiveForTest()) {
                            qCritical("the wheel zoom dropped the canvas");
                            application.exit(1);
                            return;
                        }
                        // A window with no extent is what the raster-pixel
                        // reading of cell-space scene coordinates produced.
                        if (after.width() <= 0.0 || after.height() <= 0.0) {
                            qCritical("the wheel zoom left a %gx%g window",
                                after.width(), after.height());
                            application.exit(1);
                            return;
                        }
                        if (after.width() >= before->width()) {
                            qCritical("zooming in did not narrow the window");
                            application.exit(1);
                            return;
                        }
                        // Zoomed about the viewport centre, so the centre is
                        // what must not move.
                        const auto drift = std::abs(
                            after.center().x() - before->center().x());
                        if (drift > 0.05 * after.width()) {
                            qCritical("the wheel zoom moved the centre by %g",
                                drift);
                            application.exit(1);
                            return;
                        }
                        application.exit(0);
                    });
                window.selectFixedScaleForTest(32);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3 && std::string_view(argv[1])
            == "--remote-fixed-scale-flicker-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        // Regression: in a window far too small for the whole domain at 32x,
        // the demand-driven fixed scale must settle with the viewport fully
        // backed by fetched raster, stay quiet with no input (the demand used
        // to re-issue itself endlessly through the as-needed scrollbars,
        // flickering through one remote render per flip), refetch when the
        // virtual scroll bars pan to unfetched cells, and drop the
        // domain-spanning scroll bars on a rubber-band zoom, whose selection is
        // re-rendered fitted to the pane exactly as for local data. The step
        // gating below waits for quiescence, so it does not count the refetches.
        auto phase = std::make_shared<int>(0);
        // Judge each step once the demand loop has quiesced -- nothing on a
        // worker and no settle since the last tick -- rather than after a fixed
        // delay. A slow runner's extra convergence settle just costs a tick;
        // a step that needs no refetch is quiet on the next tick instead of
        // hanging; and the flicker regression (the demand endlessly re-issuing
        // itself) never quiesces, so it trips the bounded-tick guard with its
        // own exit code rather than being conflated with the watchdog. Same
        // model as the fixed-scale-parity poll above.
        auto* poll = new QTimer(&window);
        poll->setInterval(100);
        auto settles = std::make_shared<int>(0);
        auto settlesLastTick = std::make_shared<int>(-1);
        // Settle count when the current step's action was issued. The flicker
        // guard bounds settles-per-step (fetch *rounds*), not elapsed ticks: a
        // legitimate convergence is a few rounds however slow the runner, while
        // the flicker re-issues without bound. Counting ticks instead would
        // misread a starved-but-finite fetch chain as a loop.
        auto settlesAtStep = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [settles] { ++*settles; });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, poll](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                poll->start();
                window.selectFixedScaleForTest(32);
            });
        QObject::connect(poll, &QTimer::timeout, &application,
            [&window, &application, poll, phase, settles, settlesLastTick,
                settlesAtStep] {
                // Quiescence also requires no request queued behind the slice
                // debounce: a pan/zoom schedules its refetch there, so between
                // the input and the debounce firing nothing is on a worker yet
                // and a bare in-flight check would read that gap as converged.
                const auto quiet = !window.sliceRequestPendingForTest()
                    && window.slicesInFlightForTest() == 0
                    && *settles == *settlesLastTick;
                *settlesLastTick = *settles;
                if (!quiet) {
                    // Too many fetch rounds for one step is the flicker
                    // regression (the demand loop re-issuing itself endlessly);
                    // a legitimate step converges in a handful. A pure hang with
                    // no settles is left to the watchdog, not misreported here.
                    if (*settles - *settlesAtStep > 20) {
                        poll->stop();
                        std::cerr << "the fixed-scale demand loop never "
                                     "quiesced (flicker regression)\n";
                        application.exit(5);
                    }
                    return;
                }
                if (*phase == 0) {
                    // Fixed scale converged: the raster backs the whole viewport
                    // and the demand loop stays quiet, rather than re-issuing
                    // itself through the as-needed scroll bars.
                    *phase = 1;
                    if (!window.fixedScaleStateMatchesForTest(32)
                        || !window
                            .allViewsFixedScaleRasterCoversViewportForTest()) {
                        poll->stop();
                        application.exit(1);
                        return;
                    }
                    // Five cells' worth of pixels at 32x through the real
                    // Shift+left mouse event path; the newly visible cells are
                    // fetched, then the loop is quiet again.
                    *settlesAtStep = *settles;
                    window.shiftDragActiveViewForTest(-160, 0);
                    return;
                }
                if (*phase == 1) {
                    // The scrolled fixed scale keeps the fetched raster under the
                    // whole viewport, with the domain-spanning scroll bars.
                    *phase = 2;
                    if (!window.fixedScaleStateMatchesForTest(32)
                        || !window
                            .allViewsFixedScaleRasterCoversViewportForTest()
                        || !window.activeViewScrollBarsVisibleForTest()) {
                        poll->stop();
                        application.exit(1);
                        return;
                    }
                    *settlesAtStep = *settles;
                    window.rubberBandZoomActiveViewForTest();
                    return;
                }
                if (*phase == 2) {
                    // The re-rendered selection stands alone, fitted to the pane
                    // without scroll bars, as for local data.
                    poll->stop();
                    application.exit(
                        window.activeViewIsZoomedForTest()
                            && !window.activeViewScrollBarsVisibleForTest()
                            ? 0 : 1);
                }
            });
        // Backstop for a hang outside the poll (e.g. the initial load never
        // finishing); the poll's own settle-count guard catches a flicker first.
        // Stop the poll so a tick in the same pass can't overwrite exit(4).
        QTimer::singleShot(20000, &application,
            [&application, poll] {
                poll->stop();
                application.exit(4);
            });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                window.resize(420, 301);
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--local-remote-fixed-scale-window-smoke-test") {
        // The comparison the manual --fixed-scale-local-remote-repro cannot
        // make, because its fixture fits the viewport whole: at 32x in a window
        // too small for the domain, a local fixed scale scales a whole-domain
        // raster while a remote one hosts the fetched raster on a whole-domain
        // virtual canvas of finest cells. Both must report the same visible
        // physical window and paint the same viewport pixels -- scrolling is
        // what forces the two coordinate spaces apart, so an unscrolled canvas
        // proves nothing.
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        constexpr int scaleFactor = 32;
        // Drag hard against the top-left stop before measuring anything: the
        // fixed-scale transform is applied with AnchorUnderMouse, so the scroll
        // position it leaves behind is an artifact of the pointer, not of the
        // data. Both sides clamp to the same stop -- the domain origin, since
        // both scenes are the same number of view pixels across -- which leaves
        // the pan below as the only thing positioning the view.
        constexpr int anchorDrag = 512;
        constexpr int panX = -96;   // three finest cells at 32x
        constexpr int panY = -160;  // five finest cells at 32x
        enum class Await { Scale, Anchor, Pan };
        struct WindowProbe {
            bool remotePhase = false;
            Await await = Await::Scale;
            int settles = 0;
            int settlesAtLastTick = -1;
            std::array<int, 2> viewport{};
            ViewCapture localAnchored;
            ViewCapture localPanned;
        };
        auto probe = std::make_shared<WindowProbe>();
        const auto capture = [](const amrvis::qt::MainWindow& probed) {
            return ViewCapture{probed.activeViewVisibleDataWindowForTest(),
                probed.activeViewViewportImageForTest(),
                probed.activeViewImageSizeForTest()};
        };
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled, &application,
            [probe] { ++probe->settles; });
        // A remote step may or may not demand a refetch -- an anchor drag on an
        // already-anchored canvas demands none -- so rather than wait for a
        // settle that may never come, poll: a step completes once the fetched
        // raster covers the viewport and no further settle has arrived. Both
        // pans below uncover cells (the fetch keeps only one cell of slack), so
        // no step can be measured against the raster it is replacing. The timer
        // belongs to the window, which outlives every tick and owns it.
        auto* const poll = new QTimer(&window);
        poll->setInterval(100);
        QObject::connect(poll, &QTimer::timeout, &application,
            [&window, &application, probe, capture, poll] {
                const auto covered
                    = window.allViewsFixedScaleRasterCoversViewportForTest();
                const auto stable = probe->settles == probe->settlesAtLastTick;
                probe->settlesAtLastTick = probe->settles;
                if (!covered || !stable) {
                    return;
                }
                if (!window.fixedScaleStateMatchesForTest(scaleFactor)
                    || !window.activeViewScrollBarsVisibleForTest()) {
                    std::cerr << "the remote view is not in a scrolled fixed "
                                 "scale\n";
                    application.exit(1);
                    return;
                }
                // Both phases must measure the same viewport, or the two
                // visible windows are not comparable in the first place.
                if (window.activeViewViewportSizeForTest() != probe->viewport) {
                    const auto viewport
                        = window.activeViewViewportSizeForTest();
                    std::cerr << "viewport size changed between the local and "
                                 "remote phases: local=" << probe->viewport[0]
                              << 'x' << probe->viewport[1] << " remote="
                              << viewport[0] << 'x' << viewport[1] << '\n';
                    application.exit(1);
                    return;
                }
                if (probe->await == Await::Scale) {
                    probe->await = Await::Anchor;
                    window.shiftDragActiveViewForTest(anchorDrag, anchorDrag);
                    return;
                }
                const bool anchored = probe->await == Await::Anchor;
                const auto& expected = anchored
                    ? probe->localAnchored : probe->localPanned;
                const auto* label = anchored ? "anchored" : "panned";
                const auto actual = capture(window);
                if (!dataWindowsAgree(expected.window, actual.window,
                        probe->viewport)) {
                    std::cerr << label << " local/remote data window mismatch: "
                                 "local=";
                    printDataWindow(std::cerr, expected.window);
                    std::cerr << " remote=";
                    printDataWindow(std::cerr, actual.window);
                    // Raster sizes are not compared -- local holds the whole
                    // domain while remote holds only the fetched window, which
                    // is the point of the virtual canvas -- but they explain a
                    // mismatch.
                    std::cerr << " local-raster=" << expected.image[0] << 'x'
                              << expected.image[1] << " remote-raster="
                              << actual.image[0] << 'x' << actual.image[1]
                              << '\n';
                    application.exit(1);
                    return;
                }
                if (const auto difference = viewportDifference(
                        expected.viewport, actual.viewport)) {
                    std::cerr << label << " viewport content differs: "
                              << difference->summary.toStdString() << " ("
                              << difference->differingPixels << " pixels)\n";
                    application.exit(1);
                    return;
                }
                if (anchored) {
                    probe->await = Await::Pan;
                    window.shiftDragActiveViewForTest(panX, panY);
                    return;
                }
                poll->stop();
                application.exit(0);
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application,
            [&window, &application, probe, capture, poll,
                server = smokeServer,
                path = std::string(argv[2])](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                window.selectFixedScaleForTest(scaleFactor);
                if (probe->remotePhase) {
                    poll->start();
                    return;
                }
                if (!window.fixedScaleStateMatchesForTest(scaleFactor)
                    || !window.activeViewScrollBarsVisibleForTest()) {
                    std::cerr << "the local fixed scale did not overflow the "
                                 "viewport; the fixture is too small for this "
                                 "comparison\n";
                    application.exit(1);
                    return;
                }
                // Measure only after the as-needed scroll bars this scale just
                // demanded have actually been laid out: they shrink the
                // viewport, and the remote phase necessarily measures on the
                // far side of that layout pass.
                QTimer::singleShot(100, &window,
                    [&window, &application, probe, capture, path, server] {
                        probe->viewport
                            = window.activeViewViewportSizeForTest();
                        // The local raster spans the whole domain and is
                        // already loaded, so every local step is synchronous.
                        window.shiftDragActiveViewForTest(
                            anchorDrag, anchorDrag);
                        probe->localAnchored = capture(window);
                        window.shiftDragActiveViewForTest(panX, panY);
                        probe->localPanned = capture(window);
                        if (!(probe->localPanned.window.left()
                                > probe->localAnchored.window.left())
                            || !(probe->localPanned.window.top()
                                < probe->localAnchored.window.top())) {
                            std::cerr << "the local pan did not move the view "
                                         "on both axes: viewport="
                                      << probe->viewport[0] << 'x'
                                      << probe->viewport[1] << " raster="
                                      << probe->localAnchored.image[0] << 'x'
                                      << probe->localAnchored.image[1]
                                      << " anchored=";
                            printDataWindow(std::cerr,
                                probe->localAnchored.window);
                            std::cerr << " panned=";
                            printDataWindow(std::cerr,
                                probe->localPanned.window);
                            std::cerr << '\n';
                            application.exit(1);
                            return;
                        }
                        probe->remotePhase = true;
                        attachSmokeServer(window, server);
                        window.openRemoteDataset(path);
                    });
            });
        QTimer::singleShot(30000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window,
            [&window, path = std::filesystem::path(argv[2])] {
                window.resize(420, 301);
                window.openDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--remote-slice-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                application.exit(success
                        && window.activeViewUsesViewportBoundedOutputForTest()
                    ? 0 : 1);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--remote-initial-geometry-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                if (window.allViewsUseViewportBoundedOutputForTest()) {
                    application.exit(0);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application] {
                        application.exit(
                            window.allViewsUseViewportBoundedOutputForTest()
                            ? 0 : 1);
                    }, Qt::SingleShotConnection);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--remote-rubber-aspect-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                // Three settles: the wide selection, the reset back to the
                // whole domain, the tall selection. The wide and tall
                // selections err on opposite sides of the pane when the
                // arrival's framing regresses, so both must end snug — and
                // already at the first settle: the transient-scroll-bar
                // double fetch showed a mis-framed raster there before its
                // correction arrived.
                auto step = std::make_shared<int>(0);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, step] {
                        switch ((*step)++) {
                        case 0:
                            if (!window.activeViewIsZoomedForTest()
                                || !window.activeViewHasPhysicalAspectForTest(
                                    9.0 / 4.0)
                                || !window.activeViewRasterSnugForTest()) {
                                application.exit(1);
                                return;
                            }
                            window.resetZoomAllViewsForTest();
                            return;
                        case 1:
                            window.rubberBandZoomTallActiveViewForTest();
                            return;
                        default:
                            application.exit(window.activeViewIsZoomedForTest()
                                    && window.activeViewHasPhysicalAspectForTest(
                                        4.0 / 9.0)
                                    && window.activeViewRasterSnugForTest()
                                ? 0 : 1);
                        }
                    });
                window.rubberBandZoomRectangularActiveViewForTest();
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--remote-grid-boxes-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        auto boxLoads = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, boxLoads](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                window.setGridBoxesVisibleForTest(false);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, boxLoads] {
                        if (window.activeViewGridBoxCountForTest() == 0) {
                            application.exit(1);
                            return;
                        }
                        if ((*boxLoads)++ == 0) {
                            window.setGridBoxesVisibleForTest(false);
                            window.setGridBoxesVisibleForTest(true);
                            return;
                        }
                        application.exit(0);
                    });
                window.setGridBoxesVisibleForTest(true);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--remote-sequence-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        auto firstFrameDisplayed = std::make_shared<bool>(false);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application,
            [&window, &application, firstFrameDisplayed](int index) {
                if (index == 0 && !*firstFrameDisplayed) {
                    *firstFrameDisplayed = true;
                    window.stepSequence(1);
                    window.stepSequence(-1);
                    window.stepSequence(1);
                    return;
                }
                if (index == 1) {
                    application.exit(
                        window.activeViewUsesViewportBoundedOutputForTest()
                            ? 0 : 1);
                }
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteSequence({path, path});
            });
    } else if (argc == 5
        && std::string_view(argv[1])
            == "--remote-sequence-after-fab-smoke-test") {
        const std::filesystem::path fab(argv[2]);
        const std::string first(argv[3]);
        const std::string second(argv[4]);
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        auto opened = std::make_shared<bool>(false);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, first, second, opened,
                server = smokeServer](bool success) {
                if (*opened) {
                    return;
                }
                const auto* selector
                    = window.findChild<amrvis::qt::FabSelectorDock*>();
                if (!success || selector == nullptr || !selector->isVisible()
                    || !window.windowTitle().endsWith(
                        QStringLiteral(" FAB"))) {
                    application.exit(1);
                    return;
                }
                *opened = true;
                attachSmokeServer(window, server);
                window.openRemoteSequence({first, second});
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application](int index) {
                if (index == 0) {
                    application.exit(
                        window.fabStateClearedForTest() ? 0 : 1);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, fab] { window.openDataset(fab); });
    } else if (argc == 3 && std::string_view(argv[1])
            == "--fixed-scale-centre-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        // Regression for fixed-scale-switch-lands-off-center-remotely.
        // Selecting a fixed scale is supposed to keep looking at the same
        // place. Local does that implicitly, through the view's own
        // transformation anchor; remote has to re-centre explicitly, on the
        // centre viewCenterInData reports. Those two only agree if that centre
        // is the true one -- and it was a fraction of a raster pixel off, which
        // is many finest cells on a domain this wide. Open the same dataset
        // remotely, switch to 1x without touching the view, and require the
        // resulting window to be centred on the domain, which is where a
        // fitted view was looking.
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, phase](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, phase] {
                        if (*phase != 0) {
                            return;
                        }
                        *phase = 1;
                        const auto shown
                            = window.activeViewVisibleDataWindowForTest();
                        const auto domain
                            = window.datasetPhysicalDomainForTest();
                        if (!(shown.width() > 0.0)) {
                            qCritical("no visible window after the switch");
                            application.exit(1);
                            return;
                        }
                        const auto drift = std::abs(
                            shown.center().x() - domain.center().x());
                        const auto cellSize
                            = window.activeViewFinestCellSizeForTest();
                        // One finest cell of slack: the fetch window is
                        // quantised to whole cells, and nothing more than that
                        // is explainable.
                        if (drift > cellSize) {
                            qCritical("the switch left the view %g off centre "
                                      "(%g finest cells)",
                                drift, cellSize > 0.0 ? drift / cellSize : 0.0);
                            application.exit(1);
                            return;
                        }
                        application.exit(0);
                    });
                window.selectFixedScaleForTest(1);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    }
    else {
        return {false, std::nullopt};
    }
    return {true, std::nullopt};
}

} // namespace amrvis::qt::smoke
