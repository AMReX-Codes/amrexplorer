#include "SmokeHarnessInternal.hpp"

#include "MainWindow.hpp"

#include <QTimer>

#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// Volume: the Volume Rendering window over a 3-D plotfile, opened locally
// and over an in-process server. Each waits for the initial slices, opens
// the window through the same path as the View menu action, and requires
// the first frame the controller displays to have lit some pixels -- the
// ray caster ran end to end, locally or on the server, and the frame came
// back through the session, the pipeline and the window. Coverage, not
// pixels: the exact picture depends on the viewport the platform gives an
// offscreen window.

namespace amrvis::qt::smoke {

namespace {

void attachSmokeServer(amrvis::qt::MainWindow& window,
    const std::shared_ptr<amrvis::remote::Server>& server)
{
    window.useRemoteConnection(
        std::make_shared<amrvis::remote::Connection>("127.0.0.1",
            server->port(),
            amrvis::remote::ConnectionOptions{
                .clientName = "AMReXplorer Qt smoke",
                .sessionToken = server->token()}),
        QStringLiteral("127.0.0.1:%1").arg(server->port()));
}

// Arms the volume checks on `window`: open the window once the initial
// slices are in, then exit 0 on the first displayed frame with coverage,
// 1 on one without, 2 on a failed load, 3 if no frame arrives in time.
void armVolumeChecks(amrvis::qt::MainWindow& window, QApplication& application)
{
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&window, &application](bool success) {
            if (!success) {
                application.exit(2);
                return;
            }
            window.showVolumeWindowForTest();
            if (!window.volumeWindowOpenForTest()) {
                qCritical("the volume window did not open");
                application.exit(1);
                return;
            }
        });
    QObject::connect(&window, &amrvis::qt::MainWindow::volumeFrameDisplayed,
        &application, [&window, &application] {
            const auto coverage = window.volumeFrameAlphaCoverageForTest();
            if (!(coverage > 0.0)) {
                qCritical("the volume frame lit no pixels");
                application.exit(1);
                return;
            }
            application.exit(0);
        });
    QTimer::singleShot(30000, &application, [&application] { application.exit(3); });
}

} // namespace

Outcome dispatchVolume(Context& context)
{
    auto& application = context.application;
    auto& window = context.window;
    const int argc = context.argc;
    char** argv = context.argv;
    auto& smokeServer = context.server;
    auto& smokeServerThread = context.serverThread;

    if (argc == 3 && std::string_view(argv[1]) == "--volume-smoke-test") {
        const std::filesystem::path path(argv[2]);
        armVolumeChecks(window, application);
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--remote-volume-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        armVolumeChecks(window, application);
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else {
        return {false, std::nullopt};
    }
    return {true, std::nullopt};
}

} // namespace amrvis::qt::smoke
