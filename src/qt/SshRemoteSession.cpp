#include "SshRemoteSession.hpp"

#include <amrexplorer/remote/Frame.hpp>

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QProcess>
#include <QSocketNotifier>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace amrvis::qt {

namespace {

// Worker threads the launched server gets. Modest on purpose: the typical
// destination is a shared login node, and the server's own default is the
// machine's hardware concurrency.
constexpr int launchedServerThreads = 8;

// Bounds ssh start-up including any interactive authentication; fetching an
// MFA code from a phone must fit comfortably.
constexpr int startupTimeoutMilliseconds = 300000;

// Everything before the ready line is discarded; a preamble larger than this
// is not a login banner but a stream that will never carry the line.
constexpr std::size_t maximumPreambleBytes = 64U * 1024U;

struct HandshakeOutcome {
    std::shared_ptr<remote::Connection> connection;
    QString error;
};

#ifndef _WIN32
// Only POSIX paths report raw errno values; on Windows MSVC deprecates
// strerror outright and nothing here needs it.
QString describeReadError(int error)
{
    return QString::fromLocal8Bit(std::strerror(error));
}
#endif

} // namespace

std::optional<SshServerReadyLine> parseSshServerReadyLine(std::string_view line)
{
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    constexpr std::string_view marker = "AMREXPLORER-STDIO ";
    constexpr std::string_view separator = " TOKEN ";
    const auto markerAt = line.find(marker);
    if (markerAt == std::string_view::npos) {
        return std::nullopt;
    }
    line.remove_prefix(markerAt + marker.size());
    const auto separatorAt = line.find(separator);
    if (separatorAt == std::string_view::npos) {
        return std::nullopt;
    }
    const auto versionText = line.substr(0, separatorAt);
    const auto token = line.substr(separatorAt + separator.size());
    int version = 0;
    const auto [end, error] = std::from_chars(
        versionText.data(), versionText.data() + versionText.size(), version);
    if (error != std::errc{} || end != versionText.data() + versionText.size()
        || version != sshStdioReadyVersion || token.empty()
        || token.find_first_of(" \t\r\n") != std::string_view::npos) {
        return std::nullopt;
    }
    return SshServerReadyLine{version, std::string(token)};
}

