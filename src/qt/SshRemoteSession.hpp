#pragma once

#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class QProcess;
class QTcpSocket;
class QTimer;

namespace amrvis::qt {

struct SshServerReadyLine {
    std::uint16_t port = 0;
    std::string token;
};

// Parses the one machine-readable line emitted by amrexplorer-server. Kept
// public so the security-sensitive handoff can be unit tested without running
// SSH or exposing a real bearer token.
[[nodiscard]] std::optional<SshServerReadyLine> parseSshServerReadyLine(std::string_view line);

// Builds the constant `exec` command sent to the remote Linux login shell,
// quoting the executable path as one shell word.
[[nodiscard]] std::optional<std::string>
sshRemoteServerCommand(std::string_view executable, std::uint16_t port);

// Builds one OpenSSH invocation that both starts the remote server and forwards
// a local loopback port to the same port on the SSH destination.
[[nodiscard]] std::optional<QStringList> sshRemoteProcessArguments(
    std::string_view destination, std::string_view executable, std::uint16_t port);

// Forces OpenSSH to invoke this application as its graphical askpass helper.
// The application recognises the private mode variable before creating its
// normal main window and returns one keyboard-interactive response on stdout.
[[nodiscard]] QProcessEnvironment sshAskpassEnvironment(const QString& applicationExecutable);

// True when ssh's collected stderr shows the local forwarding listener could
// not bind, i.e. the reserved ephemeral port was lost to a concurrent process
// before ssh claimed it. Only this failure is retried on a fresh port; any
// other ssh exit is reported to the user unchanged.
[[nodiscard]] bool sshLocalForwardBindFailed(const QString& errors);

// Owns the single SSH process that runs the loopback-only server and carries a
// loopback-only local forward to it. The bearer token is retained only in
// memory and never put in the process's arguments.
class SshRemoteSession final : public QObject {
  public:
    struct Endpoint {
        std::string host;
        std::uint16_t port = 0;
        std::string token;
    };

    using ReadyHandler = std::function<void(Endpoint)>;
    using ErrorHandler = std::function<void(QString)>;
    using LostHandler = std::function<void(QString)>;

    explicit SshRemoteSession(QObject* parent = nullptr);
    ~SshRemoteSession() override;

    SshRemoteSession(const SshRemoteSession&) = delete;
    SshRemoteSession& operator=(const SshRemoteSession&) = delete;

    void start(std::string destination, std::string serverExecutable, ReadyHandler ready,
               ErrorHandler error, LostHandler lost);
    void stop();

  private:
    void readServerOutput();
    void drainProcessErrors(QProcess* process, QString& buffer, QString& pending, bool redactTokens,
                            bool flush = false);
    void startSshProcess();
    void probeTunnel();
    void fail(const QString& message);
    void processEnded(const QString& description);

    QProcess* m_server = nullptr;
    QTcpSocket* m_probe = nullptr;
    QTimer* m_startupTimer = nullptr;
    std::string m_destination;
    std::string m_serverExecutable;
    std::string m_serverOutput;
    QString m_serverErrors;
    QString m_serverErrorPending;
    std::uint16_t m_localPort = 0;
    std::string m_token;
    bool m_ready = false;
    bool m_stopping = false;
    int m_forwardRetries = 0;
    ReadyHandler m_readyHandler;
    ErrorHandler m_errorHandler;
    LostHandler m_lostHandler;
};

} // namespace amrvis::qt
