#include "SmokeHarnessInternal.hpp"

#include "MainWindow.hpp"
#include "VolumeWindow.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QTimer>

#include <amrexplorer/remote/Server.hpp>

#include <filesystem>
#include <functional>
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
// offscreen window. The isosurface variant drives the window's own controls
// before that first frame: an isosurface alone, the volume hidden, so the
// lit pixels can only have come from the surface.

namespace amrvis::qt::smoke {

namespace {

// Arms the volume checks on `window`: open the window once the initial
// slices are in (and let `configure` set its controls before the first
// render), then exit 0 on the first displayed frame with coverage, 1 on one
// without, 2 on a failed load, 3 if no frame arrives in time.
void armVolumeChecks(amrvis::qt::MainWindow& window, QApplication& application,
    std::function<bool(amrvis::qt::VolumeWindow&)> configure = {})
{
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&window, &application, configure](bool success) {
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
            // Before the render throttle fires, so the first frame is the
            // configured one.
            if (configure && !configure(*window.volumeWindowForTest())) {
                qCritical("the volume window's controls could not be set");
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

// The isosurface alone: a value typed in the middle of the fixture's field
// (the 3-D fixture is (i + j + k) / 9, so 0.5 is a plane across it), the
// surface on, the volume off. Through the named controls, the way a user
// gets there; false if the window lacks them.
bool configureIsosurfaceOnly(amrvis::qt::VolumeWindow& volume)
{
    auto* const value = volume.findChild<QDoubleSpinBox*>(
        QStringLiteral("volumeIsosurfaceValueSpin"));
    auto* const group = volume.findChild<QGroupBox*>(
        QStringLiteral("volumeIsosurfaceGroup"));
    auto* const showVolume = volume.findChild<QCheckBox*>(
        QStringLiteral("volumeShowVolumeCheck"));
    if (value == nullptr || group == nullptr || showVolume == nullptr) {
        return false;
    }
    value->setValue(0.5);
    group->setChecked(true);
    showVolume->setChecked(false);
    return volume.isosurface().has_value() && !volume.showVolume();
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
    } else if (argc == 3 && std::string_view(argv[1]) == "--isosurface-smoke-test") {
        const std::filesystem::path path(argv[2]);
        armVolumeChecks(window, application, configureIsosurfaceOnly);
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
