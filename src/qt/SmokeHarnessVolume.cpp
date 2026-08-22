#include "SmokeHarnessInternal.hpp"

#include "MainWindow.hpp"
#include "VolumeWindow.hpp"
#include "WidgetImageExport.hpp"

#include <QAction>
#include <QApplication>
#include <QImage>
#include <QString>
#include <QTimer>

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

// The one Volume window on screen. The controller parents it for placement
// only, so it is a top-level widget rather than a child of the main window.
amrvis::qt::VolumeWindow* volumeWindow()
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = qobject_cast<amrvis::qt::VolumeWindow*>(widget)) {
            return window;
        }
    }
    return nullptr;
}

// Arms the export checks: once a frame is on screen, take the picture the
// File > Export Image... action would take and write it, then read it back.
// Exercised end to end against a real rendered frame -- the name rule and the
// child-exclusion rule have their own unit test; what this adds is that the
// action is reachable and the seam produces a file on a real widget.
void armExportChecks(amrvis::qt::MainWindow& window, QApplication& application,
    const QString& outputPath)
{
    QObject::connect(&window, &amrvis::qt::MainWindow::volumeFrameDisplayed,
        &application, [&application, outputPath] {
            auto* volume = volumeWindow();
            if (volume == nullptr) {
                qCritical("no volume window on screen");
                application.exit(1);
                return;
            }
            // Named, so a harness can find it: an anonymous action is one no
            // test can reach, which is how three bugs shipped in the slot
            // behind this one.
            if (volume->findChild<QAction*>(
                    QStringLiteral("volumeExportImageAction"))
                == nullptr) {
                qCritical("the export action is not reachable by name");
                application.exit(1);
                return;
            }
            const auto image = volume->renderedView(1.0);
            if (image.isNull() || image.size() != volume->viewSize()) {
                qCritical("the exported view is empty or not the view's size");
                application.exit(1);
                return;
            }
            const auto path = amrvis::qt::pngExportPath(outputPath);
            if (!image.save(path, "PNG")) {
                qCritical("the exported image did not save");
                application.exit(1);
                return;
            }
            QImage reloaded;
            if (!reloaded.load(path) || reloaded.size() != image.size()) {
                qCritical("the written file did not read back at its own size");
                application.exit(1);
                return;
            }
            application.exit(0);
        });
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
    } else if (argc == 4
        && std::string_view(argv[1]) == "--volume-export-smoke-test") {
        const std::filesystem::path path(argv[2]);
        // Its own open handler rather than armVolumeChecks: the coverage check
        // there exits 0 on the first frame, which would end the run before the
        // export ever happened.
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                window.showVolumeWindowForTest();
            });
        armExportChecks(window, application, QString::fromUtf8(argv[3]));
        QTimer::singleShot(30000, &application,
            [&application] { application.exit(3); });
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
