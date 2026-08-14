#include "SshRemoteSession.hpp"

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QHostAddress>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <charconv>
#include <string_view>
#include <utility>

namespace amrvis::qt {

std::optional<SshServerReadyLine> parseSshServerReadyLine(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    constexpr std::string_view prefix = "LISTENING 127.0.0.1 ";
    constexpr std::string_view separator = " TOKEN ";
    if (!line.starts_with(prefix)) {
        return std::nullopt;
    }
    line.remove_prefix(prefix.size());
    const auto separatorAt = line.find(separator);
    if (separatorAt == std::string_view::npos) {
        return std::nullopt;
    }
    const auto portText = line.substr(0, separatorAt);
    const auto token = line.substr(separatorAt + separator.size());
    unsigned int port = 0;
    const auto [end, error] =
        std::from_chars(portText.data(), portText.data() + portText.size(), port);
    if (error != std::errc{} || end != portText.data() + portText.size() || port == 0 ||
        port > 65535 || token.empty() || token.find_first_of(" \t\r\n") != std::string_view::npos) {
        return std::nullopt;
    }
    return SshServerReadyLine{static_cast<std::uint16_t>(port), std::string(token)};
}

std::optional<std::string> sshRemoteServerCommand(std::string_view executable, std::uint16_t port) {
    if (executable.empty() || executable.find_first_of("\r\n") != std::string_view::npos ||
        executable.find('\0') != std::string_view::npos || port == 0) {
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
    command += " --port ";
    command += std::to_string(port);
    return command;
}

std::optional<QStringList> sshRemoteProcessArguments(std::string_view destination,
                                                     std::string_view executable,
                                                     std::uint16_t port) {
    if (destination.empty() || destination.front() == '-' ||
        destination.find_first_of(" \t\r\n") != std::string_view::npos) {
        return std::nullopt;
    }
    const auto remoteCommand = sshRemoteServerCommand(executable, port);
    if (!remoteCommand) {
        return std::nullopt;
    }
    const auto forwarding = QStringLiteral("127.0.0.1:%1:127.0.0.1:%1").arg(port);
    return QStringList{QStringLiteral("-T"), QStringLiteral("-o"),
                       QStringLiteral("ExitOnForwardFailure=yes"), QStringLiteral("-L"), forwarding,
                       QStringLiteral("--"), QString::fromUtf8(destination.data(),
                                                               static_cast<qsizetype>(destination.size())),
                       QString::fromStdString(*remoteCommand)};
}

QProcessEnvironment sshAskpassEnvironment(const QString& applicationExecutable) {
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SSH_ASKPASS"), applicationExecutable);
    environment.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("force"));
    environment.insert(QStringLiteral("AMREXPLORER_SSH_ASKPASS_MODE"), QStringLiteral("1"));
    return environment;
}

SshRemoteSession::SshRemoteSession(QObject* parent)
    : QObject(parent), m_server(new QProcess(this)), m_probe(new QTcpSocket(this)),
      m_startupTimer(new QTimer(this)) {
    m_startupTimer->setSingleShot(true);
    connect(m_startupTimer, &QTimer::timeout, this, [this] {
        drainProcessErrors(m_server, m_serverErrors, m_serverErrorPending, true, true);
        fail(tr("Timed out while starting the remote AMReXplorer session.%1")
                 .arg(m_serverErrors.isEmpty() ? QString()
                                               : QStringLiteral("\n") + m_serverErrors));
    });
    connect(m_server, &QProcess::readyReadStandardOutput, this, [this] { readServerOutput(); });
    connect(m_server, &QProcess::readyReadStandardError, this,
            [this] { drainProcessErrors(m_server, m_serverErrors, m_serverErrorPending, true); });
    connect(m_server, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!m_stopping && error == QProcess::FailedToStart) {
            fail(tr("Could not start ssh: %1").arg(m_server->errorString()));
        }
    });
    connect(m_server, &QProcess::started, this, [this] {
        if (m_stopping) {
            m_server->terminate();
        }
    });
    connect(m_server, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
                drainProcessErrors(m_server, m_serverErrors, m_serverErrorPending, true, true);
                processEnded(tr("The SSH server process exited.%1")
                                 .arg(m_serverErrors.isEmpty()
                                          ? QString()
                                          : QStringLiteral("\n") + m_serverErrors));
            });
    connect(m_probe, &QTcpSocket::connected, this, [this] {
        m_probe->abort();
        if (m_stopping || m_ready) {
            return;
        }
        m_ready = true;
        m_startupTimer->stop();
        if (m_readyHandler) {
            m_readyHandler(Endpoint{"127.0.0.1", m_localPort, std::move(m_token)});
        }
    });
    connect(m_probe, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (!m_stopping && !m_ready) {
            QTimer::singleShot(100, this, [this] { probeTunnel(); });
        }
    });
}

