#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QSettings;
class QWidget;

namespace amrvis::remote {
class Connection;
}

namespace amrvis::qt {

class SshRemoteSession;

// The window's remote session: the ssh-launched server (SshRemoteSession),
// the one connection every remote open goes through, and the ways a user
// reaches it -- the Open Remote dialogs, the remote directory browser, the
// per-destination settings (last destination, server executable, last
// browsed directory). It says what to open through openRequested(); the host
// owns the datasets and opens them over connection(). Status-bar text and
// background errors go out as signals; the host decides where they show.
class RemoteSessionController final : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        // True once the host began closing; late session callbacks then do
        // nothing (the host is tearing the session down itself).
        std::function<bool()> isShuttingDown;
        // The server-side path of the dataset the host currently shows, when
        // it came over the remote connection; for the diagnostics lines.
        std::function<std::optional<std::string>()> currentRemotePath;
        // The application's settings store, opened per use.
        std::function<std::unique_ptr<QSettings>()> settings;
    };

    // `softwareVersion` is what the client reports in the protocol handshake.
    RemoteSessionController(
        Hooks hooks, std::string softwareVersion, QObject* parent = nullptr);
    ~RemoteSessionController() override;

    // Installs the connection every remote open goes through, with its
    // handshake already complete: the SSH session's once it is ready, or a
    // loopback one from a test harness. Replaces any previous connection;
    // datasets open on that one fail on their next request.
    void install(std::shared_ptr<remote::Connection> connection, QString label);
    [[nodiscard]] std::shared_ptr<remote::Connection> connection() const;
    // True while an installed connection is live.
    [[nodiscard]] bool connected() const;
    // Bumped by every install(); a sequence's frame loader captures it so a
    // frame from a replaced connection can be told apart.
    [[nodiscard]] std::uint64_t connectionGeneration() const noexcept
    {
        return m_connectionGeneration;
    }

    // Runs amrexplorer-server on the named OpenSSH destination with the wire
    // protocol over ssh's stdio, installs the connection once its handshake
    // completes, and asks the host to open the supplied server-visible paths
    // (openRequested). An empty path list only establishes the session.
    // `onReady` runs after that. An empty executable means the one
    // remembered for the destination, else "amrexplorer-server".
    void start(std::string destination, std::string serverExecutable,
        std::vector<std::string> remotePaths,
        std::function<void()> onReady = {});
    // The Open Remote Plotfile / Sequence dialog, modal on `parent`. Reuses
    // the live session when the connection fields are unchanged; otherwise
    // starts a new one and opens the paths once it is ready. Browse... goes
    // through browse() the same way.
    void promptOpen(QWidget* parent, bool sequence);
    // The remote directory browser over the live connection, modal on
    // `parent`; what it picks goes out through openRequested(). Starts at the
    // directory last browsed on this destination, else the server's home.
    void browse(QWidget* parent, bool sequence);
    // The server executable to use for a destination: the one last used for
    // it, else "amrexplorer-server". Per destination only: an explicit path
    // is a property of one machine.
    [[nodiscard]] QString serverExecutableFor(const QString& destination) const;
    // The Diagnostics panel's remote lines, or empty when there is no session.
    [[nodiscard]] QString diagnosticsLines() const;
    // Ends the ssh session; the host's close path.
    void shutdown();

signals:
    // The connection was installed or replaced, or the session started or
    // ended: the diagnostics line changed.
    void sessionChanged();
    // The host should open these server-visible paths over connection(): as
    // a sequence when `sequence`, else the (single) path as a dataset.
    void openRequested(std::vector<std::string> paths, bool sequence);
    void statusMessage(const QString& message, int timeoutMs);
    void errorReported(const QString& message);

private:
    [[nodiscard]] QString sessionDestination() const;
    // Whether the live session is the one these fields describe.
    [[nodiscard]] bool sessionMatches(
        const QString& destination, const QString& executable) const;
    void rememberDestination(
        const QString& destination, const QString& executable);

    Hooks m_hooks;
    std::string m_softwareVersion;
    std::unique_ptr<SshRemoteSession> m_session;
    std::shared_ptr<remote::Connection> m_connection;
    QString m_label;
    std::uint64_t m_connectionGeneration = 0;
};

} // namespace amrvis::qt
