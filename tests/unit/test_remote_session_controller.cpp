// RemoteSessionController against a loopback Server (install) and the real
// amrexplorer-server run by the stand-in ssh script (start): what it installs,
// what it asks the host to open, what it remembers per destination, and what
// its diagnostics lines say in each state, plus the Open Remote dialog and
// the browser driven from inside their exec() by a timer, the way a user
// would drive them.

#include "RemoteFileDialog.hpp"
#include "RemoteOpenDialog.hpp"
#include "RemoteSessionController.hpp"

#include "../../src/remote/Codec.hpp"

#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/Frame.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#endif

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

#ifndef _WIN32
// A peer that completes the handshake at a chosen protocol minor and then
// holds the stream open, so the client's negotiated capabilities can be set
// to anything the running server would never report.
void serveHelloAt(int descriptor, std::uint16_t minorVersion)
{
    using namespace amrvis::remote;
    const Socket peer = adoptStreamSocket(descriptor);
    const auto frame = readFrame(peer);
    require(frame.has_value(), "client closed before sending hello");
    const auto request = codec::decode(*frame);
    HelloResponseData hello;
    hello.serverName = "old peer";
    hello.softwareVersion = "test";
    hello.selectedMinorVersion = minorVersion;
    hello.maximumFrameBytes = defaultMaximumFrameBytes;
    hello.maximumDatasets = 8;
    hello.maximumOutstandingRequests = 8;
    hello.workerCount = 1;
    writeFrame(peer,
        codec::encode(
            request->request_id, codec::toWire(hello), minorVersion));
    // Until the client hangs up, so the session stays connected.
    (void)readFrame(peer);
}
#endif

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

// The visible top-level dialog of a type, if one is up (a modal exec() is
// running under it).
template <typename Dialog>
Dialog* visibleDialog()
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        // dynamic_cast: the dialogs have no Q_OBJECT (nothing to moc).
        if (auto* dialog = dynamic_cast<Dialog*>(widget);
            dialog != nullptr && dialog->isVisible()) {
            return dialog;
        }
    }
    return nullptr;
}

QPushButton* buttonNamed(QWidget& dialog, const QString& text)
{
    const auto buttons = dialog.findChildren<QPushButton*>();
    const auto found = std::find_if(buttons.begin(), buttons.end(),
        [&](QPushButton* button) { return button->text() == text; });
    return found == buttons.end() ? nullptr : *found;
}

// Drives a modal dialog from inside its own exec() loop: `step` runs every
// few milliseconds until it returns true. What a user would do with the
// mouse, done by a timer.
class Driver {
public:
    explicit Driver(std::function<bool()> step)
        : m_step(std::move(step))
    {
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] {
            if (m_step()) {
                m_done = true;
                m_timer.stop();
            }
        });
        m_timer.start(20);
    }
    [[nodiscard]] bool done() const noexcept { return m_done; }

private:
    std::function<bool()> m_step;
    QTimer m_timer;
    bool m_done = false;
};

// Everything the host would react to.
struct Observed {
    int sessionChanges = 0;
    std::vector<std::vector<std::string>> opens;
    std::vector<bool> openAsSequence;
    std::vector<std::string> companions;
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
        &amrvis::qt::RemoteSessionController::companionRequested, &controller,
        [&observed](const std::string& path) { observed.companions.push_back(path); });
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

#ifndef _WIN32
    // A server predating full-precision values (protocol 1.5): the notice is
    // a retained property of the session, not a line said once. The status
    // bar cannot carry it -- the open that follows the ready message
    // overwrites it in the same event-loop turn -- so it has to outlive that.
    {
        int descriptors[2] = {-1, -1};
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
            "socketpair failed");
        std::thread peer([fd = descriptors[0]] { serveHelloAt(fd, 4); });
        auto connection = std::make_shared<amrvis::remote::Connection>(
            std::make_unique<amrvis::remote::Socket>(
                amrvis::remote::adoptStreamSocket(descriptors[1])),
            amrvis::remote::ConnectionOptions{.sessionToken = "any-token"});
        require(!connection->supportsDoublePrecisionValues(),
            "a 1.4 peer was taken for one that sends doubles");

        RemoteSessionController controller(hooks(), "test");
        controller.install(connection, QStringLiteral("ssh old"));
        require(controller.valuePrecisionNotice().contains(
                    QStringLiteral("protocol 1.5")),
            "an older server left no precision notice");
        require(controller.diagnosticsLines().contains(
                    QStringLiteral("\nremote values: float")),
            "diagnostics did not keep the precision notice");
        connection->close();
        peer.join();
        require(controller.valuePrecisionNotice().isEmpty()
                && !controller.diagnosticsLines().contains(
                    QStringLiteral("remote values")),
            "a dead session kept warning about values it cannot send");

        // And a current server clears it, rather than leaving the last
        // session's warning standing over full-precision values.
        amrvis::remote::Server server;
        RunningServer running(server);
        controller.install(std::make_shared<amrvis::remote::Connection>(
                               "127.0.0.1", server.port(),
                               amrvis::remote::ConnectionOptions{
                                   .sessionToken = server.token()}),
            QStringLiteral("127.0.0.1:test"));
        require(controller.valuePrecisionNotice().isEmpty()
                && !controller.diagnosticsLines().contains(
                    QStringLiteral("remote values")),
            "a current server kept the older session's precision notice");
    }