SshRemoteSession::~SshRemoteSession() {
    stop();
    if (m_server->state() != QProcess::NotRunning && !m_server->waitForFinished(1000)) {
        m_server->kill();
        m_server->waitForFinished(1000);
    }
}

void SshRemoteSession::start(std::string destination, std::string serverExecutable,
                             ReadyHandler ready, ErrorHandler error, LostHandler lost) {
    m_destination = std::move(destination);
    m_readyHandler = std::move(ready);
    m_errorHandler = std::move(error);
    m_lostHandler = std::move(lost);
    m_stopping = false;
    m_ready = false;
    m_serverOutput.clear();
    m_serverErrors.clear();
    m_serverErrorPending.clear();
    m_token.clear();
    m_localPort = 0;
    m_startupTimer->start(60000);

    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, 0)) {
        fail(tr("Could not allocate a local port for the SSH tunnel: %1")
                 .arg(reservation.errorString()));
        return;
    }
    m_localPort = reservation.serverPort();
    reservation.close();

    const auto sshArguments =
        sshRemoteProcessArguments(m_destination, serverExecutable, m_localPort);
    if (!sshArguments) {
        fail(tr("The SSH destination or remote server executable path is invalid."));
        return;
    }
    auto environment = sshAskpassEnvironment(QCoreApplication::applicationFilePath());
    environment.insert(QStringLiteral("AMREXPLORER_SSH_DESTINATION"),
                       QString::fromStdString(m_destination));
    m_server->setProcessEnvironment(environment);
    // QProcess passes each item as an argument without a local shell. OpenSSH
    // does invoke the remote login shell, so remoteCommand single-quotes the
    // user-provided executable path as one shell word.
    m_server->start(QStringLiteral("ssh"), *sshArguments);
}

void SshRemoteSession::drainProcessErrors(QProcess* process, QString& buffer, QString& pending,
                                          bool redactTokens, bool flush) {
    // Always drain stderr so a chatty SSH/server process cannot deadlock on a
    // full pipe. Never retain lines containing TOKEN: the server deliberately
    // repeats its bearer token on stderr for manual use.
    pending += QString::fromLocal8Bit(process->readAllStandardError());
    auto appendLine = [&](const QString& line) {
        if ((!redactTokens || !line.contains(QStringLiteral("token"), Qt::CaseInsensitive)) &&
            !line.trimmed().isEmpty()) {
            buffer += line.trimmed() + QLatin1Char('\n');
        }
    };
    for (auto newline = pending.indexOf(QLatin1Char('\n')); newline >= 0;
         newline = pending.indexOf(QLatin1Char('\n'))) {
        appendLine(pending.left(newline));
        pending.remove(0, newline + 1);
    }
    if (flush && !pending.isEmpty()) {
        appendLine(pending);
        pending.clear();
    }
    constexpr qsizetype maximumErrorCharacters = 8192;
    if (buffer.size() > maximumErrorCharacters) {
        buffer = buffer.right(maximumErrorCharacters);
    }
    if (flush) {
        buffer = buffer.trimmed();
    }
}

void SshRemoteSession::stop() {
    if (m_stopping) {
        return;
    }
    m_stopping = true;
    m_startupTimer->stop();
    m_probe->abort();
    // The server command and local forward share this process and SSH
    // connection, so terminating it tears down both together.
    if (m_server->state() != QProcess::NotRunning) {
        m_server->terminate();
    }
}

void SshRemoteSession::readServerOutput() {
    m_serverOutput += m_server->readAllStandardOutput().toStdString();
    for (auto newline = m_serverOutput.find('\n'); newline != std::string::npos;
         newline = m_serverOutput.find('\n')) {
        auto line = m_serverOutput.substr(0, newline);
        m_serverOutput.erase(0, newline + 1);
        if (const auto ready = parseSshServerReadyLine(line)) {
            if (ready->port != m_localPort) {
                fail(tr("The remote server listened on an unexpected port."));
                return;
            }
            m_token = ready->token;
            probeTunnel();
            return;
        }
    }
    if (m_serverOutput.size() > 16384) {
        fail(tr("The remote server produced no valid startup line."));
    }
}

void SshRemoteSession::probeTunnel() {
    if (m_stopping || m_ready) {
        return;
    }
    m_probe->abort();
    m_probe->connectToHost(QHostAddress::LocalHost, m_localPort);
}

void SshRemoteSession::fail(const QString& message) {
    if (m_stopping) {
        return;
    }
    auto handler = m_errorHandler;
    stop();
    if (handler) {
        handler(message);
    }
}

void SshRemoteSession::processEnded(const QString& description) {
    if (m_stopping) {
        return;
    }
    if (!m_ready) {
        fail(description + tr(" Make sure the SSH destination is reachable and "
                              "amrexplorer-server is installed in its PATH."));
        return;
    }
    auto handler = m_lostHandler;
    stop();
    if (handler) {
        handler(description);
    }
}

} // namespace amrvis::qt
