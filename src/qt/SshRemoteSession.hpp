#pragma once

#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/remote/Connection.hpp>

#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class QProcess;
class QSocketNotifier;
class QTimer;

namespace amrvis::qt {

// The line amrexplorer-server --stdio writes first. Everything before it on the
// stream is login-shell chatter and is discarded; everything after it is the
// wire protocol.
inline constexpr int sshStdioReadyVersion = 1;

struct SshServerReadyLine {
    int version = 0;
    std::string token;
};

// Recognises the ready line anywhere in `line` (a prompt printed without a
// trailing newline may precede it), for the supported version only.
[[nodiscard]] std::optional<SshServerReadyLine> parseSshServerReadyLine(
    std::string_view line);

// The remote command: `exec '<executable>' --stdio --threads N`, quoted as
// one word for the remote login shell. A leading `~/` is spelled through
// "$HOME" so it still expands. Returns nullopt for an unusable path.
[[nodiscard]] std::optional<std::string> sshRemoteServerCommand(
    std::string_view executable);

// The full ssh argument vector, or nullopt when the destination could be read
// as an option or the executable is unusable.
[[nodiscard]] std::optional<QStringList> sshRemoteProcessArguments(
    std::string_view destination, std::string_view executable);

// The environment that routes OpenSSH's prompts (password, MFA, host key)
// through this application's askpass mode.
[[nodiscard]] QProcessEnvironment sshAskpassEnvironment(
    const QString& applicationExecutable);

// The ssh program to run: $AMREXPLORER_SSH when set (tests substitute a script
// that runs the server locally), otherwise "ssh" from PATH.
[[nodiscard]] QString sshProgram();

// One ssh process running `amrexplorer-server --stdio` on the destination, with
// the wire protocol carried over the process's stdin/stdout. Nothing listens
// anywhere: the server reads and writes the ssh channel, and it exits when
// the stream ends, so it cannot outlive this session. Prompts go through the
// askpass environment. Callbacks run on this object's thread; the ready
// callback receives the connection with its handshake complete.
class SshRemoteSession final : public QObject {
public:
    using ReadyHandler
        = std::function<void(std::shared_ptr<remote::Connection>)>;
    using ErrorHandler = std::function<void(QString)>;
    using LostHandler = std::function<void(QString)>;

    explicit SshRemoteSession(QObject* parent = nullptr);
    ~SshRemoteSession() override;

    SshRemoteSession(const SshRemoteSession&) = delete;
    SshRemoteSession& operator=(const SshRemoteSession&) = delete;

    // options.sessionToken is filled in from the ready line.
    void start(std::string destination, std::string serverExecutable,
        remote::ConnectionOptions options, ReadyHandler ready,
        ErrorHandler error, LostHandler lost);
    // Closes the connection (the server then exits on end of stream) and
    // ends the ssh process. Idempotent; no callback fires after it.
    void stop();

    [[nodiscard]] const std::string& destination() const noexcept;
    [[nodiscard]] std::shared_ptr<remote::Connection> connection() const;
    [[nodiscard]] bool ready() const noexcept;
    // The ssh process: whether it is still running, and once it is not, its
    // exit code when it exited normally (nullopt when it was killed).
    [[nodiscard]] bool processRunning() const;
    [[nodiscard]] std::optional<int> processExitCode() const;

private:
    void readPreamble();
    void beginHandshake(int wire, std::string token);
    void drainProcessErrors(bool flush);
    [[nodiscard]] QString errorSuffix() const;
    void closeWire();
    void fail(const QString& message);
    void processEnded(const QString& description);

    QProcess* m_process;
    QTimer* m_startupTimer;
    QSocketNotifier* m_wireNotifier = nullptr;
    // Our end of the socket pair, until the handshake takes it over.
    int m_wire = -1;
    std::string m_destination;
    std::string m_preamble;
    QString m_errors;
    QString m_errorPending;
    remote::ConnectionOptions m_options;
    StopSource m_handshakeStop;
    std::shared_ptr<remote::Connection> m_connection;
    bool m_ready = false;
    bool m_stopping = false;
    ReadyHandler m_readyHandler;
    ErrorHandler m_errorHandler;
    LostHandler m_lostHandler;
};

} // namespace amrvis::qt