std::optional<std::string> sshRemoteServerCommand(std::string_view executable)
{
    if (executable.empty()
        || executable.find_first_of("\r\n") != std::string_view::npos
        || executable.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    std::string command = "exec ";
    if (executable == "~") {
        command += R"("$HOME")";
    } else {
        if (executable.starts_with("~/")) {
            // A quoted '~' does not expand in a POSIX shell. Spell this common
            // remote-home form through the trusted HOME variable, then quote the
            // user-provided suffix normally.
            command += R"("$HOME"/)";
            executable.remove_prefix(2);
        }
        command += '\'';
        for (const char character : executable) {
            if (character == '\'') {
                command += "'\\''";
            } else {
                command += character;
            }
        }
        command += '\'';
    }
    command += " --stdio --threads ";
    command += std::to_string(launchedServerThreads);
    return command;
}

std::optional<QStringList> sshRemoteProcessArguments(
    std::string_view destination, std::string_view executable)
{
    if (destination.empty() || destination.front() == '-'
        || destination.find_first_of(" \t\r\n") != std::string_view::npos) {
        return std::nullopt;
    }
    const auto remoteCommand = sshRemoteServerCommand(executable);
    if (!remoteCommand) {
        return std::nullopt;
    }
    // -T: no pty, so the channel carries bytes verbatim. No forwardings, even
    // from the user's config: none is needed and a failing one would end the
    // session. Keepalives so a dead link is noticed in under a minute rather
    // than whenever TCP gives up.
    return QStringList{QStringLiteral("-T"), QStringLiteral("-o"),
        QStringLiteral("ClearAllForwardings=yes"), QStringLiteral("-o"),
        QStringLiteral("ServerAliveInterval=15"), QStringLiteral("-o"),
        QStringLiteral("ServerAliveCountMax=3"), QStringLiteral("-o"),
        QStringLiteral("ConnectTimeout=30"), QStringLiteral("--"),
        QString::fromUtf8(
            destination.data(), static_cast<qsizetype>(destination.size())),
        QString::fromStdString(*remoteCommand)};
}

QProcessEnvironment sshAskpassEnvironment(const QString& applicationExecutable)
{
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SSH_ASKPASS"), applicationExecutable);
    environment.insert(
        QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("force"));
    environment.insert(
        QStringLiteral("AMREXPLORER_SSH_ASKPASS_MODE"), QStringLiteral("1"));
    return environment;
}

QString sshProgram()
{
    const auto override = qEnvironmentVariable("AMREXPLORER_SSH");
    return override.isEmpty() ? QStringLiteral("ssh") : override;
}

SshRemoteSession::SshRemoteSession(QObject* parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_startupTimer(new QTimer(this))
    , m_terminateTimer(new QTimer(this))
    , m_streamEndTimer(new QTimer(this))
{
    m_startupTimer->setSingleShot(true);
    connect(m_startupTimer, &QTimer::timeout, this, [this] {
        drainProcessErrors(true);
        fail(tr("Timed out while starting the remote AMReXplorer session.%1")
                .arg(errorSuffix()));
    });
    m_terminateTimer->setSingleShot(true);
    connect(m_terminateTimer, &QTimer::timeout, this, [this] {
        if (m_process->state() != QProcess::NotRunning) {
            m_process->terminate();
        }
    });
    m_streamEndTimer->setSingleShot(true);
    connect(m_streamEndTimer, &QTimer::timeout, this, [this] {
        drainProcessErrors(true);
        fail(tr("The remote server ended the stream before it was ready.%1")
                .arg(errorSuffix()));
    });
    connect(m_process, &QProcess::readyReadStandardError, this,
        [this] { drainProcessErrors(false); });
    connect(m_process, &QProcess::errorOccurred, this,
        [this](QProcess::ProcessError error) {
            if (!m_stopping && error == QProcess::FailedToStart) {
                fail(tr("Could not start %1: %2")
                        .arg(m_process->program(), m_process->errorString()));
            }
        });
    connect(m_process, &QProcess::started, this, [this] {
        if (m_stopping) {
            m_process->terminate();
        }
    });
    connect(m_process,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int, QProcess::ExitStatus) {
            drainProcessErrors(true);
            processEnded(tr("The ssh process exited.%1").arg(errorSuffix()));
        });
}

SshRemoteSession::~SshRemoteSession()
{
    stop();
    // The connection is closed, so the server exits on end of stream and ssh
    // follows within a round trip; the wait covers that. Anything slower is
    // ended here rather than left running.
    if (m_process->state() != QProcess::NotRunning
        && !m_process->waitForFinished(1000)) {
        m_process->terminate();
        if (!m_process->waitForFinished(500)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
}

void SshRemoteSession::start(std::string destination,
    std::string serverExecutable, remote::ConnectionOptions options,
    ReadyHandler ready, ErrorHandler error, LostHandler lost)
{
    m_destination = std::move(destination);
    m_serverExecutable = serverExecutable;
    m_options = std::move(options);
    m_readyHandler = std::move(ready);
    m_errorHandler = std::move(error);
    m_lostHandler = std::move(lost);
    m_stopping = false;
    m_ready = false;
    m_preamble.clear();
    m_errors.clear();
    m_errorPending.clear();
    m_connection.reset();
    m_handshakeStop = StopSource{};
    m_terminateTimer->stop();
    m_streamEndTimer->stop();
    m_startupTimer->start(startupTimeoutMilliseconds);

#ifdef _WIN32
    fail(tr("SSH remote sessions are not supported on Windows yet."));
#else
    const auto sshArguments
        = sshRemoteProcessArguments(m_destination, serverExecutable);
    if (!sshArguments) {
        fail(tr("The SSH destination or remote server executable path is "
                "invalid."));
        return;
    }

    // One socket pair: our end stays here, the other becomes the child's
    // stdin and stdout, so ssh's stdio *is* the wire and no port or pipe
    // buffer sits in between. Both ends are close-on-exec; dup2 in the child
    // clears the flag on 0 and 1 only.
    int pair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        fail(tr("Could not create the ssh channel: %1")
                .arg(describeReadError(errno)));
        return;
    }
    for (const int descriptor : pair) {
        ::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    }
    ::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK);
    m_wire = pair[0];
    const int childEnd = pair[1];

    // QProcess's own stdio pipes are not used; the modifier runs in the child
    // after QProcess has set the standard descriptors up and before exec, and
    // puts our socket in their place.
    m_process->setStandardInputFile(QProcess::nullDevice());
    m_process->setStandardOutputFile(QProcess::nullDevice());
    m_process->setChildProcessModifier([childEnd] {
        ::dup2(childEnd, STDIN_FILENO);
        ::dup2(childEnd, STDOUT_FILENO);
    });
    auto environment
        = sshAskpassEnvironment(QCoreApplication::applicationFilePath());
    environment.insert(QStringLiteral("AMREXPLORER_SSH_DESTINATION"),
        QString::fromStdString(m_destination));
    m_process->setProcessEnvironment(environment);
    // QProcess passes each item as an argument without a local shell. OpenSSH
    // does invoke the remote login shell, so the remote command single-quotes
    // the user-provided executable path as one shell word.
    m_process->start(sshProgram(), *sshArguments);
    // start() has forked by the time it returns; the child holds its own copy.
    ::close(childEnd);
    if (m_stopping) {
        return;
    }
    m_wireNotifier = new QSocketNotifier(m_wire, QSocketNotifier::Read, this);
    connect(m_wireNotifier, &QSocketNotifier::activated, this,
        [this] { readPreamble(); });
#endif
}

void SshRemoteSession::readPreamble()
{
#ifndef _WIN32
    char buffer[4096];
    for (;;) {
        const auto count = ::read(m_wire, buffer, sizeof(buffer));
        if (count < 0) {
            const auto error = errno;
            if (error == EINTR) {
                continue;
            }
            if (error == EAGAIN || error == EWOULDBLOCK) {
                return;
            }
            fail(tr("Could not read from ssh: %1")
                    .arg(describeReadError(error)));
            return;
        }
        if (count == 0) {
            // The stream ended before the server was ready. Let the process
            // exit report it -- that path has the whole of stderr -- and only
            // fall back to a report of our own if ssh lingers.
            closeWire();
            m_streamEndTimer->start(1500);
            return;
        }
        m_preamble.append(buffer, static_cast<std::size_t>(count));
        break;
    }
    for (auto newline = m_preamble.find('\n'); newline != std::string::npos;
         newline = m_preamble.find('\n')) {
        const auto line = m_preamble.substr(0, newline);
        m_preamble.erase(0, newline + 1);
        if (const auto ready = parseSshServerReadyLine(line)) {
            if (!m_preamble.empty()) {
                // The server sends nothing after the ready line until it has
                // our hello, so bytes here are from something else sharing
                // the stream; framing them would be guesswork.
                fail(tr("Unexpected output after the remote server's ready "
                        "line."));
                return;
            }
            delete m_wireNotifier;
            m_wireNotifier = nullptr;
            beginHandshake(std::exchange(m_wire, -1), ready->token);
            return;
        }
    }
    if (m_preamble.size() > maximumPreambleBytes) {
        drainProcessErrors(true);
        fail(tr("The remote server produced no valid ready line.%1")
                .arg(errorSuffix()));
    }
#endif
}

void SshRemoteSession::beginHandshake(int wire, std::string token)
{
    auto* watcher = new QFutureWatcher<HandshakeOutcome>(this);
    connect(watcher, &QFutureWatcher<HandshakeOutcome>::finished, this,
        [this, watcher] {
            auto outcome = watcher->result();
            watcher->deleteLater();
            if (m_stopping) {
                if (outcome.connection) {
                    outcome.connection->close();
                }
                return;
            }
            if (!outcome.connection) {
                drainProcessErrors(true);
                fail(tr("The remote server did not complete the protocol "
                        "handshake: %1%2")
                        .arg(outcome.error, errorSuffix()));
                return;
            }
            m_ready = true;
            m_startupTimer->stop();
            m_connection = std::move(outcome.connection);
            if (m_readyHandler) {
                m_readyHandler(m_connection);
            }
        });
    auto options = m_options;
    options.sessionToken = std::move(token);
    watcher->setFuture(QtConcurrent::run(
        [wire, options = std::move(options),
            cancellation = m_handshakeStop.get_token()]() -> HandshakeOutcome {
            try {
                auto socket = std::make_unique<remote::Socket>(
                    remote::adoptStreamSocket(wire));
                return {std::make_shared<remote::Connection>(
                            std::move(socket), options, cancellation),
                    {}};
            } catch (const std::exception& error) {
                return {nullptr, QString::fromUtf8(error.what())};
            }
        }));
}

void SshRemoteSession::drainProcessErrors(bool flush)
{
    // Always drain stderr so a chatty ssh or server cannot deadlock on a full
    // pipe. The tail is kept for diagnostics; the server prints no secrets on
    // stderr in stdio mode.
    m_errorPending
        += QString::fromLocal8Bit(m_process->readAllStandardError());
    // A newline-free flood must not grow the pending fragment without bound;
    // like the kept tail, only its last part is worth anything.
    constexpr qsizetype maximumErrorCharacters = 8192;
    if (m_errorPending.size() > maximumErrorCharacters) {
        m_errorPending = m_errorPending.right(maximumErrorCharacters);
    }
    const auto appendLine = [this](const QString& line) {
        if (!line.trimmed().isEmpty()) {
            m_errors += line.trimmed() + QLatin1Char('\n');
        }
    };
    for (auto newline = m_errorPending.indexOf(QLatin1Char('\n'));
         newline >= 0; newline = m_errorPending.indexOf(QLatin1Char('\n'))) {
        appendLine(m_errorPending.left(newline));
        m_errorPending.remove(0, newline + 1);
    }
    if (flush && !m_errorPending.isEmpty()) {
        appendLine(m_errorPending);
        m_errorPending.clear();
    }
    if (m_errors.size() > maximumErrorCharacters) {
        m_errors = m_errors.right(maximumErrorCharacters);
    }
}

QString SshRemoteSession::errorSuffix() const
{
    // Trimmed for display only: the stored lines keep their separators, so
    // a line drained after a flush does not run into the previous one.
    const auto errors = m_errors.trimmed();
    return errors.isEmpty() ? QString() : QStringLiteral("\n") + errors;
}

void SshRemoteSession::closeWire()
{
    delete m_wireNotifier;
    m_wireNotifier = nullptr;
#ifndef _WIN32
    if (m_wire >= 0) {
        ::close(m_wire);
        m_wire = -1;
    }
#endif
}

void SshRemoteSession::stop()
{
    if (m_stopping) {
        return;
    }
    m_stopping = true;
    m_startupTimer->stop();
    m_handshakeStop.request_stop();
    closeWire();
    if (m_connection) {
        // End of stream is the shutdown signal: the server exits when it
        // reads it, and ssh exits when the server does.
        m_connection->close();
    }
    m_streamEndTimer->stop();
    if (m_process->state() != QProcess::NotRunning) {
        m_terminateTimer->start(2000);
    }
}

const std::string& SshRemoteSession::destination() const noexcept
{
    return m_destination;
}

const std::string& SshRemoteSession::serverExecutable() const noexcept
{
    return m_serverExecutable;
}

std::shared_ptr<remote::Connection> SshRemoteSession::connection() const
{
    return m_connection;
}

bool SshRemoteSession::ready() const noexcept
{
    return m_ready;
}

bool SshRemoteSession::processRunning() const
{
    return m_process->state() != QProcess::NotRunning;
}

std::optional<int> SshRemoteSession::processExitCode() const
{
    if (m_process->state() != QProcess::NotRunning
        || m_process->exitStatus() != QProcess::NormalExit) {
        return std::nullopt;
    }
    return m_process->exitCode();
}

void SshRemoteSession::fail(const QString& message)
{
    if (m_stopping) {
        return;
    }
    auto handler = m_errorHandler;
    stop();
    if (handler) {
        handler(message);
    }
}

void SshRemoteSession::processEnded(const QString& description)
{
    if (m_stopping) {
        return;
    }
    if (!m_ready) {
        auto hint = tr(" Make sure the SSH destination is reachable and "
                       "amrexplorer-server is installed in its PATH.");
        if (m_errors.contains(QStringLiteral("unknown option: --stdio"))) {
            hint = tr(" The amrexplorer-server on the destination is too old "
                      "for this client; install a current one.");
        }
        fail(description + hint);
        return;
    }
    auto handler = m_lostHandler;
    stop();
    if (handler) {
        handler(description);
    }
}

} // namespace amrvis::qt