#endif

    // start(): destination validation never touches the settings.
    {
        Observed observed;
        RemoteSessionController controller(hooks(), "test");
        observe(controller, observed);
        // The destination goes into ssh's argv after "--", so an option-like
        // or multi-word one is refused before anything is remembered or
        // started: neither "-oProxyCommand=..." nor "host -o ProxyCommand=..."
        // nor a shell-looking "host; touch /tmp/x" may reach ssh.
        for (const char* bad : {"-bad", "-oProxyCommand=touch /tmp/x",
                 "host -o ProxyCommand=touch /tmp/x", "host; touch /tmp/x",
                 "host\tuser", "host\n", ""}) {
            controller.start(bad, "", {});
        }
        require(observed.errors.size() == 7
                && std::all_of(observed.errors.begin(), observed.errors.end(),
                    [](const QString& error) {
                        return error == QStringLiteral("Invalid SSH destination.");
                    })
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
    // The server the stand-in ssh runs inherits this process's HOME; point it
    // at the scratch directory holding one fake plotfile so the browser has
    // something to pick.
    const auto home = std::filesystem::path(scratch.path().toStdString());
    std::filesystem::create_directories(home / "plt00000" / "Level_0");
    std::ofstream(home / "plt00000" / "Header") << "fake plotfile header\n";
    qputenv("HOME", scratch.path().toUtf8());

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
        // Per destination only: what one machine remembers must not leak to
        // another (the bug the executable key's comment describes).
        require(controller.serverExecutableFor(QStringLiteral("elsewhere"))
                == QStringLiteral("amrexplorer-server"),
            "a remembered executable leaked to another destination");

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

        // The Open Remote dialog, driven from inside its exec(): with the
        // live session's fields left alone, Open asks the host to open the
        // typed path directly (no new session); the sequence dialog asks for
        // a sequence; an empty path is refused with a warning; Cancel does
        // nothing; a different destination starts a new session; Browse...
        // opens the remote browser over the live session, whose pick is
        // opened.
        using amrvis::qt::RemoteOpenDialog;
        const auto statusesBefore = observed.statuses.size();
        {
            Driver driver([] {
                auto* dialog = visibleDialog<RemoteOpenDialog>();
                if (dialog == nullptr) {
                    return false;
                }
                dialog->findChildren<QLineEdit*>()[2]->setText(
                    QStringLiteral("/scratch/plt00030"));
                buttonNamed(*dialog, QStringLiteral("Open"))->click();
                return true;
            });
            controller.promptOpen(nullptr, false);
            require(driver.done() && observed.opens.size() == 3
                    && observed.opens.back()
                        == std::vector<std::string>{"/scratch/plt00030"}
                    && !observed.openAsSequence.back()
                    && observed.statuses.size() == statusesBefore,
                "Open over the live session did not open the path directly");
        }
        {
            Driver driver([] {
                auto* dialog = visibleDialog<RemoteOpenDialog>();
                if (dialog == nullptr) {
                    return false;
                }
                dialog->findChild<QPlainTextEdit*>()->setPlainText(
                    QStringLiteral("/a/plt00010\n/a/plt00020\n"));
                buttonNamed(*dialog, QStringLiteral("Open"))->click();
                return true;
            });
            controller.promptOpen(nullptr, true);
            require(driver.done() && observed.opens.size() == 4
                    && observed.opens.back().size() == 2
                    && observed.openAsSequence.back(),
                "the sequence dialog did not ask for a sequence");
        }
        {
            // One path in the sequence dialog is refused up front, for the
            // live session and a new one alike.
            bool warned = false;
            Driver driver([&warned] {
                if (auto* warning = visibleDialog<QMessageBox>()) {
                    warned = true;
                    warning->buttons().first()->click();
                    return true;
                }
                if (auto* dialog = visibleDialog<RemoteOpenDialog>()) {
                    dialog->findChild<QPlainTextEdit*>()->setPlainText(
                        QStringLiteral("/a/plt00010\n"));
                    buttonNamed(*dialog, QStringLiteral("Open"))->click();
                }
                return false;
            });
            controller.promptOpen(nullptr, true);
            require(driver.done() && warned && observed.opens.size() == 4
                    && observed.statuses.size() == statusesBefore,
                "a one-path sequence was opened, or the warning did not show");
        }
        {
            bool warned = false;
            Driver driver([&warned] {
                if (auto* warning = visibleDialog<QMessageBox>()) {
                    warned = true;
                    warning->buttons().first()->click();
                    return true;
                }
                if (auto* dialog = visibleDialog<RemoteOpenDialog>()) {
                    buttonNamed(*dialog, QStringLiteral("Open"))->click();
                }
                return false;
            });
            controller.promptOpen(nullptr, false);
            require(driver.done() && warned && observed.opens.size() == 4
                    && observed.statuses.size() == statusesBefore,
                "an empty path was opened, or the warning did not show");
        }
        {
            Driver driver([] {
                auto* dialog = visibleDialog<RemoteOpenDialog>();
                if (dialog == nullptr) {
                    return false;
                }
                buttonNamed(*dialog, QStringLiteral("Cancel"))->click();
                return true;
            });
            controller.promptOpen(nullptr, false);
            require(driver.done() && observed.opens.size() == 4
                    && observed.statuses.size() == statusesBefore,
                "Cancel opened or started something");
        }
        {
            Driver driver([&serverBinary] {
                auto* dialog = visibleDialog<RemoteOpenDialog>();
                if (dialog == nullptr) {
                    return false;
                }
                const auto edits = dialog->findChildren<QLineEdit*>();
                edits[0]->setText(QStringLiteral("other-destination"));
                edits[1]->setText(QString::fromStdString(serverBinary));
                edits[2]->setText(QStringLiteral("/scratch/plt00040"));
                buttonNamed(*dialog, QStringLiteral("Open"))->click();
                return true;
            });
            controller.promptOpen(nullptr, false);
            require(driver.done()
                    && observed.statuses.size() == statusesBefore + 1
                    && observed.statuses.back().contains(QStringLiteral(
                        "Starting remote session on other-destination")),
                "a changed destination did not start a new session");
            require(waitUntil([&] { return observed.opens.size() == 5; }, 20000),
                "the new session did not open the typed path");
            require(observed.opens.back()
                        == std::vector<std::string>{"/scratch/plt00040"}
                    && controller.connectionGeneration() == 3
                    && controller.diagnosticsLines().contains(
                        QStringLiteral("ssh other-destination (connected)")),
                "the new session did not replace the old one");
        }
        {
            // Browse... over the (matching) live session: the browser lists
            // the server's home -- the scratch directory -- and picking its
            // plotfile opens it.
            Driver driver([] {
                if (auto* browser
                    = visibleDialog<amrvis::qt::RemoteFileDialog>()) {
                    auto* entries = browser->findChild<QTreeWidget*>();
                    for (int index = 0; index < entries->topLevelItemCount();
                         ++index) {
                        auto* item = entries->topLevelItem(index);
                        if (item->text(0) == QStringLiteral("plt00000")) {
                            item->setSelected(true);
                            browser->findChild<QDialogButtonBox*>()
                                ->button(QDialogButtonBox::Open)
                                ->click();
                            return true;
                        }
                    }
                    return false;
                }
                if (auto* dialog = visibleDialog<RemoteOpenDialog>()) {
                    buttonNamed(*dialog, QStringLiteral("Browse..."))->click();
                }
                return false;
            });
            controller.promptOpen(nullptr, false);
            require(driver.done() && observed.opens.size() == 6
                    && observed.opens.back()
                        == std::vector<std::string>{(home / "plt00000").string()}
                    && !observed.openAsSequence.back(),
                "Browse... did not open the picked plotfile");
            require(QSettings(settingsFile, QSettings::IniFormat)
                        .value(QStringLiteral(
                            "remote/lastDirectories/other-destination"))
                        .toString()
                    == QString::fromStdString(home.string()),
                "the browsed directory was not remembered per destination");
        }
        {
            // chooseRemotePlotfile: the same browser over a given connection,
            // handing the pick back instead of asking the host to open it
            // (a companion rides the primary's own connection). Cancel is an
            // empty pick; a dead or missing connection is an error.
            const auto pick = [](QDialogButtonBox::StandardButton button) {
                return [button] {
                    auto* browser = visibleDialog<amrvis::qt::RemoteFileDialog>();
                    if (browser == nullptr) {
                        return false;
                    }
                    auto* entries = browser->findChild<QTreeWidget*>();
                    for (int index = 0; index < entries->topLevelItemCount();
                         ++index) {
                        auto* item = entries->topLevelItem(index);
                        if (item->text(0) == QStringLiteral("plt00000")) {
                            item->setSelected(true);
                            browser->findChild<QDialogButtonBox*>()
                                ->button(button)
                                ->click();
                            return true;
                        }
                    }
                    return false;
                };
            };
            const auto opensBefore = observed.opens.size();
            const auto errorsBefore = observed.errors.size();
            Driver open(pick(QDialogButtonBox::Open));
            const auto picked
                = controller.chooseRemotePlotfile(nullptr, controller.connection());
            require(open.done() && picked == (home / "plt00000").string()
                    && observed.opens.size() == opensBefore,
                "chooseRemotePlotfile did not hand back the picked plotfile");
            Driver cancel(pick(QDialogButtonBox::Cancel));
            const auto cancelled
                = controller.chooseRemotePlotfile(nullptr, controller.connection());
            require(cancel.done() && cancelled.empty()
                    && observed.errors.size() == errorsBefore,
                "a cancelled browser did not come back empty");
            require(controller.chooseRemotePlotfile(nullptr, nullptr).empty()
                    && observed.errors.size() == errorsBefore + 1
                    && observed.opens.size() == opensBefore,
                "a missing connection was not reported");
        }
        {
            // promptCompanion: a typed path over the live session comes back
            // through companionRequested, nothing is opened and no session
            // starts; with sameServer a changed destination is refused with a
            // warning; Browse... hands the browser's pick back.
            const auto opensBefore = observed.opens.size();
            const auto statusesAtCompanion = observed.statuses.size();
            Driver typed([] {
                auto* dialog = visibleDialog<RemoteOpenDialog>();
                if (dialog == nullptr) {
                    return false;
                }
                dialog->findChildren<QLineEdit*>()[2]->setText(
                    QStringLiteral("/scratch/ocean"));
                buttonNamed(*dialog, QStringLiteral("Open"))->click();
                return true;
            });
            controller.promptCompanion(nullptr, true);
            require(typed.done() && observed.companions.size() == 1
                    && observed.companions.back() == "/scratch/ocean"
                    && observed.opens.size() == opensBefore
                    && observed.statuses.size() == statusesAtCompanion,
                "a typed companion path was not handed back over the live session");
            Driver changed([] {
                if (auto* warning = visibleDialog<QMessageBox>()) {
                    warning->buttons().first()->click();
                    return true;
                }
                auto* dialog = visibleDialog<RemoteOpenDialog>();
                if (dialog == nullptr) {
                    return false;
                }
                const auto edits = dialog->findChildren<QLineEdit*>();
                edits[0]->setText(QStringLiteral("third-destination"));
                edits[2]->setText(QStringLiteral("/scratch/ocean"));
                buttonNamed(*dialog, QStringLiteral("Open"))->click();
                return false;
            });
            controller.promptCompanion(nullptr, true);
            require(changed.done() && observed.companions.size() == 1
                    && observed.statuses.size() == statusesAtCompanion,
                "a companion from another server was accepted beside a remote primary");
            Driver browsed([] {
                if (auto* browser = visibleDialog<amrvis::qt::RemoteFileDialog>()) {
                    auto* entries = browser->findChild<QTreeWidget*>();
                    for (int index = 0; index < entries->topLevelItemCount(); ++index) {
                        auto* item = entries->topLevelItem(index);
                        if (item->text(0) == QStringLiteral("plt00000")) {
                            item->setSelected(true);
                            browser->findChild<QDialogButtonBox*>()
                                ->button(QDialogButtonBox::Open)
                                ->click();
                            return true;
                        }
                    }
                    return false;
                }
                if (auto* dialog = visibleDialog<RemoteOpenDialog>()) {
                    buttonNamed(*dialog, QStringLiteral("Browse..."))->click();
                }
                return false;
            });
            controller.promptCompanion(nullptr, false);
            require(browsed.done() && observed.companions.size() == 2
                    && observed.companions.back() == (home / "plt00000").string()
                    && observed.opens.size() == opensBefore,
                "Browse... did not hand the companion pick back");
        }
        controller.shutdown();
        require(controller.diagnosticsLines().contains(
                    QStringLiteral("remote session: ssh other-destination")),
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
