#include "SshRemoteSession.hpp"

#include <amrexplorer/remote/Connection.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void unitTests()
{
    using amrvis::qt::parseSshServerReadyLine;
    using amrvis::qt::sshAskpassEnvironment;
    using amrvis::qt::sshRemoteProcessArguments;
    using amrvis::qt::sshRemoteServerCommand;

    const auto ready
        = parseSshServerReadyLine("AMREXPLORER-STDIO 1 TOKEN secret-token\r");
    require(ready && ready->version == 1 && ready->token == "secret-token",
        "valid ready line was not parsed");
    // A prompt printed without a newline may sit in front of the marker.
    const auto prefixed = parseSshServerReadyLine(
        "login$ AMREXPLORER-STDIO 1 TOKEN abc");
    require(prefixed && prefixed->token == "abc",
        "ready line after prompt text was not parsed");
    require(!parseSshServerReadyLine("AMREXPLORER-STDIO 2 TOKEN abc"),
        "unsupported ready line version was accepted");
    require(!parseSshServerReadyLine("AMREXPLORER-STDIO 1 TOKEN "),
        "empty token was accepted");
    require(!parseSshServerReadyLine("AMREXPLORER-STDIO 1 TOKEN a b"),
        "token containing whitespace was accepted");
    require(!parseSshServerReadyLine("LISTENING 127.0.0.1 41419 TOKEN abc"),
        "loopback listener line was taken for a stdio ready line");
    require(!parseSshServerReadyLine("Welcome to the login node"),
        "banner line was taken for a ready line");

    const auto quoted = sshRemoteServerCommand(
        "/opt/AMReXplorer's build/bin/amrexplorer-server");
    require(quoted
            && *quoted
                == "exec '/opt/AMReXplorer'\\''s build/bin/amrexplorer-server'"
                   " --stdio --threads 8",
        "remote command did not quote the executable path");
    const auto homeRelative
        = sshRemoteServerCommand("~/AMReXplorer build/amrexplorer-server");
    require(homeRelative
            && *homeRelative
                == "exec \"$HOME\"/'AMReXplorer build/amrexplorer-server'"
                   " --stdio --threads 8",
        "home-relative executable was not spelled through $HOME");
    require(!sshRemoteServerCommand(""), "empty executable was accepted");
    require(!sshRemoteServerCommand("bad\nname"),
        "executable containing a newline was accepted");
    const auto home = sshRemoteServerCommand("~");
    require(home && *home == "exec \"$HOME\" --stdio --threads 8",
        "bare ~ was not spelled through $HOME");

    const auto arguments
        = sshRemoteProcessArguments("frontier", "~/bin/amrexplorer-server");
    require(arguments
            && *arguments
                == QStringList{QStringLiteral("-T"), QStringLiteral("-o"),
                    QStringLiteral("ClearAllForwardings=yes"),
                    QStringLiteral("-o"),
                    QStringLiteral("ServerAliveInterval=15"),
                    QStringLiteral("-o"),
                    QStringLiteral("ServerAliveCountMax=3"),
                    QStringLiteral("-o"), QStringLiteral("ConnectTimeout=30"),
                    QStringLiteral("--"), QStringLiteral("frontier"),
                    QStringLiteral("exec \"$HOME\"/'bin/amrexplorer-server' "
                                   "--stdio --threads 8")},
        "ssh arguments were not built as expected");
    require(!sshRemoteProcessArguments("-oProxyCommand=x", "server"),
        "option-like destination was accepted");
    require(!sshRemoteProcessArguments("host name", "server"),
        "destination with whitespace was accepted");

    const auto environment
        = sshAskpassEnvironment(QStringLiteral("/Applications/AMReXplorer"));
    require(environment.value(QStringLiteral("SSH_ASKPASS"))
                == QStringLiteral("/Applications/AMReXplorer")
            && environment.value(QStringLiteral("SSH_ASKPASS_REQUIRE"))
                == QStringLiteral("force")
            && environment.value(QStringLiteral("AMREXPLORER_SSH_ASKPASS_MODE"))
                == QStringLiteral("1"),
        "askpass environment was not configured");
}

#ifndef _WIN32

// Spins the event loop until the predicate holds or the bound passes.
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

struct Outcome {
    std::shared_ptr<amrvis::remote::Connection> connection;
    QString error;
    QString lost;
    int readyCalls = 0;
    int errorCalls = 0;
    int lostCalls = 0;
};

void startSession(amrvis::qt::SshRemoteSession& session,
    const std::string& serverBinary, Outcome& outcome)
{
    session.start("fake-destination", serverBinary,
        amrvis::remote::ConnectionOptions{
            .clientName = "AMReXplorer test", .softwareVersion = "test",
            .sessionToken = {}},
        [&outcome](std::shared_ptr<amrvis::remote::Connection> connection) {
            ++outcome.readyCalls;
            outcome.connection = std::move(connection);
        },
        [&outcome](QString message) {
            ++outcome.errorCalls;
            outcome.error = std::move(message);
        },
        [&outcome](QString message) {
            ++outcome.lostCalls;
            outcome.lost = std::move(message);
        });
}

