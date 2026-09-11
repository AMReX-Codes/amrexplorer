#include "RemoteSessionController.hpp"

#include "RemoteFileDialog.hpp"
#include "RemoteOpenDialog.hpp"
#include "SshRemoteSession.hpp"

#include <amrexplorer/remote/Connection.hpp>

#include <QDialog>
#include <QMessageBox>
#include <QSettings>

#include <utility>

namespace amrvis::qt {

RemoteSessionController::RemoteSessionController(
    Hooks hooks, std::string softwareVersion, QObject* parent)
    : QObject(parent)
    , m_hooks(std::move(hooks))
    , m_softwareVersion(std::move(softwareVersion))
{
}

RemoteSessionController::~RemoteSessionController() = default;

void RemoteSessionController::install(
    std::shared_ptr<remote::Connection> connection, QString label)
{
    m_connection = std::move(connection);
    m_label = std::move(label);
    // A property of the connection, so it is derived once here and stands for
    // the session's life. A status message cannot carry it: the open that
    // follows the ready line replaces it within the same event-loop turn.
    m_precisionNotice
        = m_connection && !m_connection->supportsDoublePrecisionValues()
        ? QString::fromLatin1(remote::doublePrecisionValuesUnsupportedMessage)
        : QString();
    ++m_connectionGeneration;
    emit sessionChanged();
}

std::shared_ptr<remote::Connection> RemoteSessionController::connection() const
{
    return m_connection;
}

bool RemoteSessionController::connected() const
{
    return m_connection && m_connection->connected();
}

QString RemoteSessionController::sessionDestination() const
{
    return m_session ? QString::fromStdString(m_session->destination())
                     : QString();
}

bool RemoteSessionController::sessionMatches(
    const QString& destination, const QString& executable) const
{
    return connected() && m_session
        && destination.toStdString() == m_session->destination()
        && executable.toStdString() == m_session->serverExecutable();
}

QString RemoteSessionController::serverExecutableFor(
    const QString& destination) const
{
    // An explicit executable path is a property of one machine, so nothing
    // stored for another destination is consulted: a destination without its
    // own entry gets the name every install puts on the remote PATH. (An
    // earlier revision fell back to the executable last used anywhere, which
    // let one machine's path break the destinations that worked by default.)
    const auto settings = m_hooks.settings();
    const auto forDestination
        = settings
              ->value(QStringLiteral("remote/serverExecutables/%1")
                      .arg(destination))
              .toString()
              .trimmed();
    return forDestination.isEmpty() ? QStringLiteral("amrexplorer-server")
                                    : forDestination;
}

void RemoteSessionController::rememberDestination(
    const QString& destination, const QString& executable)
{
    // Remember this destination and its executable; the CLI teaches the
    // dialog this way too. Per destination only -- see serverExecutableFor.
    const auto settings = m_hooks.settings();
    settings->setValue(QStringLiteral("remote/sshDestination"), destination);
    settings->setValue(
        QStringLiteral("remote/serverExecutables/%1").arg(destination),
        executable);
}

void RemoteSessionController::start(std::string destination,
    std::string serverExecutable, std::vector<std::string> remotePaths,
    std::function<void()> onReady)
{
    if (destination.empty() || destination.front() == '-'
        || destination.find_first_of(" \t\r\n") != std::string::npos) {
        emit errorReported(tr("Invalid SSH destination."));
        return;
    }
    const auto destinationText = QString::fromStdString(destination);
    if (serverExecutable.empty()) {
        serverExecutable = serverExecutableFor(destinationText).toStdString();
    }
    rememberDestination(
        destinationText, QString::fromStdString(serverExecutable));
    // A previous session's connection is closed with it; a dataset still open
    // on it fails on its next request, and the new server gets fresh opens.
    m_session.reset();
    m_connection.reset();
    m_label.clear();
    m_precisionNotice.clear();
    m_session = std::make_unique<SshRemoteSession>(this);
    emit statusMessage(
        tr("Starting remote session on %1...").arg(destinationText), 0);
    m_session->start(destination, std::move(serverExecutable),
        remote::ConnectionOptions{.clientName = "AMReXplorer Qt",
            .softwareVersion = m_softwareVersion, .sessionToken = {}},
        [this, destination, paths = std::move(remotePaths),
            onReady = std::move(onReady)](
            std::shared_ptr<remote::Connection> connection) {
            if (m_hooks.isShuttingDown && m_hooks.isShuttingDown()) {
                return;
            }
            const auto& server = connection->serverInfo();
            install(std::move(connection),
                tr("ssh %1").arg(QString::fromStdString(destination)));
            // One multi-arg call: chaining would rescan the inserted text, so
            // a peer whose reported name held a marker could steer the rest.
            emit statusMessage(
                tr("Remote session on %1 is ready (%2 %3, %4 worker threads)")
                    .arg(QString::fromStdString(destination),
                        QString::fromStdString(server.serverName),
                        QString::fromStdString(server.softwareVersion),
                        QString::number(server.workerCount)),
                0);
            if (!paths.empty()) {
                emit openRequested(paths, paths.size() > 1);
            }
            if (onReady) {
                onReady();
            }
        },
        [this, destination](const QString& message) {
            if (m_hooks.isShuttingDown && m_hooks.isShuttingDown()) {
                return;
            }
            emit statusMessage(tr("Could not start the remote session"), 0);
            emit errorReported(
                tr("Could not start the remote session on %1: %2")
                    .arg(QString::fromStdString(destination), message));
            emit sessionChanged();
        },
        [this, destination](const QString& message) {
            if (m_hooks.isShuttingDown && m_hooks.isShuttingDown()) {
                return;
            }
            emit statusMessage(tr("Remote session ended"), 0);
            emit errorReported(tr("The remote session on %1 ended: %2")
                    .arg(QString::fromStdString(destination), message));
            emit sessionChanged();
        });
    // After start(): the session reports its destination only from then on.
    emit sessionChanged();
}

void RemoteSessionController::promptOpen(QWidget* parent, bool sequence)
{
    RemoteOpenDialog dialog(sequence, sessionDestination(),
        m_hooks.settings()
            ->value(QStringLiteral("remote/sshDestination"))
            .toString(),
        [this](const QString& destination) {
            return serverExecutableFor(destination);
        },
        parent);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto destination = dialog.destination();
    const auto executable = dialog.executable();
    const bool matches = sessionMatches(destination, executable);
    if (dialog.browseRequested()) {
        if (destination.isEmpty() || executable.isEmpty()) {
            QMessageBox::warning(parent, dialog.windowTitle(),
                tr("The SSH destination and the server executable are "
                   "required."));
            return;
        }
        if (matches) {
            browse(parent, sequence);
        } else {
            start(destination.toStdString(), executable.toStdString(), {},
                [this, parent, sequence] { browse(parent, sequence); });
        }
        return;
    }
    auto paths = dialog.paths();
    if (destination.isEmpty() || executable.isEmpty() || paths.empty()) {
        QMessageBox::warning(parent, dialog.windowTitle(),
            tr("The SSH destination, the server executable, and at least one "
               "plotfile path are required."));
        return;
    }
    // Checked here for both routes: over a new session the ready handler
    // would otherwise open a lone path as a single dataset, silently
    // dropping the sequence the dialog was for.
    if (sequence && paths.size() < 2) {
        QMessageBox::warning(parent, dialog.windowTitle(),
            tr("Enter two or more plotfile paths, one per line, in playback "
               "order."));
        return;
    }
    // An unchanged destination and executable mean the current session is the
    // one asked for; open over it directly instead of starting ssh again.
    if (matches) {
        emit openRequested(std::move(paths), sequence);
        return;
    }
    start(destination.toStdString(), executable.toStdString(),
        std::move(paths));
}

void RemoteSessionController::promptCompanion(QWidget* parent, bool sameServer)
{
    RemoteOpenDialog dialog(RemoteOpenDialog::Kind::Companion, sessionDestination(),
        m_hooks.settings()
            ->value(QStringLiteral("remote/sshDestination"))
            .toString(),
        [this](const QString& destination) {
            return serverExecutableFor(destination);
        },
        parent);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto destination = dialog.destination();
    const auto executable = dialog.executable();
    const bool matches = sessionMatches(destination, executable);
    if (destination.isEmpty() || executable.isEmpty()) {
        QMessageBox::warning(parent, dialog.windowTitle(),
            tr("The SSH destination and the server executable are required."));
        return;
    }
    // A new session would replace the connection the open plotfile came
    // over, and its datasets with it.
    if (sameServer && !matches) {
        QMessageBox::warning(parent, dialog.windowTitle(),
            tr("A plotfile on show came over the session on %1; a companion "
               "must come from the same server, so keep that destination and "
               "server executable (or close the companion first).")
                .arg(sessionDestination()));
        return;
    }
    if (dialog.browseRequested()) {
        const auto pick = [this, parent] {
            auto paths = runBrowser(parent, m_connection, /*sequence=*/false);
            if (!paths.empty()) {
                emit companionRequested(std::move(paths.front()));
            }
        };
        if (matches) {
            pick();
        } else {
            start(destination.toStdString(), executable.toStdString(), {}, pick);
        }
        return;
    }
    auto paths = dialog.paths();
    if (paths.empty()) {
        QMessageBox::warning(parent, dialog.windowTitle(),
            tr("The plotfile path is required."));
        return;
    }
    if (matches) {
        emit companionRequested(std::move(paths.front()));
        return;
    }
    start(destination.toStdString(), executable.toStdString(), {},
        [this, path = std::move(paths.front())] { emit companionRequested(path); });
}

void RemoteSessionController::browse(QWidget* parent, bool sequence)
{
    if (!connected()) {
        emit errorReported(tr("Open a remote session first "
                              "(File > Open Remote Plotfile...)."));
        return;
    }
    auto paths = runBrowser(parent, m_connection, sequence);
    if (paths.empty()) {
        return;
    }
    // One plotfile picked in the sequence browser is just that plotfile.
    const bool asSequence = paths.size() > 1;
    emit openRequested(std::move(paths), asSequence);
}

std::string RemoteSessionController::chooseRemotePlotfile(QWidget* parent,
    const std::shared_ptr<remote::Connection>& connection)
{
    if (!connection || !connection->connected()) {
        emit errorReported(tr("The remote session that opened the plotfile "
                              "has ended."));
        return {};
    }
    auto paths = runBrowser(parent, connection, /*sequence=*/false);
    return paths.empty() ? std::string{} : std::move(paths.front());
}

std::vector<std::string> RemoteSessionController::runBrowser(QWidget* parent,
    const std::shared_ptr<remote::Connection>& connection, bool sequence)
{
    // The last directory browsed is remembered per destination: a path is a
    // property of one machine, like the server executable.
    const auto destination
        = m_session ? sessionDestination() : m_label;
    const auto settingsKey
        = QStringLiteral("remote/lastDirectories/%1").arg(destination);
    RemoteFileDialog dialog(connection,
        m_hooks.settings()->value(settingsKey).toString(),
        sequence ? RemoteFileDialog::SelectionMode::PlotfileSequence
                 : RemoteFileDialog::SelectionMode::SinglePlotfile,
        parent);
    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }
    auto paths = dialog.selectedPaths();
    if (!paths.empty() && !dialog.currentDirectory().isEmpty()) {
        m_hooks.settings()->setValue(settingsKey, dialog.currentDirectory());
    }
    return paths;
}

QString RemoteSessionController::diagnosticsLines() const
{
    QString text;
    if (m_connection) {
        text += tr("\nremote session: %1 (%2)")
                    .arg(m_label,
                        m_connection->connected() ? tr("connected")
                                                  : tr("disconnected"));
        const auto remotePath = m_hooks.currentRemotePath
            ? m_hooks.currentRemotePath()
            : std::nullopt;
        if (remotePath) {
            text += tr("\nremote path: %1")
                        .arg(QString::fromStdString(*remotePath));
        }
        const auto precisionNotice = valuePrecisionNotice();
        if (!precisionNotice.isEmpty()) {
            text += tr("\nremote values: float -- %1").arg(precisionNotice);
        }
    } else if (m_session) {
        text += tr("\nremote session: ssh %1 (starting)")
                    .arg(sessionDestination());
    }
    return text;
}

void RemoteSessionController::shutdown()
{
    m_session.reset();
}

} // namespace amrvis::qt
