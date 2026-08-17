// RemoteSessionController against a loopback Server (install) and the real
// amrexplorer-server run by the stand-in ssh script (start): what it installs,
// what it asks the host to open, what it remembers per destination, and what
// its diagnostics lines say in each state. The dialogs it owns are covered by
// their own tests; this one never opens a modal dialog.

#include "RemoteSessionController.hpp"

#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

class RunningServer {
public:
    explicit RunningServer(amrvis::remote::Server& server)
        : m_server(server)
        , m_thread([this] {
            try {
                m_server.run();
            } catch (...) {
                m_failure = std::current_exception();
            }
        })
    {
    }

    ~RunningServer()
    {
        m_server.requestStop();
        m_thread.join();
        if (m_failure) {
            std::terminate();
        }
    }

private:
    amrvis::remote::Server& m_server;
    std::thread m_thread;
    std::exception_ptr m_failure;
};

bool waitUntil(const std::function<bool()>& predicate, int milliseconds)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate()) {
        if (elapsed.elapsed() > milliseconds) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

// Everything the host would react to.
struct Observed {
    int sessionChanges = 0;
    std::vector<std::vector<std::string>> opens;
    std::vector<bool> openAsSequence;
    QStringList statuses;
    QStringList errors;
};

void observe(amrvis::qt::RemoteSessionController& controller, Observed& observed)
{
    QObject::connect(&controller,
        &amrvis::qt::RemoteSessionController::sessionChanged, &controller,
        [&observed] { ++observed.sessionChanges; });
    QObject::connect(&controller,
        &amrvis::qt::RemoteSessionController::openRequested, &controller,
        [&observed](const std::vector<std::string>& paths, bool sequence) {
            observed.opens.push_back(paths);
            observed.openAsSequence.push_back(sequence);
        });
    QObject::connect(&controller,
        &amrvis::qt::RemoteSessionController::statusMessage, &controller,
        [&observed](const QString& message, int) {
            observed.statuses << message;
        });
    QObject::connect(&controller,
        &amrvis::qt::RemoteSessionController::errorReported, &controller,
        [&observed](const QString& message) { observed.errors << message; });
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    using amrvis::qt::RemoteSessionController;

    QTemporaryDir scratch;
    require(scratch.isValid(), "could not create a scratch directory");
    const auto settingsFile = scratch.filePath(QStringLiteral("settings.ini"));
    bool shuttingDown = false;
    std::optional<std::string> currentRemotePath;
    const auto hooks = [&] {
        return RemoteSessionController::Hooks{
            [&shuttingDown] { return shuttingDown; },
            [&currentRemotePath] { return currentRemotePath; },
            [settingsFile] {
                return std::make_unique<QSettings>(
                    settingsFile, QSettings::IniFormat);
            },
        };
    };

    // install(): the connection, its label, the generation, the diagnostics
    // lines with and without a remote dataset on show.
    {
        amrvis::remote::Server server;
        RunningServer running(server);
        auto connection = std::make_shared<amrvis::remote::Connection>(
            "127.0.0.1", server.port(),
            amrvis::remote::ConnectionOptions{
                .clientName = "remote session controller test",
                .sessionToken = server.token()});

        Observed observed;
        RemoteSessionController controller(hooks(), "test");
        observe(controller, observed);
        require(!controller.connection() && !controller.connected()
                && controller.connectionGeneration() == 0
                && controller.diagnosticsLines().isEmpty(),
            "a fresh controller reports a session");
        controller.install(connection, QStringLiteral("127.0.0.1:test"));
        require(controller.connection() == connection && controller.connected()
                && controller.connectionGeneration() == 1
                && observed.sessionChanges == 1,
            "install did not install the connection");
        require(controller.diagnosticsLines()
                == QStringLiteral("\nremote session: 127.0.0.1:test (connected)"),
            "diagnostics did not describe the connected session");
        currentRemotePath = "/scratch/run/plt00010";
        require(controller.diagnosticsLines().contains(
                    QStringLiteral("\nremote path: /scratch/run/plt00010")),
            "diagnostics did not name the remote dataset's path");
        currentRemotePath.reset();
        controller.install(connection, QStringLiteral("again"));
        require(controller.connectionGeneration() == 2
                && observed.sessionChanges == 2,
            "a second install did not bump the generation");
        connection->close();
        require(!controller.connected() && controller.connection() == connection
                && controller.diagnosticsLines().contains(
                    QStringLiteral("(disconnected)")),
            "a closed connection still reads as connected");
        require(controller.serverExecutableFor(QStringLiteral("anywhere"))
                == QStringLiteral("amrexplorer-server"),
            "an unknown destination did not default the executable");
    }

    // start(): destination validation never touches the settings.
    {
        Observed observed;
        RemoteSessionController controller(hooks(), "test");
        observe(controller, observed);
        controller.start("-bad", "", {});
        require(observed.errors.size() == 1
                && observed.errors.front()
                    == QStringLiteral("Invalid SSH destination.")
                && observed.sessionChanges == 0
                && controller.diagnosticsLines().isEmpty(),
            "an invalid destination was not rejected up front");
        require(QSettings(settingsFile, QSettings::IniFormat)
                    .value(QStringLiteral("remote/sshDestination"))
                    .toString()
                    .isEmpty(),
            "an invalid destination was remembered");
    }

#ifndef _WIN32
    if (argc < 3) {
        std::cerr << "usage: test_remote_session_controller SERVER FAKE_SSH\n";
        return 2;
    }
    const std::string serverBinary = argv[1];
    qputenv("AMREXPLORER_SSH", argv[2]);

    // start() over the stand-in ssh: the connection is installed, the paths
    // go to the host (one as a dataset, two as a sequence), onReady runs
    // after, the destination and executable are remembered per destination.
    {
        Observed observed;
        RemoteSessionController controller(hooks(), "test");
        observe(controller, observed);
        int readyCalls = 0;
        controller.start("fake-destination", serverBinary,
            {"/scratch/plt00010"}, [&] { ++readyCalls; });
        require(observed.statuses.size() == 1
                && observed.statuses.front().contains(
                    QStringLiteral("Starting remote session on fake-destination"))
                && observed.sessionChanges == 1
                && controller.diagnosticsLines()
                    == QStringLiteral(
                        "\nremote session: ssh fake-destination (starting)"),
            "start did not announce the session");
        require(waitUntil([&] { return readyCalls == 1; }, 20000),
            "the session did not become ready");
        require(controller.connected() && controller.connectionGeneration() == 1
                && observed.sessionChanges == 2,
            "the ready session did not install its connection");
        require(observed.opens.size() == 1
                && observed.opens.front()
                    == std::vector<std::string>{"/scratch/plt00010"}
                && observed.openAsSequence.front() == false,
            "the ready session did not ask to open the path as a dataset");
        require(observed.statuses.size() == 2
                && observed.statuses.back().contains(
                    QStringLiteral("Remote session on fake-destination is ready")),
            "the ready session did not report itself");
        require(controller.diagnosticsLines()
                == QStringLiteral(
                    "\nremote session: ssh fake-destination (connected)"),
            "diagnostics did not describe the ssh session");
        QSettings settings(settingsFile, QSettings::IniFormat);
        require(settings.value(QStringLiteral("remote/sshDestination")).toString()
                    == QStringLiteral("fake-destination")
                && settings
                        .value(QStringLiteral(
                            "remote/serverExecutables/fake-destination"))
                        .toString()
                    == QString::fromStdString(serverBinary),
            "the destination and executable were not remembered");
        require(controller.serverExecutableFor(QStringLiteral("fake-destination"))
                == QString::fromStdString(serverBinary),
            "serverExecutableFor did not read the remembered executable");

        // A second start with the remembered executable (empty argument) and
        // two paths replaces the session and asks for a sequence.
        controller.start("fake-destination", "",
            {"/scratch/plt00010", "/scratch/plt00020"});
        require(waitUntil([&] { return observed.opens.size() == 2; }, 20000),
            "the second session did not become ready");
        require(observed.openAsSequence.back()
                && observed.opens.back().size() == 2
                && controller.connectionGeneration() == 2,
            "two paths were not requested as a sequence over a new session");
        controller.shutdown();
        require(controller.diagnosticsLines().contains(
                    QStringLiteral("remote session: ssh fake-destination")),
            "shutdown dropped the installed connection's line");
    }

    // A session that cannot start: the error is reported once, nothing is
    // installed, and the diagnostics line goes back to nothing.
    {
        qputenv("AMREXPLORER_FAKE_SSH_MODE", "fail");
        Observed observed;
        RemoteSessionController controller(hooks(), "test");
        observe(controller, observed);
        controller.start("fake-destination", serverBinary, {});
        require(waitUntil([&] { return !observed.errors.isEmpty(); }, 20000),
            "the failed session did not report");
        require(observed.errors.size() == 1
                && observed.errors.front().startsWith(QStringLiteral(
                    "Could not start the remote session on fake-destination"))
                && !controller.connected() && observed.opens.empty()
                && observed.statuses.back()
                    == QStringLiteral("Could not start the remote session"),
            "the failed session installed something or reported twice");
        qunsetenv("AMREXPLORER_FAKE_SSH_MODE");
    }

    // Once the host is shutting down, a late ready callback does nothing.
    {
        Observed observed;
        RemoteSessionController controller(hooks(), "test");
        observe(controller, observed);
        controller.start("fake-destination", serverBinary, {"/scratch/plt"});
        shuttingDown = true;
        static_cast<void>(waitUntil([] { return false; }, 1500));
        require(!controller.connected() && observed.opens.empty()
                && observed.errors.isEmpty(),
            "a ready callback acted on a host that is shutting down");
        shuttingDown = false;
    }
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
#endif

    std::cout << "remote session controller tests passed\n";
    return 0;
}