// The whole client flow against the real server: ready with a connected
// Connection, requests work, and stop() ends the server through end of
// stream alone -- exit code 0, no signal.
void sessionRoundTrip(const std::string& serverBinary,
    const std::string& datasetPath, const char* mode)
{
    qputenv("AMREXPLORER_FAKE_SSH_MODE", mode);
    amrvis::qt::SshRemoteSession session;
    Outcome outcome;
    startSession(session, serverBinary, outcome);
    require(waitUntil([&] { return outcome.readyCalls > 0
                                || outcome.errorCalls > 0; }, 20000),
        "session neither became ready nor failed");
    if (outcome.errorCalls > 0) {
        std::cerr << outcome.error.toStdString() << '\n';
    }
    require(outcome.readyCalls == 1 && outcome.errorCalls == 0,
        "session did not become ready");
    require(session.ready() && session.connection() == outcome.connection
            && outcome.connection->connected(),
        "ready callback did not carry the live connection");
    require(session.destination() == "fake-destination"
            && session.serverExecutable() == serverBinary,
        "session did not report what it was started with");
    outcome.connection->ping();
    const auto opened
        = outcome.connection->openDataset(datasetPath, 16ULL << 20);
    require(opened.catalog.dimension == 2,
        "dataset could not be opened over the ssh session");
    outcome.connection->closeDataset(opened.id);

    session.stop();
    require(!outcome.connection->connected(),
        "stop() left the connection open");
    require(waitUntil([&] { return !session.processRunning(); }, 5000),
        "ssh process did not exit after stop()");
    const auto exitCode = session.processExitCode();
    require(exitCode && *exitCode == 0,
        "server did not exit cleanly on end of stream");
    require(outcome.lostCalls == 0 && outcome.errorCalls == 0,
        "callbacks fired after stop()");
}

// stop() during the handshake returns at once and the wedged process is
// ended; no callback fires afterwards.
void stopDuringHandshake(const std::string& serverBinary)
{
    qputenv("AMREXPLORER_FAKE_SSH_MODE", "silent");
    amrvis::qt::SshRemoteSession session;
    Outcome outcome;
    startSession(session, serverBinary, outcome);
    // Give the ready line time to arrive and the handshake to start.
    waitUntil([] { return false; }, 500);
    require(outcome.readyCalls == 0 && outcome.errorCalls == 0,
        "silent server produced a callback before stop()");
    QElapsedTimer elapsed;
    elapsed.start();
    session.stop();
    require(elapsed.elapsed() < 2000, "stop() blocked during the handshake");
    require(waitUntil([&] { return !session.processRunning(); }, 6000),
        "wedged ssh process was not ended after stop()");
    waitUntil([] { return false; }, 200);
    require(outcome.readyCalls == 0 && outcome.errorCalls == 0
            && outcome.lostCalls == 0,
        "callbacks fired after stop() during the handshake");
}

// ssh failing before the server runs reports through the error callback with
// ssh's stderr attached.
void connectFailure(const std::string& serverBinary)
{
    qputenv("AMREXPLORER_FAKE_SSH_MODE", "fail");
    amrvis::qt::SshRemoteSession session;
    Outcome outcome;
    startSession(session, serverBinary, outcome);
    require(waitUntil([&] { return outcome.errorCalls > 0; }, 10000),
        "ssh failure was not reported");
    require(outcome.readyCalls == 0 && outcome.errorCalls == 1
            && outcome.error.contains(QStringLiteral("Connection refused")),
        "error report did not carry ssh's stderr");
    require(!session.processRunning(), "failed ssh process still running");
}

// The remote command names an executable that does not exist: the login
// shell reports it on stderr and exits, before any ready line.
void missingServer()
{
    qputenv("AMREXPLORER_FAKE_SSH_MODE", "");
    amrvis::qt::SshRemoteSession session;
    Outcome outcome;
    startSession(session, "/nonexistent/amrexplorer-server", outcome);
    require(waitUntil([&] { return outcome.errorCalls > 0; }, 10000),
        "missing server was not reported");
    require(outcome.readyCalls == 0
            && outcome.error.contains(QStringLiteral("installed in its PATH")),
        "missing-server report lacked the PATH hint");
}

#endif

} // namespace

int main(int argc, char* argv[])
{
    unitTests();
#ifndef _WIN32
    if (argc < 4) {
        std::cerr << "usage: test_ssh_remote_session DATASET SERVER FAKE_SSH\n";
        return 2;
    }
    QCoreApplication application(argc, argv);
    qputenv("AMREXPLORER_SSH", argv[3]);
    const std::string datasetPath = argv[1];
    const std::string serverBinary = argv[2];
    sessionRoundTrip(serverBinary, datasetPath, "");
    sessionRoundTrip(serverBinary, datasetPath, "noise");
    stopDuringHandshake(serverBinary);
    connectFailure(serverBinary);
    missingServer();
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
#endif
    return 0;
}
