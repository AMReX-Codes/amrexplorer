#include "MainWindow.hpp"
#include "FabSelectorDock.hpp"
#include "RemoteEndpoint.hpp"
#include "SshConnectArguments.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QSignalBlocker>
#include <QRunnable>
#include <QTableView>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>

#include <amrexplorer/render2d/Contours.hpp>
#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace {

QtMessageHandler g_previousMessageHandler = nullptr;

void printUsage(std::FILE* output)
{
    std::fprintf(output,
        "usage: amrexplorer [PLOTFILE...]\n"
        "       amrexplorer --ssh SSH_DESTINATION [--server PATH] "
        "[REMOTE_PLOTFILE...]\n\n"
        "Open one plotfile directory, or several to play them as a\n"
        "sequence, or none for an empty window.\n\n"
        "  --ssh SSH_DESTINATION  run amrexplorer-server on the destination\n"
        "                         through ssh and open the remote plotfile\n"
        "                         paths there; with no paths, only establish\n"
        "                         the session\n"
        "  --server PATH          remote amrexplorer-server executable, for\n"
        "                         when it is not on the non-interactive\n"
        "                         remote PATH; remembered per destination\n"
        "  -h, --help             show this help\n\n"
        "See docs/user-guide.md for details.\n");
}

#ifdef AMREXPLORER_QT_TEST_ACCESS
// The smoke tests speak to an in-process loopback server; the handshake
// against it takes milliseconds, so it runs right here on the GUI thread.
void attachSmokeServer(amrvis::qt::MainWindow& window,
    const std::shared_ptr<amrvis::remote::Server>& server)
{
    window.useRemoteConnection(
        std::make_shared<amrvis::remote::Connection>("127.0.0.1",
            server->port(),
            amrvis::remote::ConnectionOptions{
                .clientName = "AMReXplorer Qt smoke",
                .sessionToken = server->token()}),
        QStringLiteral("127.0.0.1:%1").arg(server->port()));
}
#endif

bool writeAskpassResponse(const QByteArray& response)
{
    if (std::fwrite(response.constData(), 1,
            static_cast<std::size_t>(response.size()), stdout)
            != static_cast<std::size_t>(response.size())
        || std::fputc('\n', stdout) == EOF) {
        return false;
    }
    std::fflush(stdout);
    return true;
}

// OpenSSH runs $SSH_ASKPASS with the prompt as its argument and reads one line
// from its stdout. SSH_ASKPASS_PROMPT says what kind of prompt it is: unset
// for a secret (password, passphrase, MFA code), "confirm" for a yes/no
// question such as accepting a new host key, "none" for a notification that
// expects no answer (a hardware-key touch); ssh ends the notification process
// itself once the wait is over.
int runSshAskpass(const QString& prompt)
{
    if (prompt.isEmpty()) {
        return 1;
    }
    const auto destination = qEnvironmentVariable("AMREXPLORER_SSH_DESTINATION");
    const auto title = destination.isEmpty()
        ? QStringLiteral("SSH authentication")
        : QStringLiteral("SSH authentication — %1").arg(destination);
    const auto kind = qEnvironmentVariable("SSH_ASKPASS_PROMPT");
    if (kind == QLatin1String("confirm")) {
        QMessageBox box(QMessageBox::Question, title, prompt,
            QMessageBox::Yes | QMessageBox::No);
        box.setTextFormat(Qt::PlainText);
        box.setWindowFlag(Qt::WindowStaysOnTopHint);
        const auto answer = box.exec();
        return writeAskpassResponse(
                   answer == QMessageBox::Yes ? "yes" : "no")
            ? 0 : 1;
    }
    if (kind == QLatin1String("none")) {
        QMessageBox box(QMessageBox::Information, title, prompt,
            QMessageBox::Ok);
        box.setTextFormat(Qt::PlainText);
        box.setWindowFlag(Qt::WindowStaysOnTopHint);
        box.exec();
        return 0;
    }
    QInputDialog dialog;
    dialog.setWindowTitle(title);
    dialog.setLabelText(prompt);
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setTextEchoMode(QLineEdit::Password);
    dialog.setOkButtonText(QStringLiteral("Respond"));
    dialog.setCancelButtonText(QStringLiteral("Cancel"));
    dialog.setWindowFlag(Qt::WindowStaysOnTopHint);
    if (auto* label = dialog.findChild<QLabel*>()) {
        label->setTextFormat(Qt::PlainText);
    }
    if (dialog.exec() != QDialog::Accepted) {
        return 1;
    }
    return writeAskpassResponse(dialog.textValue().toUtf8()) ? 0 : 1;
}

// Qt 6 on Wayland logs a benign xdg-shell warning whenever a new grabbing popup
// -- a menu, submenu, combo-box dropdown, or tooltip -- opens while another
// popup is still grabbing, which happens during ordinary menu-bar and submenu
// navigation. QtWayland already copes by reparenting the new popup to the
// topmost grabbing one, so the "setGrabPopup ... does not match the current
// topmost grabbing popup" line is pure noise. Drop just that message (matched
// narrowly on category + text) and forward everything else -- including all
// other qt.qpa.wayland diagnostics -- to the previous handler unchanged.
void filterWaylandPopupWarning(QtMsgType type,
    const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg && context.category != nullptr
        && std::strcmp(context.category, "qt.qpa.wayland") == 0
        && message.contains(QLatin1String("topmost grabbing popup"))) {
        return;
    }
    if (g_previousMessageHandler != nullptr) {
        g_previousMessageHandler(type, context, message);
        return;
    }
    // No prior handler: mirror Qt's default output (stderr, abort on fatal).
    std::fprintf(stderr, "%s\n",
        qFormatLogMessage(type, context, message).toLocal8Bit().constData());
    std::fflush(stderr);
    if (type == QtFatalMsg) {
        std::abort();
    }
}

// "Copy and run" support for Linux docks. GNOME/KDE docks can only show an app
// icon when a .desktop entry and a themed icon exist on this machine -- a
// binary copied to another box has neither. So on startup we install them from
// the bundled (qrc) icons, with Exec pointing at this running binary's path,
// which makes the dock work wherever the executable is copied. Idempotent: it
// only writes when the entry is missing or the binary moved. User-local
// (~/.local/share); delete ~/.local/share/applications/amrexplorer.desktop and the
// amrexplorer.png files under ~/.local/share/icons/hicolor to undo. The standalone
// resources/install-desktop-entry.sh does the same thing by hand.
// Escapes a path for the inside of a quoted Desktop Entry Exec argument. The
// spec reserves backslash, double quote, backtick and dollar there, each
// escaped with a backslash; a path containing any of them previously produced
// an Exec line that would not parse back to the same path.
//
// Guarded on the same condition as its only caller below. ensureDesktopEntry
// compiles to an early `return` off Linux, so the call is preprocessed away and
// an unguarded definition here is an unused static function -- which -Werror
// turns into a build failure on macOS and Windows, where nothing else in this
// file would have noticed.
#ifdef Q_OS_LINUX
[[nodiscard]] QString desktopExecEscaped(const QString& path)
{
    // Two layers, applied in the order the reader undoes them. The Exec value
    // is first unescaped as a desktop-entry string, and only then parsed as an
    // Exec command line, so the backslashes the quoting rule needs must
    // themselves survive the string rule -- which is why the spec says a
    // literal backslash inside a quoted argument takes four of them.
    QString escaped;
    escaped.reserve(path.size());
    for (const auto character : path) {
        // Exec quoting: reserved inside double quotes.
        if (character == QLatin1Char('\\') || character == QLatin1Char('"')
            || character == QLatin1Char('`') || character == QLatin1Char('$')) {
            escaped.append(QLatin1Char('\\'));
            escaped.append(character);
            continue;
        }
        // Field codes: a literal percent is written as two. Without this a
        // path containing, say, "%q" is read as an unknown field code and
        // desktop-file-validate rejects the entry.
        if (character == QLatin1Char('%')) {
            escaped.append(QLatin1String("%%"));
            continue;
        }
        escaped.append(character);
    }
    // Desktop-entry string escaping, over the result of the above so the
    // quoting layer's own backslashes are doubled with the path's.
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    return escaped;
}
#endif

void ensureDesktopEntry()
{
#ifndef Q_OS_LINUX
    // Desktop entry + hicolor icons are a GNOME/KDE (Linux) mechanism. On
    // other platforms the writes land in nonsensical locations and the
    // cache-refresh helpers do not exist, so do nothing.
    return;
#else
    static constexpr int kSizes[] = {16, 32, 64, 128, 256};
    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (dataDir.isEmpty()) {
        return;
    }
    const QString desktopPath = dataDir + "/applications/amrexplorer.desktop";
    const QString execPath = QCoreApplication::applicationFilePath();

    const auto iconInstalled = [&]() {
        for (int size : kSizes) {
            const QString path = QDir(
                dataDir + QString("/icons/hicolor/%1x%1/apps").arg(size))
                .filePath("amrexplorer.png");
            if (!QFileInfo::exists(path)) {
                return false;
            }
        }
        return true;
    };
    const auto desktopCurrent = [&]() {
        QFile file(desktopPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return false;
        }
        return file.readAll().contains("Exec=" + execPath.toUtf8());
    };
    if (iconInstalled() && desktopCurrent()) {
        return;
    }

    for (int size : kSizes) {
        const QString dir = dataDir + QString("/icons/hicolor/%1x%1/apps").arg(size);
        const QString path = QDir(dir).filePath("amrexplorer.png");
        if (QFileInfo::exists(path)) {
            // Already installed: skip the no-op rewrite. Conscious trade-off
            // -- this also means a changed bundled icon won't reach an existing
            // install; delete the file to force a refresh.
            continue;
        }
        QDir().mkpath(dir);
        QFile in(QStringLiteral(":/amrexplorer-%1.png").arg(size));
        QFile out(path);
        if (in.open(QIODevice::ReadOnly)
            && out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(in.readAll());
        }
    }
    QDir().mkpath(dataDir + "/applications");
    // Rewritten wholesale when the binary moved (the Exec line must track the
    // running binary), which discards user edits to the other fields. That is
    // intentional for this install-on-startup helper; a surgical Exec-only
    // patch would preserve edits but is out of scope.
    QFile desktop(desktopPath);
    if (desktop.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QTextStream out(&desktop);
        out << "[Desktop Entry]\n"
            << "Type=Application\n"
            << "Name=AMReXplorer\n"
            << "GenericName=AMR Visualization\n"
            << "Comment=Demand-driven AMR visualization\n"
            << "Exec=\"" << desktopExecEscaped(execPath) << "\" %F\n"
            << "Icon=amrexplorer\n"
            << "StartupWMClass=amrexplorer\n"
            << "Terminal=false\n"
            << "Categories=Science;DataVisualization;\n";
    }
    // Best-effort cache refresh. gtk-update-icon-cache warns ("No theme index
    // file") unless the theme dir has an index.theme, so copy the system
    // hicolor one into the user tree if it is missing.
    const QString hicolorDir = dataDir + "/icons/hicolor";
    const QString indexTheme = hicolorDir + "/index.theme";
    if (!QFileInfo::exists(indexTheme)) {
        for (const QString& source : {
                 QStringLiteral("/usr/share/icons/hicolor/index.theme"),
                 QStringLiteral("/usr/local/share/icons/hicolor/index.theme")}) {
            if (QFile::copy(source, indexTheme)) {
                break;
            }
        }
    }
    // Best-effort cache refresh; failures are harmless. Arguments go through
    // QProcess as a list rather than being pasted into a shell command: a
    // single quote anywhere in $HOME used to break the quoting and run
    // something else entirely. Output is discarded by redirecting the detached
    // process's channels, not by a shell, since these otherwise print "Cache
    // file created successfully." on every install.
    const auto runSilent = [](const QString& program,
                               const QStringList& arguments) {
        QProcess process;
        process.setProgram(program);
        process.setArguments(arguments);
        process.setStandardOutputFile(QProcess::nullDevice());
        process.setStandardErrorFile(QProcess::nullDevice());
        process.startDetached();
    };
    runSilent(QStringLiteral("gtk-update-icon-cache"),
        QStringList{QStringLiteral("-f"), hicolorDir});
    runSilent(QStringLiteral("update-desktop-database"),
        QStringList{dataDir + QStringLiteral("/applications")});
#endif
}

// Everything from here to the end of the namespace serves the smoke-test
// branches in main() and nothing else, so it is gated with them: without this
// a release build compiles a few hundred lines it cannot reach, and -Werror
// reports every one of them as unused.
#ifdef AMREXPLORER_QT_TEST_ACCESS

bool rangeSelectorMatches(
    const amrvis::qt::MainWindow& window, bool metadataRangesAvailable)
{
    const auto* selector = window.findChild<QComboBox*>(
        QStringLiteral("rangeModeSelector"));
    if (selector == nullptr) {
        return false;
    }
    const auto fileIndex = selector->findData(
        static_cast<int>(amrvis::qt::RangeMode::File));
    const auto levelIndex = selector->findData(
        static_cast<int>(amrvis::qt::RangeMode::Level));
    if (fileIndex < 0 || levelIndex < 0) {
        return false;
    }
    const auto fileEnabled = selector->model()->flags(
        selector->model()->index(fileIndex, 0)) & Qt::ItemIsEnabled;
    const auto levelEnabled = selector->model()->flags(
        selector->model()->index(levelIndex, 0)) & Qt::ItemIsEnabled;
    const auto expectedMode = metadataRangesAvailable
        ? amrvis::qt::RangeMode::File : amrvis::qt::RangeMode::Visible;
    return selector->currentData().toInt() == static_cast<int>(expectedMode)
        && static_cast<bool>(fileEnabled) == metadataRangesAvailable
        && static_cast<bool>(levelEnabled) == metadataRangesAvailable;
}

bool fabRangeSelectorMatches(const amrvis::qt::MainWindow& window)
{
    const auto* selector = window.findChild<QComboBox*>(
        QStringLiteral("rangeModeSelector"));
    if (selector == nullptr) {
        return false;
    }
    const auto fileIndex = selector->findData(
        static_cast<int>(amrvis::qt::RangeMode::File));
    const auto levelIndex = selector->findData(
        static_cast<int>(amrvis::qt::RangeMode::Level));
    if (fileIndex < 0 || levelIndex < 0) {
        return false;
    }
    const auto fileEnabled = selector->model()->flags(
        selector->model()->index(fileIndex, 0)) & Qt::ItemIsEnabled;
    const auto levelEnabled = selector->model()->flags(
        selector->model()->index(levelIndex, 0)) & Qt::ItemIsEnabled;
    return selector->currentData().toInt()
            == static_cast<int>(amrvis::qt::RangeMode::File)
        && static_cast<bool>(fileEnabled)
        && !static_cast<bool>(levelEnabled);
}

bool fabSelectorIsAscending(const amrvis::qt::FabSelectorDock& selector)
{
    const auto* table = selector.findChild<QTableView*>(
        QStringLiteral("fabSelectorTable"));
    if (table == nullptr || table->model() == nullptr) {
        return false;
    }
    qulonglong previous = 0;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        const auto grid = table->model()->index(row, 0).data().toULongLong();
        if (row != 0 && grid < previous) {
            return false;
        }
        previous = grid;
    }
    return true;
}

bool fabSelectorColumnsMatch(
    const amrvis::qt::FabSelectorDock& selector, bool viewingMultiFab)
{
    const auto* table = selector.findChild<QTableView*>(
        QStringLiteral("fabSelectorTable"));
    if (table == nullptr || table->model() == nullptr
        || table->model()->columnCount() != 7) {
        return false;
    }
    const std::array<QString, 7> expected{
        QStringLiteral("Grid"),
        QStringLiteral("Valid box"),
        QStringLiteral("FAB Box"),
        QStringLiteral("Components"),
        QStringLiteral("File"),
        QStringLiteral("Offset"),
        QStringLiteral("Precision")
    };
    for (int column = 0; column < table->model()->columnCount(); ++column) {
        if (table->model()->headerData(
                column, Qt::Horizontal, Qt::DisplayRole).toString()
            != expected[static_cast<std::size_t>(column)]) {
            return false;
        }
    }
    return table->isColumnHidden(1) != viewingMultiFab;
}

bool fabSelectorPointFilterMatches(
    amrvis::qt::FabSelectorDock& selector, bool exercisePrompt)
{
    auto* filter = selector.findChild<QLineEdit*>(
        QStringLiteral("fabSelectorFilter"));
    auto* clear = selector.findChild<QPushButton*>(
        QStringLiteral("fabSelectorClearFilter"));
    const auto* table = selector.findChild<QTableView*>(
        QStringLiteral("fabSelectorTable"));
    const auto& entries = selector.entries();
    if (filter == nullptr || clear == nullptr || table == nullptr
        || table->model() == nullptr || entries.empty()) {
        return false;
    }

    const auto dimension = entries.front().dimension;
    const auto expectedExample = dimension == 1
        ? QStringLiteral("(34)")
        : dimension == 2
            ? QStringLiteral("(34,24)")
            : QStringLiteral("(34,24,0)");
    if (!filter->isReadOnly()
        || filter->placeholderText()
            != QStringLiteral("Filter int tuple (e.g., %1)")
                .arg(expectedExample)) {
        return false;
    }
    if (!exercisePrompt) {
        return true;
    }

    const auto& first = entries.front();
    const auto& targetBox = first.storedBox;
    QString tuple = QStringLiteral("(");
    for (int axis = 0; axis < dimension; ++axis) {
        if (axis != 0) {
            tuple += QLatin1Char(',');
        }
        tuple += QString::number(
            targetBox.lower[static_cast<std::size_t>(axis)]);
    }
    tuple += QLatin1Char(')');

    int expectedRows = 0;
    for (const auto& entry : entries) {
        const auto& box = entry.storedBox;
        bool contains = true;
        for (int axis = 0; axis < dimension; ++axis) {
            const auto index = static_cast<std::size_t>(axis);
            contains = contains
                && targetBox.lower[index] >= box.lower[index]
                && targetBox.lower[index] <= box.upper[index];
        }
        expectedRows += contains ? 1 : 0;
    }

    if (expectedRows != 1) {
        return false;
    }

    bool promptOpened = false;
    QTimer::singleShot(0, &selector, [&promptOpened, tuple] {
        auto* dialog = qobject_cast<QInputDialog*>(
            QApplication::activeModalWidget());
        if (dialog != nullptr) {
            promptOpened = true;
            dialog->setTextValue(tuple);
            dialog->accept();
        }
    });
    QTimer::singleShot(100, [] {
        if (auto* dialog = QApplication::activeModalWidget()) {
            dialog->close();
        }
    });
    const QPointF localPosition(1.0, 1.0);
    QMouseEvent click(
        QEvent::MouseButtonRelease, localPosition,
        filter->mapToGlobal(localPosition.toPoint()),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(filter, &click);
    return
        promptOpened && filter->text() == tuple
        && table->model()->rowCount() == expectedRows && !clear->isHidden();
}

bool clearFabSelectorPointFilter(amrvis::qt::FabSelectorDock& selector)
{
    auto* filter = selector.findChild<QLineEdit*>(
        QStringLiteral("fabSelectorFilter"));
    auto* clear = selector.findChild<QPushButton*>(
        QStringLiteral("fabSelectorClearFilter"));
    const auto* table = selector.findChild<QTableView*>(
        QStringLiteral("fabSelectorTable"));
    if (filter == nullptr || clear == nullptr || table == nullptr
        || table->model() == nullptr || filter->text().isEmpty()
        || clear->isHidden()) {
        return false;
    }
    clear->click();
    return filter->text().isEmpty() && clear->isHidden()
        && table->model()->rowCount()
            == static_cast<int>(selector.entries().size());
}

// Verifies the contour-sync smoke scenario after the re-slice batch settles.
// The three 3-D panels were sliced at asymmetric positions, so each has its
// own local range; Visible mode reconciles them into one shared range. The fix
// requires every panel's contour levels to come from that shared range. We
// check: all three panels agree on the (positive) shared range, and every
// contour level shown in any panel is one of contourValues(shared range) --
// which fails if a panel kept its local-range levels (the bug). A non-vacuous
// guard ensures contours actually rendered.
bool contourSyncMatches(amrvis::qt::MainWindow& window)
{
    const auto probes = window.contourViewProbesForTest();
    if (probes.size() != 3) {
        return false;
    }
    const auto within = [](double a, double b) {
        const auto scale = std::max({1.0, std::fabs(a), std::fabs(b)});
        return std::fabs(a - b) <= 1.0e-6 * scale;
    };
    // Log was requested and every slice is strictly positive, so log must have
    // applied and all panels must agree on the shared, positive Visible range.
    const auto& shared = probes.front();
    for (const auto& probe : probes) {
        if (!probe.logarithmic || !(probe.displayMinimum > 0.0)
            || !(probe.displayMinimum < probe.displayMaximum)
            || !within(probe.displayMinimum, shared.displayMinimum)
            || !within(probe.displayMaximum, shared.displayMaximum)) {
            return false;
        }
    }
    std::vector<double> expected;
    try {
        expected = amrvis::contourValues(
            shared.displayMinimum, shared.displayMaximum, 3, true);
    } catch (const std::exception&) {
        return false;
    }
    // Every level shown in any panel must be one of the shared-range levels;
    // the bug leaves a panel showing its own local-range levels instead. Track
    // which shared levels are actually drawn (map each shown level to its
    // canonical expected index -- a well-defined membership, unlike dedup by an
    // intransitive tolerance) so the pass is not vacuously met by empty
    // overlays: require >= 2 shared levels drawn across >= 2 panels.
    std::vector<bool> drawn(expected.size(), false);
    std::size_t panelsWithLevels = 0;
    for (const auto& probe : probes) {
        for (const auto level : probe.contourLevels) {
            std::size_t match = expected.size();
            for (std::size_t i = 0; i < expected.size(); ++i) {
                if (within(level, expected[i])) {
                    match = i;
                    break;
                }
            }
            if (match == expected.size()) {
                return false;  // a level not derived from the shared range
            }
            drawn[match] = true;
        }
        if (!probe.contourLevels.empty()) {
            ++panelsWithLevels;
        }
    }
    const auto sharedLevelsDrawn = static_cast<std::size_t>(
        std::count(drawn.begin(), drawn.end(), true));
    return panelsWithLevels >= 2 && sharedLevelsDrawn >= 2;
}

// What one view shows, for a local-versus-remote comparison: the physical
// window the probe reports, the viewport pixels, and the raster size.
struct ViewCapture {
    QRectF window;
    QImage viewport;
    std::array<int, 2> image{};
};

void printDataWindow(std::ostream& stream, const QRectF& window)
{
    stream << std::setprecision(17) << window.x() << ',' << window.y() << ','
           << window.width() << ',' << window.height();
}

// Local and remote must agree on the physical window, but not to the last bit:
// a local fixed scale scrolls a whole-domain raster while a remote one scrolls
// a whole-domain virtual canvas of finest cells, and both scroll in whole
// viewport pixels, so their integer scroll positions may differ by a rounding
// step. Two viewport pixels of slack covers that; the coordinate-space
// confusion this guards against is off by many cells.
bool dataWindowsAgree(const QRectF& local, const QRectF& remote,
    const std::array<int, 2>& viewportPixels)
{
    const auto slack = [](double extent, int pixels) {
        return pixels > 0 ? 2.0 * std::fabs(extent) / pixels : 0.0;
    };
    const auto slackX = std::max(slack(local.width(), viewportPixels[0]),
        1.0e-9 * std::fabs(local.width()));
    const auto slackY = std::max(slack(local.height(), viewportPixels[1]),
        1.0e-9 * std::fabs(local.height()));
    return std::fabs(local.left() - remote.left()) <= slackX
        && std::fabs(local.right() - remote.right()) <= slackX
        && std::fabs(local.top() - remote.top()) <= slackY
        && std::fabs(local.bottom() - remote.bottom()) <= slackY;
}

// Rendering happens client-side for both datasets, so equal data must paint
// equal viewport pixels. Reports the first difference (raster order) and how
// many pixels differ, or nullopt when the two viewports are identical.
struct ViewportDifference {
    QString summary;
    std::size_t differingPixels = 0;
};

std::optional<ViewportDifference> viewportDifference(
    const QImage& local, const QImage& remote)
{
    if (local.size() != remote.size() || local.isNull() || remote.isNull()) {
        return ViewportDifference{
            QStringLiteral("viewport sizes differ (local %1x%2, remote %3x%4)")
                .arg(local.width())
                .arg(local.height())
                .arg(remote.width())
                .arg(remote.height()),
            0};
    }
    const auto left = local.convertToFormat(QImage::Format_ARGB32);
    const auto right = remote.convertToFormat(QImage::Format_ARGB32);
    std::optional<ViewportDifference> difference;
    std::size_t differing = 0;
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            const auto localPixel = left.pixel(x, y);
            const auto remotePixel = right.pixel(x, y);
            if (localPixel == remotePixel) {
                continue;
            }
            ++differing;
            if (!difference) {
                difference = ViewportDifference{
                    QStringLiteral("first differing pixel at (%1,%2): "
                                   "local=%3 remote=%4")
                        .arg(x)
                        .arg(y)
                        .arg(localPixel, 8, 16, QLatin1Char('0'))
                        .arg(remotePixel, 8, 16, QLatin1Char('0')),
                    0};
            }
        }
    }
    if (difference) {
        difference->differingPixels = differing;
    }
    return difference;
}

// The sequence preserve smokes assert that the *data window* survives the
// step: rubberBandZoomActiveViewForTest selects the domain's centered half,
// and after the density change the view must still show exactly that window.
// They used to assert the transform differed from a fresh fit instead, but
// that proxy only held while the rubber-band feedback disturbed the viewport
// (transient scroll bars): once the arrival is framed cleanly, preserving a
// window that the raster covers exactly *is* numerically a fit, while a
// broken preserve (refit to the whole frame) still moves the window and
// fails here.
bool centeredHalfWindowPreserved(const amrvis::qt::MainWindow& window)
{
    const auto domain = window.datasetPhysicalDomainForTest();
    const auto shown = window.activeViewVisibleDataWindowForTest();
    if (domain.isEmpty() || shown.isEmpty()) {
        return false;
    }
    const QRectF expected(
        domain.left() + 0.25 * domain.width(),
        domain.top() + 0.25 * domain.height(),
        0.5 * domain.width(), 0.5 * domain.height());
    const auto tolerance
        = 0.02 * std::max(domain.width(), domain.height());
    return std::abs(shown.left() - expected.left()) <= tolerance
        && std::abs(shown.top() - expected.top()) <= tolerance
        && std::abs(shown.right() - expected.right()) <= tolerance
        && std::abs(shown.bottom() - expected.bottom()) <= tolerance;
}

#endif // AMREXPLORER_QT_TEST_ACCESS

} // namespace

int main(int argc, char* argv[])
{
    // Silence the benign xdg-shell popup-nesting warning that Wayland prints
    // during menu/submenu/combo navigation (see filterWaylandPopupWarning).
    // Installed before QApplication so it also covers construction-time output.
    g_previousMessageHandler = qInstallMessageHandler(filterWaylandPopupWarning);

    // Disable Wayland warnings, and the spurious "Failed to register with host
    // portal" message Qt prints at startup: it reads the portal's Settings
    // interface before calling host.portal.Registry.Register, so
    // xdg-desktop-portal has already associated an app ID with the connection
    // and rejects the registration. Nothing is lost -- the portal derived the
    // app ID it needs, which is why the call failed in the first place.
    QLoggingCategory::setFilterRules(
        QStringLiteral("qt.qpa.wayland.textinput=false\n"
                       "qt.qpa.services.warning=false"));

    if (qEnvironmentVariableIsSet("AMREXPLORER_SSH_ASKPASS_MODE")) {
        if (argc < 2) {
            return 1;
        }
        QStringList promptParts;
        promptParts.reserve(argc - 1);
        for (int index = 1; index < argc; ++index) {
            promptParts.push_back(QString::fromLocal8Bit(argv[index]));
        }
        // Do not let QApplication interpret a server-controlled prompt as a
        // Qt command-line option. It needs only argv[0] in askpass mode.
        int askpassArgc = 1;
        QApplication askpassApplication(askpassArgc, argv);
        return runSshAskpass(promptParts.join(QLatin1Char(' ')));
    }
    // Answered before QApplication exists, so it works without a display.
    if (argc >= 2
        && (std::string_view(argv[1]) == "--help"
            || std::string_view(argv[1]) == "-h")) {
        printUsage(stdout);
        return 0;
    }
    QApplication application(argc, argv);
    // Undo, for numeric conversion only, what QApplication just did. Qt calls
    // setlocale(LC_ALL, "") on Unix, which hands the C locale to the
    // environment -- and the C locale is what strtod, printf("%f") and atof
    // consult. The plotfile reader parses per-block statistics with strtod, so
    // under any comma-decimal locale strtod("0.5") stopped at the '.' and *no
    // plotfile opened at all*: LC_ALL=en_DK.utf8 failed 49 of 115 tests.
    //
    // This is pinned here, once, rather than fixed at the call site, because
    // the call site is not the class of bug: the next strtod, atof or
    // printf("%f") anyone adds re-opens the same hole, and only a pin closes
    // it by construction.
    //
    // The placement is forced rather than merely chosen. Qt moves the locale
    // inside the constructor above (QCoreApplicationPrivate::initLocale), and
    // that runs once behind a static guard, so pinning earlier is simply
    // overwritten; pinning later only widens the window in which the wrong
    // locale is live. Immediately after is the earliest point that holds.
    //
    // It is not, however, "before any thread exists". No *application* window
    // or worker does, but the constructor has already started Qt's own --
    // QDBusConnection always, and with a platform theme or a non-offscreen
    // platform also the xcb/wayland event threads and glib's pango/gdbus/pool
    // threads (measured: 2 threads offscreen, 10 under wayland with the gtk3
    // theme). glibc marks setlocale MT-Unsafe, so this call is not provably
    // race-free against threads the application does not control. It is the
    // best available placement, not a safe one in the formal sense.
    //
    // The gtk3 platform theme is the one plausible defeater and it is not one:
    // gtk_init does call setlocale(LC_ALL, "") of its own, but theme creation
    // is eager inside the constructor above, so it happens before this line,
    // and GTK's call is one-shot -- opening a native dialog later cannot undo
    // the pin.
    //
    // Nothing user-visible is lost. QLocale is independent of the C locale, so
    // QLocale::system() still reports the user's real locale and separators;
    // only the C conversion functions are pinned. C++ iostreams were never
    // affected either -- they consult the C++ global locale, which stays "C"
    // -- which is why the reader's other numeric fields, and AMReX's readers,
    // never broke. See agent-notes comma-locale-breaks-every-open.
    std::setlocale(LC_NUMERIC, "C");
    // Advertise the desktop entry name and WM class as "amrexplorer" so Linux
    // docks/taskbars can match the running window to amrexplorer.desktop and
    // resolve its icon from the icon theme (setWindowIcon alone only sets the
    // title-bar icon).
    application.setApplicationName(QStringLiteral("amrexplorer"));
    application.setApplicationDisplayName(QStringLiteral("AMReXplorer"));
    QGuiApplication::setDesktopFileName(QStringLiteral("amrexplorer"));
    // Bundle the logo (rounded-square heatmap) at several sizes so it stays
    // crisp from the 16 px title bar up to the 256 px taskbar/dock.
    QIcon icon;
    icon.addFile(QStringLiteral(":/amrexplorer-16.png"));
    icon.addFile(QStringLiteral(":/amrexplorer-32.png"));
    icon.addFile(QStringLiteral(":/amrexplorer-64.png"));
    icon.addFile(QStringLiteral(":/amrexplorer-128.png"));
    icon.addFile(QStringLiteral(":/amrexplorer-256.png"));
    application.setWindowIcon(icon);
    ensureDesktopEntry();
    amrvis::qt::MainWindow window;
    window.show();
    std::shared_ptr<amrvis::remote::Server> smokeServer;
    std::optional<std::thread> smokeServerThread;
    // The option chain below carries two kinds of branch. The production
    // options -- --ssh, and the bare plotfile paths at the end -- always
    // compile. The ~55 "--...-smoke-test" branches drive the offscreen test
    // harness through MainWindow's ForTest accessors, so they compile only
    // where those accessors do, and the release binary carries neither. The
    // chain is split into two guarded runs because --ssh sits between them;
    // the trailing `else` of the first run attaches to --ssh, so removing the
    // run leaves it as the leading `if`.
#ifdef AMREXPLORER_QT_TEST_ACCESS
    if ((argc == 8 || argc == 10)
        && std::string_view(argv[1])
            == "--fixed-scale-local-remote-repro") {
        if (std::string_view(argv[2]) != "--connect"
            || std::string_view(argv[4]) != "--token-stdin"
            || std::string_view(argv[5]) != "--remote-path"
            || (argc == 10 && std::string_view(argv[7]) != "--screenshot")) {
            qCritical("usage: amrexplorer --fixed-scale-local-remote-repro "
                "--connect HOST:PORT --token-stdin --remote-path "
                "REMOTE_PLOTFILE [--screenshot OUTPUT.png] LOCAL_PLOTFILE");
            return 2;
        }
        const auto endpoint = amrvis::qt::parseRemoteEndpoint(argv[3]);
        std::string token;
        if (!endpoint || !std::getline(std::cin, token) || token.empty()) {
            qCritical("invalid endpoint or empty token");
            return 2;
        }
        if (!token.empty() && token.back() == '\r') {
            token.pop_back();
        }
        struct FixedScaleProbe {
            int phase = 0;
            bool localShowsWholeImage = false;
            std::array<int, 2> localImage{};
            std::array<int, 2> localViewport{};
            QRectF localWindow;
            QImage localViewportImage;
        };
        auto probe = std::make_shared<FixedScaleProbe>();
        const auto remotePath = std::string(argv[6]);
        const auto screenshotPath = argc == 10
            ? QString::fromLocal8Bit(argv[8]) : QString();
        const auto localPath = std::filesystem::path(argv[argc - 1]);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application,
            [&window, &application, endpoint = *endpoint,
                token = std::move(token), remotePath, screenshotPath,
                probe](bool success) {
                if (!success) {
                    application.exit(probe->phase == 0 ? 2 : 3);
                    return;
                }
                window.selectFixedScaleForTest(1);
                if (probe->phase == 0) {
                    probe->phase = 1;
                    // Measure only after the as-needed scroll bars this scale
                    // may have demanded are laid out: they shrink the viewport,
                    // and the remote phase necessarily measures on the far side
                    // of that layout pass. On a domain that fits there are no
                    // bars and this only costs a tick.
                    QTimer::singleShot(100, &window,
                        [&window, probe, endpoint, token, remotePath] {
                            probe->localShowsWholeImage
                                = window.activeViewShowsWholeImageForTest();
                            probe->localImage
                                = window.activeViewImageSizeForTest();
                            probe->localViewport
                                = window.activeViewViewportSizeForTest();
                            probe->localWindow
                                = window.activeViewVisibleDataWindowForTest();
                            probe->localViewportImage
                                = window.activeViewViewportImageForTest();
                            try {
                                window.useRemoteConnection(
                                    std::make_shared<
                                        amrvis::remote::Connection>(
                                        endpoint.host, endpoint.port,
                                        amrvis::remote::ConnectionOptions{
                                            .clientName
                                            = "AMReXplorer Qt repro",
                                            .sessionToken = token}),
                                    QStringLiteral("%1:%2")
                                        .arg(QString::fromStdString(
                                            endpoint.host))
                                        .arg(endpoint.port));
                            } catch (const std::exception& error) {
                                std::cerr << "cannot connect to "
                                          << endpoint.host << ':'
                                          << endpoint.port << ": "
                                          << error.what() << '\n';
                                QCoreApplication::exit(2);
                                return;
                            }
                            window.openRemoteDataset(remotePath);
                        });
                    return;
                }
                const auto measure = [&window, &application, probe,
                                         screenshotPath] {
                    const auto remoteShowsWholeImage
                        = window.activeViewShowsWholeImageForTest();
                    const auto remoteImage
                        = window.activeViewImageSizeForTest();
                    const auto remoteViewport
                        = window.activeViewViewportSizeForTest();
                    const auto remoteWindow
                        = window.activeViewVisibleDataWindowForTest();
                    const auto remoteViewportImage
                        = window.activeViewViewportImageForTest();
                    std::cout << "local 1x: image=" << probe->localImage[0]
                              << 'x' << probe->localImage[1]
                              << " viewport=" << probe->localViewport[0]
                              << 'x' << probe->localViewport[1]
                              << " whole-image-visible="
                              << (probe->localShowsWholeImage ? "yes" : "no")
                              << " data-window=";
                    printDataWindow(std::cout, probe->localWindow);
                    std::cout << "\nremote 1x: image=" << remoteImage[0]
                              << 'x' << remoteImage[1]
                              << " viewport=" << remoteViewport[0]
                              << 'x' << remoteViewport[1]
                              << " whole-image-visible="
                              << (remoteShowsWholeImage ? "yes" : "no")
                              << " data-window=";
                    printDataWindow(std::cout, remoteWindow);
                    std::cout << '\n';
                    // The verdict is the visible physical window plus the
                    // painted pixels -- a probe reporting the wrong window, or
                    // a raster that differs where it is visible, is exactly
                    // what this tool exists to catch. Backing-raster
                    // dimensions are printed but deliberately not compared:
                    // local keeps the whole domain while remote fetches only
                    // the visible window, so they differ by design on any
                    // domain larger than the viewport. Equal windows over
                    // equal pixels already pin the scale -- a factor the two
                    // sides disagreed on would move the window.
                    const auto viewportMatches
                        = probe->localViewport == remoteViewport;
                    const auto windowMatches = viewportMatches
                        && dataWindowsAgree(probe->localWindow, remoteWindow,
                            probe->localViewport);
                    const auto contentDifference = viewportDifference(
                        probe->localViewportImage, remoteViewportImage);
                    if (!screenshotPath.isEmpty()) {
                        constexpr int headingHeight = 36;
                        constexpr int gap = 12;
                        const auto width = probe->localViewportImage.width()
                            + remoteViewportImage.width() + gap;
                        const auto height = headingHeight + std::max(
                            probe->localViewportImage.height(),
                            remoteViewportImage.height());
                        QImage comparison(width, height,
                            QImage::Format_ARGB32_Premultiplied);
                        comparison.fill(QColor(30, 30, 30));
                        QPainter painter(&comparison);
                        painter.setPen(Qt::white);
                        painter.drawText(QRect(0, 0,
                            probe->localViewportImage.width(), headingHeight),
                            Qt::AlignCenter, QStringLiteral("Local - 1x"));
                        painter.drawText(QRect(
                            probe->localViewportImage.width() + gap, 0,
                            remoteViewportImage.width(), headingHeight),
                            Qt::AlignCenter, QStringLiteral("Remote - 1x"));
                        painter.drawImage(0, headingHeight,
                            probe->localViewportImage);
                        painter.drawImage(
                            probe->localViewportImage.width() + gap,
                            headingHeight, remoteViewportImage);
                        painter.end();
                        if (!comparison.save(screenshotPath, "PNG")) {
                            std::cerr
                                << "failed to save screenshot comparison to "
                                << screenshotPath.toStdString() << '\n';
                            application.exit(5);
                            return;
                        }
                        std::cout << "screenshot="
                                  << screenshotPath.toStdString() << '\n';
                    }
                    if (contentDifference) {
                        std::cout << "raster-content: "
                                  << contentDifference->summary.toStdString()
                                  << " (" << contentDifference->differingPixels
                                  << " differing pixels)\n";
                    } else {
                        std::cout << "raster-content: identical\n";
                    }
                    const auto matches = windowMatches && !contentDifference;
                    std::cout << (matches ? "MATCH" : "MISMATCH")
                              << ": remote 1x ";
                    if (matches) {
                        std::cout << "shows the same visible data window and "
                                     "paints the same viewport pixels as "
                                     "local 1x";
                    } else if (!viewportMatches) {
                        std::cout << "was measured against a different viewport "
                                     "than local 1x, so the two are not "
                                     "comparable";
                    } else if (!windowMatches) {
                        std::cout << "does not show the same physical data "
                                     "window as local 1x";
                    } else {
                        std::cout << "does not paint the same viewport pixels "
                                     "as local 1x";
                    }
                    std::cout << '\n';
                    application.exit(matches ? 0 : 1);
                };
                // The remote fixed scale is demand-driven: selecting it can
                // queue a native-resolution refetch of the visible window, and
                // measuring a raster that is still being replaced reports a
                // difference that is only a race. Poll instead of guessing a
                // grace period: measure once nothing is on a worker and no
                // settle arrived during the last tick, so a request that only
                // queues its successor is waited out too. A switch that needed
                // no new raster satisfies this on the first tick.
                auto* const poll = new QTimer(&window);
                poll->setInterval(100);
                auto ticks = std::make_shared<int>(0);
                auto settles = std::make_shared<int>(0);
                auto settlesLastTick = std::make_shared<int>(-1);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [settles] { ++*settles; });
                QObject::connect(poll, &QTimer::timeout, &application,
                    [&application, &window, measure, poll, ticks, settles,
                        settlesLastTick] {
                        const auto quiet = window.slicesInFlightForTest() == 0
                            && *settles == *settlesLastTick;
                        *settlesLastTick = *settles;
                        if (quiet) {
                            poll->stop();
                            measure();
                            return;
                        }
                        if (++*ticks >= 200) {
                            poll->stop();
                            std::cerr << "the remote view never stopped "
                                         "fetching; refusing to compare an "
                                         "in-flight raster\n";
                            application.exit(6);
                        }
                    });
                poll->start();
            });
        QTimer::singleShot(30000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window,
            [&window, localPath] { window.openDataset(localPath); });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--remote-fixed-scale-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        auto fixedScalePhase = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, fixedScalePhase](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, fixedScalePhase] {
                        if (!window.fixedScaleStateMatchesForTest(1)
                            || !window.activeViewUsesNativeOutputForTest()) {
                            application.exit(1);
                            return;
                        }
                        const auto phase = (*fixedScalePhase)++;
                        if (phase == 0
                            && window.activeViewIsZoomedForTest()) {
                            window.panActiveViewForTest(-8.0, 0.0);
                            return;
                        }
                        if (phase == 1
                            && window.activeViewIsZoomedForTest()) {
                            window.resize(
                                window.width() + 80, window.height() + 40);
                            return;
                        }
                        application.exit(0);
                    });
                window.selectFixedScaleForTest(1);
                if (window.activeViewUsesNativeOutputForTest()) {
                    QTimer::singleShot(0, &application,
                        [&window, &application] {
                            application.exit(
                                window.fixedScaleStateMatchesForTest(1)
                                ? 0 : 1);
                        });
                }
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--remote-canvas-wheel-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        // Regression for virtual-canvas-survives-wheel-zoom. A wheel notch over
        // a remote fixed scale leaves the whole-domain virtual canvas installed
        // while switching the transform mode to Custom, so the next slice
        // arrival with a changed density or owner reaches preservedDataWindow --
        // which reads scene units as raster pixels of the cached plane, and on
        // a canvas they are finest cells over the whole domain. The window it
        // computed was then fed to zoomToRect. The view must stay where the
        // wheel put it: still on the canvas, still showing a window inside the
        // domain, and still centred where it was zoomed about.
        auto phase = std::make_shared<int>(0);
        auto before = std::make_shared<QRectF>();
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, phase, before](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application,
                    [&window, &application, phase, before] {
                        if (*phase == 0) {
                            *phase = 1;
                            if (!window
                                    .activeViewVirtualCanvasActiveForTest()) {
                                qCritical("no virtual canvas at fixed scale");
                                application.exit(1);
                                return;
                            }
                            *before = window
                                .activeViewVisibleDataWindowForTest();
                            window.wheelActiveViewForTest(1);
                            return;
                        }
                        if (*phase != 1) {
                            return;
                        }
                        *phase = 2;
                        const auto after
                            = window.activeViewVisibleDataWindowForTest();
                        // The canvas survives the zoom by design: it is what
                        // lets the demand fetch keep working, and dropping it
                        // would strand the scroll bars mid-domain.
                        if (!window.activeViewVirtualCanvasActiveForTest()) {
                            qCritical("the wheel zoom dropped the canvas");
                            application.exit(1);
                            return;
                        }
                        // A window with no extent is what the raster-pixel
                        // reading of cell-space scene coordinates produced.
                        if (after.width() <= 0.0 || after.height() <= 0.0) {
                            qCritical("the wheel zoom left a %gx%g window",
                                after.width(), after.height());
                            application.exit(1);
                            return;
                        }
                        if (after.width() >= before->width()) {
                            qCritical("zooming in did not narrow the window");
                            application.exit(1);
                            return;
                        }
                        // Zoomed about the viewport centre, so the centre is
                        // what must not move.
                        const auto drift = std::abs(
                            after.center().x() - before->center().x());
                        if (drift > 0.05 * after.width()) {
                            qCritical("the wheel zoom moved the centre by %g",
                                drift);
                            application.exit(1);
                            return;
                        }
                        application.exit(0);
                    });
                window.selectFixedScaleForTest(32);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3 && std::string_view(argv[1])
            == "--remote-fixed-scale-flicker-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        // Regression: in a window far too small for the whole domain at 32x,
        // the demand-driven fixed scale must settle with the viewport fully
        // backed by fetched raster, stay quiet with no input (the demand used
        // to re-issue itself endlessly through the as-needed scrollbars,
        // flickering through one remote render per flip), refetch when the
        // virtual scroll bars pan to unfetched cells, and drop the
        // domain-spanning scroll bars on a rubber-band zoom, whose selection is
        // re-rendered fitted to the pane exactly as for local data. The step
        // gating below waits for quiescence, so it does not count the refetches.
        auto phase = std::make_shared<int>(0);
        // Judge each step once the demand loop has quiesced -- nothing on a
        // worker and no settle since the last tick -- rather than after a fixed
        // delay. A slow runner's extra convergence settle just costs a tick;
        // a step that needs no refetch is quiet on the next tick instead of
        // hanging; and the flicker regression (the demand endlessly re-issuing
        // itself) never quiesces, so it trips the bounded-tick guard with its
        // own exit code rather than being conflated with the watchdog. Same
        // model as the fixed-scale-parity poll above.
        auto* poll = new QTimer(&window);
        poll->setInterval(100);
        auto settles = std::make_shared<int>(0);
        auto settlesLastTick = std::make_shared<int>(-1);
        // Settle count when the current step's action was issued. The flicker
        // guard bounds settles-per-step (fetch *rounds*), not elapsed ticks: a
        // legitimate convergence is a few rounds however slow the runner, while
        // the flicker re-issues without bound. Counting ticks instead would
        // misread a starved-but-finite fetch chain as a loop.
        auto settlesAtStep = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [settles] { ++*settles; });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, poll](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                poll->start();
                window.selectFixedScaleForTest(32);
            });
        QObject::connect(poll, &QTimer::timeout, &application,
            [&window, &application, poll, phase, settles, settlesLastTick,
                settlesAtStep] {
                // Quiescence also requires no request queued behind the slice
                // debounce: a pan/zoom schedules its refetch there, so between
                // the input and the debounce firing nothing is on a worker yet
                // and a bare in-flight check would read that gap as converged.
                const auto quiet = !window.sliceRequestPendingForTest()
                    && window.slicesInFlightForTest() == 0
                    && *settles == *settlesLastTick;
                *settlesLastTick = *settles;
                if (!quiet) {
                    // Too many fetch rounds for one step is the flicker
                    // regression (the demand loop re-issuing itself endlessly);
                    // a legitimate step converges in a handful. A pure hang with
                    // no settles is left to the watchdog, not misreported here.
                    if (*settles - *settlesAtStep > 20) {
                        poll->stop();
                        std::cerr << "the fixed-scale demand loop never "
                                     "quiesced (flicker regression)\n";
                        application.exit(5);
                    }
                    return;
                }
                if (*phase == 0) {
                    // Fixed scale converged: the raster backs the whole viewport
                    // and the demand loop stays quiet, rather than re-issuing
                    // itself through the as-needed scroll bars.
                    *phase = 1;
                    if (!window.fixedScaleStateMatchesForTest(32)
                        || !window
                            .allViewsFixedScaleRasterCoversViewportForTest()) {
                        poll->stop();
                        application.exit(1);
                        return;
                    }
                    // Five cells' worth of pixels at 32x through the real
                    // Shift+left mouse event path; the newly visible cells are
                    // fetched, then the loop is quiet again.
                    *settlesAtStep = *settles;
                    window.shiftDragActiveViewForTest(-160, 0);
                    return;
                }
                if (*phase == 1) {
                    // The scrolled fixed scale keeps the fetched raster under the
                    // whole viewport, with the domain-spanning scroll bars.
                    *phase = 2;
                    if (!window.fixedScaleStateMatchesForTest(32)
                        || !window
                            .allViewsFixedScaleRasterCoversViewportForTest()
                        || !window.activeViewScrollBarsVisibleForTest()) {
                        poll->stop();
                        application.exit(1);
                        return;
                    }
                    *settlesAtStep = *settles;
                    window.rubberBandZoomActiveViewForTest();
                    return;
                }
                if (*phase == 2) {
                    // The re-rendered selection stands alone, fitted to the pane
                    // without scroll bars, as for local data.
                    poll->stop();
                    application.exit(
                        window.activeViewIsZoomedForTest()
                            && !window.activeViewScrollBarsVisibleForTest()
                            ? 0 : 1);
                }
            });
        // Backstop for a hang outside the poll (e.g. the initial load never
        // finishing); the poll's own settle-count guard catches a flicker first.
        // Stop the poll so a tick in the same pass can't overwrite exit(4).
        QTimer::singleShot(20000, &application,
            [&application, poll] {
                poll->stop();
                application.exit(4);
            });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                window.resize(420, 301);
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--local-remote-fixed-scale-window-smoke-test") {
        // The comparison the manual --fixed-scale-local-remote-repro cannot
        // make, because its fixture fits the viewport whole: at 32x in a window
        // too small for the domain, a local fixed scale scales a whole-domain
        // raster while a remote one hosts the fetched raster on a whole-domain
        // virtual canvas of finest cells. Both must report the same visible
        // physical window and paint the same viewport pixels -- scrolling is
        // what forces the two coordinate spaces apart, so an unscrolled canvas
        // proves nothing.
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        constexpr int scaleFactor = 32;
        // Drag hard against the top-left stop before measuring anything: the
        // fixed-scale transform is applied with AnchorUnderMouse, so the scroll
        // position it leaves behind is an artifact of the pointer, not of the
        // data. Both sides clamp to the same stop -- the domain origin, since
        // both scenes are the same number of view pixels across -- which leaves
        // the pan below as the only thing positioning the view.
        constexpr int anchorDrag = 512;
        constexpr int panX = -96;   // three finest cells at 32x
        constexpr int panY = -160;  // five finest cells at 32x
        enum class Await { Scale, Anchor, Pan };
        struct WindowProbe {
            bool remotePhase = false;
            Await await = Await::Scale;
            int settles = 0;
            int settlesAtLastTick = -1;
            std::array<int, 2> viewport{};
            ViewCapture localAnchored;
            ViewCapture localPanned;
        };
        auto probe = std::make_shared<WindowProbe>();
        const auto capture = [](const amrvis::qt::MainWindow& probed) {
            return ViewCapture{probed.activeViewVisibleDataWindowForTest(),
                probed.activeViewViewportImageForTest(),
                probed.activeViewImageSizeForTest()};
        };
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled, &application,
            [probe] { ++probe->settles; });
        // A remote step may or may not demand a refetch -- an anchor drag on an
        // already-anchored canvas demands none -- so rather than wait for a
        // settle that may never come, poll: a step completes once the fetched
        // raster covers the viewport and no further settle has arrived. Both
        // pans below uncover cells (the fetch keeps only one cell of slack), so
        // no step can be measured against the raster it is replacing. The timer
        // belongs to the window, which outlives every tick and owns it.
        auto* const poll = new QTimer(&window);
        poll->setInterval(100);
        QObject::connect(poll, &QTimer::timeout, &application,
            [&window, &application, probe, capture, poll] {
                const auto covered
                    = window.allViewsFixedScaleRasterCoversViewportForTest();
                const auto stable = probe->settles == probe->settlesAtLastTick;
                probe->settlesAtLastTick = probe->settles;
                if (!covered || !stable) {
                    return;
                }
                if (!window.fixedScaleStateMatchesForTest(scaleFactor)
                    || !window.activeViewScrollBarsVisibleForTest()) {
                    std::cerr << "the remote view is not in a scrolled fixed "
                                 "scale\n";
                    application.exit(1);
                    return;
                }
                // Both phases must measure the same viewport, or the two
                // visible windows are not comparable in the first place.
                if (window.activeViewViewportSizeForTest() != probe->viewport) {
                    const auto viewport
                        = window.activeViewViewportSizeForTest();
                    std::cerr << "viewport size changed between the local and "
                                 "remote phases: local=" << probe->viewport[0]
                              << 'x' << probe->viewport[1] << " remote="
                              << viewport[0] << 'x' << viewport[1] << '\n';
                    application.exit(1);
                    return;
                }
                if (probe->await == Await::Scale) {
                    probe->await = Await::Anchor;
                    window.shiftDragActiveViewForTest(anchorDrag, anchorDrag);
                    return;
                }
                const bool anchored = probe->await == Await::Anchor;
                const auto& expected = anchored
                    ? probe->localAnchored : probe->localPanned;
                const auto* label = anchored ? "anchored" : "panned";
                const auto actual = capture(window);
                if (!dataWindowsAgree(expected.window, actual.window,
                        probe->viewport)) {
                    std::cerr << label << " local/remote data window mismatch: "
                                 "local=";
                    printDataWindow(std::cerr, expected.window);
                    std::cerr << " remote=";
                    printDataWindow(std::cerr, actual.window);
                    // Raster sizes are not compared -- local holds the whole
                    // domain while remote holds only the fetched window, which
                    // is the point of the virtual canvas -- but they explain a
                    // mismatch.
                    std::cerr << " local-raster=" << expected.image[0] << 'x'
                              << expected.image[1] << " remote-raster="
                              << actual.image[0] << 'x' << actual.image[1]
                              << '\n';
                    application.exit(1);
                    return;
                }
                if (const auto difference = viewportDifference(
                        expected.viewport, actual.viewport)) {
                    std::cerr << label << " viewport content differs: "
                              << difference->summary.toStdString() << " ("
                              << difference->differingPixels << " pixels)\n";
                    application.exit(1);
                    return;
                }
                if (anchored) {
                    probe->await = Await::Pan;
                    window.shiftDragActiveViewForTest(panX, panY);
                    return;
                }
                poll->stop();
                application.exit(0);
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application,
            [&window, &application, probe, capture, poll,
                server = smokeServer,
                path = std::string(argv[2])](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                window.selectFixedScaleForTest(scaleFactor);
                if (probe->remotePhase) {
                    poll->start();
                    return;
                }
                if (!window.fixedScaleStateMatchesForTest(scaleFactor)
                    || !window.activeViewScrollBarsVisibleForTest()) {
                    std::cerr << "the local fixed scale did not overflow the "
                                 "viewport; the fixture is too small for this "
                                 "comparison\n";
                    application.exit(1);
                    return;
                }
                // Measure only after the as-needed scroll bars this scale just
                // demanded have actually been laid out: they shrink the
                // viewport, and the remote phase necessarily measures on the
                // far side of that layout pass.
                QTimer::singleShot(100, &window,
                    [&window, &application, probe, capture, path, server] {
                        probe->viewport
                            = window.activeViewViewportSizeForTest();
                        // The local raster spans the whole domain and is
                        // already loaded, so every local step is synchronous.
                        window.shiftDragActiveViewForTest(
                            anchorDrag, anchorDrag);
                        probe->localAnchored = capture(window);
                        window.shiftDragActiveViewForTest(panX, panY);
                        probe->localPanned = capture(window);
                        if (!(probe->localPanned.window.left()
                                > probe->localAnchored.window.left())
                            || !(probe->localPanned.window.top()
                                < probe->localAnchored.window.top())) {
                            std::cerr << "the local pan did not move the view "
                                         "on both axes: viewport="
                                      << probe->viewport[0] << 'x'
                                      << probe->viewport[1] << " raster="
                                      << probe->localAnchored.image[0] << 'x'
                                      << probe->localAnchored.image[1]
                                      << " anchored=";
                            printDataWindow(std::cerr,
                                probe->localAnchored.window);
                            std::cerr << " panned=";
                            printDataWindow(std::cerr,
                                probe->localPanned.window);
                            std::cerr << '\n';
                            application.exit(1);
                            return;
                        }
                        probe->remotePhase = true;
                        attachSmokeServer(window, server);
                        window.openRemoteDataset(path);
                    });
            });
        QTimer::singleShot(30000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window,
            [&window, path = std::filesystem::path(argv[2])] {
                window.resize(420, 301);
                window.openDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--remote-slice-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                application.exit(success
                        && window.activeViewUsesViewportBoundedOutputForTest()
                    ? 0 : 1);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--remote-initial-geometry-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                if (window.allViewsUseViewportBoundedOutputForTest()) {
                    application.exit(0);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application] {
                        application.exit(
                            window.allViewsUseViewportBoundedOutputForTest()
                            ? 0 : 1);
                    }, Qt::SingleShotConnection);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--remote-rubber-aspect-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                // Three settles: the wide selection, the reset back to the
                // whole domain, the tall selection. The wide and tall
                // selections err on opposite sides of the pane when the
                // arrival's framing regresses, so both must end snug — and
                // already at the first settle: the transient-scroll-bar
                // double fetch showed a mis-framed raster there before its
                // correction arrived.
                auto step = std::make_shared<int>(0);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, step] {
                        switch ((*step)++) {
                        case 0:
                            if (!window.activeViewIsZoomedForTest()
                                || !window.activeViewHasPhysicalAspectForTest(
                                    9.0 / 4.0)
                                || !window.activeViewRasterSnugForTest()) {
                                application.exit(1);
                                return;
                            }
                            window.resetZoomAllViewsForTest();
                            return;
                        case 1:
                            window.rubberBandZoomTallActiveViewForTest();
                            return;
                        default:
                            application.exit(window.activeViewIsZoomedForTest()
                                    && window.activeViewHasPhysicalAspectForTest(
                                        4.0 / 9.0)
                                    && window.activeViewRasterSnugForTest()
                                ? 0 : 1);
                        }
                    });
                window.rubberBandZoomRectangularActiveViewForTest();
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--remote-grid-boxes-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        auto boxLoads = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, boxLoads](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                window.setGridBoxesVisibleForTest(false);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, boxLoads] {
                        if (window.activeViewGridBoxCountForTest() == 0) {
                            application.exit(1);
                            return;
                        }
                        if ((*boxLoads)++ == 0) {
                            window.setGridBoxesVisibleForTest(false);
                            window.setGridBoxesVisibleForTest(true);
                            return;
                        }
                        application.exit(0);
                    });
                window.setGridBoxesVisibleForTest(true);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--remote-sequence-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        auto firstFrameDisplayed = std::make_shared<bool>(false);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application,
            [&window, &application, firstFrameDisplayed](int index) {
                if (index == 0 && !*firstFrameDisplayed) {
                    *firstFrameDisplayed = true;
                    window.stepSequence(1);
                    window.stepSequence(-1);
                    window.stepSequence(1);
                    return;
                }
                if (index == 1) {
                    application.exit(
                        window.activeViewUsesViewportBoundedOutputForTest()
                            ? 0 : 1);
                }
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteSequence({path, path});
            });
    } else
#endif
    if (argc >= 2 && std::string_view(argv[1]) == "--ssh") {
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(argc - 2));
        for (int index = 2; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }
        auto parsed = amrvis::qt::parseSshConnectArguments(arguments);
        if (!parsed.request) {
            qCritical("%s", parsed.error.c_str());
            return 2;
        }
        QTimer::singleShot(0, &window,
            [&window, request = std::move(*parsed.request)] {
                window.startSshRemoteSession(request.destination,
                    request.serverExecutable, request.paths);
            });
    }
#ifdef AMREXPLORER_QT_TEST_ACCESS
    else if (argc == 2
        && std::string_view(argv[1]) == "--palette-labels-smoke-test") {
        // The Palette menu and the palette selector are built in different
        // translation units from one label helper, and must therefore show the
        // same names. They did not: the menu used the helper's result raw while
        // the selector capitalized it by hand, so the menu read "rainbow" where
        // the selector read "Rainbow". Needs no dataset -- both lists are built
        // during construction.
        const auto menu = window.paletteMenuLabelsForTest();
        auto selector = window.paletteSelectorLabelsForTest();
        // The reversal suffix is a selector-only presentation rule: with
        // "Reverse Colormap" on, syncPaletteSelector appends "_r" while the
        // menu actions keep their original text. That divergence is real and
        // outside what this case is about, so it is normalized away rather
        // than asserted on -- the property here is that the two agree on the
        // *name*. Isolated settings make reversal off anyway; this keeps the
        // case honest on the platforms where XDG_CONFIG_HOME does not isolate
        // QSettings, and stops it claiming an invariant the product does not
        // hold. Unifying the suffix into the shared helper is follow-up work.
        for (auto& label : selector) {
            if (label.endsWith(QStringLiteral("_r"))) {
                label.chop(2);
            }
        }
        if (menu.isEmpty() || menu != selector) {
            qCritical("palette menu labels %s do not match the selector's %s",
                qPrintable(menu.join(QStringLiteral(","))),
                qPrintable(selector.join(QStringLiteral(","))));
            return 1;
        }
        // Pinned as a literal rather than derived from the same helper the
        // widgets used, which would pass whatever that helper produced.
        if (menu.front() != QStringLiteral("Rainbow")) {
            qCritical("the first palette is labelled '%s', not 'Rainbow'",
                qPrintable(menu.front()));
            return 1;
        }
        return 0;
    } else if (argc == 3 && std::string_view(argv[1]) == "--smoke-test") {
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::datasetOpenFinished,
            &application, [&application](bool success) {
                application.exit(success ? 0 : 1);
            });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path, true); });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--open-failure-smoke-test") {
        // A failed open has already torn the previous dataset down, so it must
        // leave a placeholder that says so rather than the "Loading dataset..."
        // one it replaced -- and the window must still be usable afterwards.
        // Open a bad path, check the settled state, then open a good one.
        const std::filesystem::path bad(argv[2]);
        const std::filesystem::path good(argv[3]);
        auto attemptedGood = std::make_shared<bool>(false);
        QObject::connect(&window, &amrvis::qt::MainWindow::datasetOpenFinished,
            &application, [&window, &application, good, attemptedGood](
                              bool success) {
                if (*attemptedGood) {
                    // The recovery open. Opening is not the end of it -- the
                    // slice has to arrive and clear the placeholder -- so the
                    // verdict is left to initialSliceFinished below.
                    if (!success) {
                        qCritical("the recovery open failed");
                        application.exit(1);
                    }
                    return;
                }
                if (success) {
                    qCritical("the bad path opened successfully");
                    application.exit(1);
                    return;
                }
                const auto placeholder = window.viewPlaceholderForTest();
                if (placeholder.isEmpty()
                    || placeholder.contains(QStringLiteral("Loading"))) {
                    qCritical("a failed open left the panels at '%s'",
                        qUtf8Printable(placeholder));
                    application.exit(1);
                    return;
                }
                *attemptedGood = true;
                // Rendered, not metadata-only: the placeholder is what a
                // failed open leaves behind, so only a real slice arriving
                // proves the recovery cleared it. Both legs used to skip the
                // render, which left that unproven.
                QTimer::singleShot(0, &window,
                    [&window, good] { window.openDataset(good); });
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, attemptedGood](bool success) {
                if (!*attemptedGood) {
                    // The failed open's own signal; the placeholder it leaves
                    // is checked above.
                    return;
                }
                if (!success) {
                    qCritical("the recovery open did not render");
                    application.exit(1);
                    return;
                }
                if (!window.viewPlaceholderForTest().isEmpty()) {
                    qCritical("the recovery open left a placeholder: '%s'",
                        qUtf8Printable(window.viewPlaceholderForTest()));
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, bad] { window.openDataset(bad, true); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--missing-range-smoke-test") {
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                const auto valid = success
                    && rangeSelectorMatches(window, false);
                application.exit(valid ? 0 : 1);
            });
        QTimer::singleShot(0, &window, [&window, path] {
            window.openDataset(path);
        });
    } else if (argc == 3 && std::string_view(argv[1]) == "--slice-smoke-test") {
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                const auto valid = success
                    && rangeSelectorMatches(window, true);
                application.exit(valid ? 0 : 1);
        });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--contour-sync-smoke-test") {
        // Once the initial load lands, switch to contours in Visible+log mode
        // with asymmetric per-panel slice positions (so the three panels have
        // unequal local ranges), then verify their contour levels once the
        // re-slice batch settles. See contourSyncMatches / the issue note.
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application] {
                        application.exit(contourSyncMatches(window) ? 0 : 1);
                    });
                // YZ(x)@i=3, XZ(y)@j=2, XY(z)@k=1 on the 4^3 cube q=(i+j+k)/9:
                // local ranges [3/9,1], [2/9,8/9], [1/9,7/9] -> shared [1/9,1].
                window.configureContourSyncForTest(
                    3, true, {0.875, 0.625, 0.375});
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--visible-sync-staleness-smoke-test") {
        // Regression for the visible-range sync staleness guard (all-or-nothing
        // form) and the single-flight dispatch gate. A panel re-sliced while a
        // sync runs makes the sync's union stale for it; cached-plane reuse keeps
        // the pointer identical, so the guard keys on a per-view render
        // generation, drops the whole outcome, and the rerun recomputes a
        // coherent one. Drive that exact path: gate a sync mid-flight, re-slice
        // one panel for real (rewriting its plane via showSlice), release the
        // now-stale sync and require it dropped, and -- while the self-healing
        // rerun is still gated -- require the two untouched panels still on a
        // sentinel range set beforehand (the stale union was not applied). Then
        // release the rerun. Gate coverage: a clean multi-panel setup batch must
        // drop nothing (skips == 0); per-arrival dispatch would drop one.
        // (The reuse contract itself is pinned by test_display_transitions.)
        constexpr double kSentinelMin = -98765.0;
        constexpr double kSentinelMax = -98764.0;
        const std::filesystem::path path(argv[2]);
        auto* poll = new QTimer(&window);
        poll->setInterval(5);
        auto phase = std::make_shared<int>(0);
        auto attempts = std::make_shared<int>(0);
        // Active panel's render generation just before the invalidating zoom, so
        // phase 2 can wait for the (debounced) re-slice to actually rewrite the
        // plane -- slicesInFlight is zero again right after scheduleSliceRequest.
        auto zoomGen = std::make_shared<std::uint64_t>(0);
        auto sentinelsHeld = std::make_shared<bool>(false);
        const auto finish = [&application, poll, &window](int code) {
            window.disarmVisibleSyncGateForTest();  // free any parked worker
            poll->stop();  // no stray timeout fires after we ask to exit
            application.exit(code);
        };
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application,
            [&window, &application, finish, poll, phase](bool success) {
                if (!success) {
                    finish(1);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, finish, poll, phase] {
                        if (*phase != 0) {
                            return;
                        }
                        *phase = 1;
                        // Single-flight gate coverage: the setup was a clean
                        // multi-panel batch, so one all-current sync ran and
                        // nothing was dropped. Per-arrival dispatch would have
                        // dropped the first (premature) sync.
                        if (window.visibleSyncStaleSkipsForTest() != 0) {
                            finish(7);
                            return;
                        }
                        // Sentinel the panel ranges, then gate a sync so it
                        // cannot land before we invalidate a panel below.
                        window.setViewDisplayRangesForTest(
                            kSentinelMin, kSentinelMax);
                        window.armVisibleSyncGateForTest();
                        window.requestVisibleSyncForTest();
                        poll->start();
                    });
                // Into 3-D Visible + contours; its re-slice settles into a sync.
                window.configureContourSyncForTest(3, false, {0.5, 0.5, 0.5});
            });
        QObject::connect(poll, &QTimer::timeout, &application,
            [&window, finish, phase, attempts, zoomGen, sentinelsHeld] {
                if (++*attempts > 3000) {
                    finish(3);
                    return;
                }
                if (*phase == 1) {
                    if (!window.visibleSyncWorkerWaitingForTest()) {
                        return;  // wait for the gated sync to reach the gate
                    }
                    *phase = 2;
                    // Re-slice one panel for real while the sync is parked; its
                    // plane rewrite (via showSlice) makes the parked sync's
                    // snapshot stale for that panel.
                    *zoomGen = window.activeViewRenderGenerationForTest();
                    window.zoomActiveViewForTest();
                    return;
                }
                if (*phase == 2) {
                    if (window.activeViewRenderGenerationForTest() <= *zoomGen) {
                        return;  // wait for the debounced re-slice to rewrite
                    }
                    *phase = 3;
                    window.releaseVisibleSyncGateForTest();  // let the stale sync land
                    return;
                }
                if (*phase == 3) {
                    // The stale sync must drop the whole outcome, then re-dispatch
                    // a rerun that parks at the gate. Wait for both.
                    if (window.visibleSyncStaleSkipsForTest() == 0
                        || !window.visibleSyncWorkerWaitingForTest()) {
                        return;
                    }
                    if (window.visibleSyncStaleSkipsForTest() != 1) {
                        finish(5);  // exactly one drop expected; more is a bug
                        return;
                    }
                    // The rerun is still gated. The two panels the re-slice never
                    // touched must still hold the sentinel range -- the stale
                    // union was dropped, not applied to them.
                    const auto probes = window.contourViewProbesForTest();
                    int atSentinel = 0;
                    for (const auto& probe : probes) {
                        if (probe.displayMinimum == kSentinelMin
                            && probe.displayMaximum == kSentinelMax) {
                            ++atSentinel;
                        }
                    }
                    *sentinelsHeld = (atSentinel == 2);
                    *phase = 4;
                    window.releaseVisibleSyncGateForTest();  // let the rerun apply
                    return;
                }
                if (*phase == 4) {
                    finish(*sentinelsHeld ? 0 : 6);
                }
            });
        QTimer::singleShot(20000, &application,
            [finish] { finish(4); });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--particle-visible-range-smoke-test") {
        // A shared Visible-range reconciliation replaces all three rasters.
        // Particle point batches must be restored after those setImage calls.
        const std::filesystem::path path(argv[2]);
        auto* poll = new QTimer(&window);
        poll->setInterval(10);
        auto attempts = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, poll](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                window.setParticleSelectionForTest({"Tracer"}, 1.0, 37);
                if (!window.particleLoadingForTest()
                    || !window.particleLoadingUiActiveForTest()) {
                    application.exit(2);
                    return;
                }
                poll->start();
            });
        QObject::connect(poll, &QTimer::timeout, &application,
            [&window, &application, poll, attempts] {
                if (++*attempts > 500) {
                    application.exit(1);
                    return;
                }
                if (window.particleSampleCountForTest() == 0
                    || window.particleOverlayCountForTest() == 0
                    || window.particleSeedForTest() != 37
                    || window.particleLoadingForTest()
                    || !window.particleLoadingUiSettledForTest()) {
                    return;
                }
                poll->stop();
                const QColor particleColor(12, 34, 56, 77);
                window.setParticleColorForTest("Tracer", particleColor);
                if (!window.particleOverlaysUseColorForTest(particleColor)) {
                    application.exit(1);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, particleColor] {
                        application.exit(
                            window.particleSampleCountForTest() > 0
                                && window.particleOverlayCountForTest() > 0
                                && window.particleSeedForTest() == 37
                                && window.particleOverlaysUseColorForTest(
                                    particleColor)
                            ? 0 : 1);
                    }, Qt::SingleShotConnection);
                window.enableVisibleRasterForTest();
            });
        QTimer::singleShot(0, &window, [&window, path] {
            window.openDataset(path);
        });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--particle-dialog-smoke-test") {
        // The particles dialog is modeless with an Apply button: settings are
        // meant to be tried against the image, so the dialog must not block the
        // main window, must survive Apply, and must not stack up copies of
        // itself when the menu item is chosen again. It also belongs to the
        // dataset whose species it lists, so opening a sequence -- which never
        // runs openDatasetImpl -- must take it down, as it must the contours
        // dialog beside it.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        // Both close with WA_DeleteOnClose, so a just-closed one can still be a
        // child of the window; the live dialog is the visible one.
        const auto liveNamedDialog
            = [&window](const QString& name) -> QDialog* {
            for (auto* candidate : window.findChildren<QDialog*>(name)) {
                if (candidate->isVisible()) {
                    return candidate;
                }
            }
            return nullptr;
        };
        const auto liveDialog = [liveNamedDialog]() {
            return liveNamedDialog(QStringLiteral("particlesDialog"));
        };
        auto* poll = new QTimer(&window);
        poll->setInterval(10);
        auto attempts = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, poll](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                auto* action = window.findChild<QAction*>(
                    QStringLiteral("particlesAction"));
                if (action == nullptr || !action->isEnabled()) {
                    qCritical("the particles menu item is missing or disabled");
                    application.exit(1);
                    return;
                }
                action->trigger();
                action->trigger();
                const auto dialogs = window.findChildren<QDialog*>(
                    QStringLiteral("particlesDialog"));
                if (dialogs.size() != 1) {
                    qCritical("expected one particles dialog, found %lld",
                        static_cast<long long>(dialogs.size()));
                    application.exit(1);
                    return;
                }
                auto* dialog = dialogs.front();
                if (!dialog->isVisible() || dialog->isModal()
                    || QApplication::activeModalWidget() != nullptr) {
                    qCritical("the particles dialog blocks the main window");
                    application.exit(1);
                    return;
                }
                auto* buttons = dialog->findChild<QDialogButtonBox*>(
                    QStringLiteral("particlesDialogButtons"));
                if (buttons == nullptr
                    || buttons->button(QDialogButtonBox::Apply) == nullptr) {
                    qCritical("the particles dialog has no Apply button");
                    application.exit(1);
                    return;
                }
                buttons->button(QDialogButtonBox::Apply)->click();
                // Apply draws the checked species and leaves the dialog up.
                if (!dialog->isVisible() || !window.particleLoadingForTest()) {
                    qCritical("Apply did not draw, or closed the dialog");
                    application.exit(1);
                    return;
                }
                poll->start();
            }, Qt::SingleShotConnection);
        // Let the Apply read finish rather than tearing the window down around
        // a live worker, then check the rest of the dialog's lifecycle.
        QObject::connect(poll, &QTimer::timeout, &application,
            [&window, &application, poll, attempts, liveDialog, liveNamedDialog,
                first, second] {
                if (++*attempts > 500) {
                    // exit() only flags the loop, so stop the timer too rather
                    // than report the same stall on every tick until it unwinds.
                    poll->stop();
                    qCritical("the particle read started by Apply never settled");
                    application.exit(1);
                    return;
                }
                if (window.particleLoadingForTest()) {
                    return;
                }
                poll->stop();
                auto* dialog = liveDialog();
                if (dialog == nullptr) {
                    qCritical("the dialog did not survive the particle read");
                    application.exit(1);
                    return;
                }
                auto* buttons = dialog->findChild<QDialogButtonBox*>(
                    QStringLiteral("particlesDialogButtons"));
                if (buttons == nullptr
                    || buttons->button(QDialogButtonBox::Ok) == nullptr) {
                    qCritical("the particles dialog has no Ok button");
                    application.exit(1);
                    return;
                }
                buttons->button(QDialogButtonBox::Ok)->click();
                if (liveDialog() != nullptr) {
                    qCritical("Ok did not close the dialog");
                    application.exit(1);
                    return;
                }
                auto* action = window.findChild<QAction*>(
                    QStringLiteral("particlesAction"));
                if (action == nullptr || !action->isEnabled()) {
                    qCritical("the particles menu item did not come back");
                    application.exit(1);
                    return;
                }
                action->trigger();
                if (liveDialog() == nullptr) {
                    qCritical("the dialog did not reopen");
                    application.exit(1);
                    return;
                }
                // The contours dialog is the other one bound to this dataset.
                auto* contoursAction = window.findChild<QAction*>(
                    QStringLiteral("contoursAction"));
                if (contoursAction == nullptr || !contoursAction->isEnabled()) {
                    qCritical("the contours menu item is missing or disabled");
                    application.exit(1);
                    return;
                }
                contoursAction->trigger();
                const auto contoursName = QStringLiteral("setContoursDialog");
                if (liveNamedDialog(contoursName) == nullptr) {
                    qCritical("the contours dialog did not open");
                    application.exit(1);
                    return;
                }
                // The species and fields they list belong to the outgoing
                // dataset, which a sequence open replaces.
                window.openSequence({first, second});
                if (liveDialog() != nullptr) {
                    qCritical("opening a sequence left the dialog on screen");
                    application.exit(1);
                    return;
                }
                if (liveNamedDialog(contoursName) != nullptr) {
                    qCritical(
                        "opening a sequence left the contours dialog on screen");
                    application.exit(1);
                    return;
                }
                window.close();
                application.exit(0);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window, [&window, first] {
            window.openDataset(first);
        });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--particle-settings-reset-smoke-test") {
        // Particle settings belong to the dataset they were chosen for. Both
        // paths that install a different one owe the same reset: a plain open,
        // and a sequence open, which reaches it through prepareSequence rather
        // than openDatasetImpl. Every setting resets, not just the species --
        // a subset chosen for a dense plotfile decimates the next one silently.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        const QColor chosenColor(12, 34, 56);
        // The startup point size is the default this compares against, so the
        // test does not have to name the constant.
        const auto defaultPointSize
            = std::make_shared<int>(window.particlePointSizeForTest());
        const auto choose = [&window, chosenColor] {
            window.setParticleSelectionForTest({"Tracer"}, 0.0005, 37);
            window.setParticlePointSizeForTest(9);
            window.setParticleColorForTest("Tracer", chosenColor);
            return window.particleSeedForTest() == 37
                && window.particleFractionForTest() == 0.0005
                && window.particlePointSizeForTest() == 9
                && window.particleColorForTest("Tracer") == chosenColor;
        };
        const auto wasReset = [&window, chosenColor, defaultPointSize](
                                  const char* what) {
            if (window.particleSeedForTest() == 0
                && window.particleFractionForTest() == 1.0
                && window.particlePointSizeForTest() == *defaultPointSize
                && window.particleColorForTest("Tracer") != chosenColor) {
                return true;
            }
            qCritical("%s inherited particle settings: seed %llu, subset %g, "
                      "point size %d, colour %s",
                what,
                static_cast<unsigned long long>(window.particleSeedForTest()),
                window.particleFractionForTest(),
                window.particlePointSizeForTest(),
                qUtf8Printable(
                    window.particleColorForTest("Tracer").name(QColor::HexArgb)));
            return false;
        };
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application,
            [&window, &application, first, second, phase, choose, wasReset](
                bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                if (*phase == 0) {
                    if (!choose()) {
                        qCritical("the particle settings did not take");
                        application.exit(1);
                        return;
                    }
                    // A plain open of a different plotfile resets them.
                    *phase = 1;
                    window.openDataset(second);
                    return;
                }
                if (!wasReset("a plain open")) {
                    application.exit(1);
                    return;
                }
                if (!choose()) {
                    qCritical("the particle settings did not take");
                    application.exit(1);
                    return;
                }
                // prepareSequence resets synchronously; frame 0 then arrives
                // through a different path, which must not put them back.
                window.openSequence({first, second});
                if (!wasReset("opening a sequence")) {
                    application.exit(1);
                }
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed, &application,
            [&window, &application, wasReset](int index) {
                if (index != 0) {
                    return;
                }
                if (!wasReset("the first sequence frame")) {
                    application.exit(1);
                    return;
                }
                window.close();
                application.exit(0);
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window, [&window, first] {
            window.openDataset(first);
        });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--raster-zoom-smoke-test") {
        // 2-D Visible-range raster/color-bar consistency: after a full-domain
        // Visible slice caches the range, a zoom must re-render the raster
        // against that reused range (not the subregion's local range) so it
        // matches the color bar. See raster-colorbar-mismatch-on-2d-visible-zoom.
        // interactiveSlicesSettled fires twice: after the full-domain slice
        // (phase 0 -> zoom) and after the zoom (phase 1 -> verify). phase is a
        // shared_ptr so it outlives this branch's scope through exec().
        const std::filesystem::path path(argv[2]);
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                const auto* sync = window.findChild<QAction*>(
                    QStringLiteral("syncRubberBandZoomAction"));
                if (!success || sync == nullptr || sync->isVisible()) {
                    application.exit(1);
                    return;
                }
                window.enableVisibleRasterForTest();
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [&window, &application, phase] {
                if (*phase == 0) {
                    *phase = 1;
                    window.zoomActiveViewForTest();
                } else {
                    application.exit(
                        window.activeViewRasterMatchesDisplayRangeForTest()
                            ? 0 : 1);
                }
        });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--spherical-supersample-smoke-test") {
        // Zoom-preserve regression for the 2-D spherical supersample control:
        // after zooming a spherical view (view-only, no re-slice), changing the
        // warp factor must resize the warped raster yet keep the same zoomed
        // framing rather than refitting to the whole sector.
        const std::filesystem::path path(argv[2]);
        auto beforeWidth = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, beforeWidth](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                // Spherical zoom is view-only; it must leave fit-to-window.
                window.rubberBandZoomActiveViewForTest();
                if (window.activeViewFitsWindowForTest()) {
                    application.exit(2);
                    return;
                }
                *beforeWidth = window.activeViewImageWidthForTest();
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, beforeWidth] {
                        const int afterWidth = window.activeViewImageWidthForTest();
                        // The 8x warp resized the raster larger, and the view is
                        // still zoomed (framing preserved, not refit to fit).
                        const bool resized = afterWidth > *beforeWidth;
                        const bool preserved = !window.activeViewFitsWindowForTest();
                        application.exit(resized && preserved ? 0 : 3);
                    }, Qt::SingleShotConnection);
                // Default factor is 4x; bump to 8x so the raster grows.
                window.setSphericalSupersampleForTest(8);
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--rubber-zoom-sync-smoke-test") {
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                auto* sync = window.findChild<QAction*>(
                    QStringLiteral("syncRubberBandZoomAction"));
                if (!success || sync == nullptr || !sync->isVisible()) {
                    application.exit(1);
                    return;
                }
                const QSignalBlocker blocker(sync);
                sync->setChecked(true);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application] {
                        application.exit(
                            window.allViewsRubberBandZoomedForTest() ? 0 : 1);
                    }, Qt::SingleShotConnection);
                window.rubberBandZoomActiveViewForTest();
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--rubber-zoom-local-smoke-test") {
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                auto* sync = window.findChild<QAction*>(
                    QStringLiteral("syncRubberBandZoomAction"));
                if (!success || sync == nullptr) {
                    application.exit(1);
                    return;
                }
                const QSignalBlocker blocker(sync);
                sync->setChecked(false);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application] {
                        application.exit(
                            window.rubberBandZoomedViewCountForTest() == 1
                                ? 0 : 1);
                    }, Qt::SingleShotConnection);
                window.rubberBandZoomActiveViewForTest();
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--rubber-overzoom-smoke-test") {
        // Regression for issue #45 (over-zoom after rubber-band): on a dataset
        // whose full-domain raster is capped at maxSliceOutputDimension, the
        // cropped re-slice arrives at a finer pixels-per-cell density than the
        // raster it replaces. Preserving the scene transform then shows the
        // crop over-zoomed with part of it outside the viewport. Rubber-band
        // the central half and require the arrived crop to be fully visible.
        const std::filesystem::path path(argv[2]);
        // Distinct exit codes so a failure pinpoints its stage: 2 = the load
        // itself failed, 3 = the initial fitted raster was not fully visible,
        // 1 = the regression (arrived crop not fully framed).
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                // Sanity: the fitted full-domain raster starts fully visible.
                if (!window.activeViewShowsWholeImageForTest()) {
                    application.exit(3);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application] {
                        application.exit(
                            window.activeViewIsZoomedForTest()
                                && window.activeViewShowsWholeImageForTest()
                            ? 0 : 1);
                    }, Qt::SingleShotConnection);
                window.rubberBandZoomActiveViewForTest();
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--pan-zoom-smoke-test") {
        const std::filesystem::path path(argv[2]);
        auto phase = std::make_shared<int>(0);
        auto immediateScale = std::make_shared<double>(0.0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, immediateScale](bool success) {
                auto* sync = window.findChild<QAction*>(
                    QStringLiteral("syncRubberBandZoomAction"));
                if (!success || sync == nullptr) {
                    application.exit(1);
                    return;
                }
                const QSignalBlocker blocker(sync);
                sync->setChecked(true);
                window.rubberBandZoomActiveViewForTest();
                *immediateScale = window.activeViewScaleForTest();
                // Exercise the timing window: pan before the cropped slice
                // requested by the rubber band has settled.
                window.panActiveViewForTest(5.0, 0.0);
                if (std::abs(window.activeViewScaleForTest() - *immediateScale)
                    > 1.0e-12) {
                    application.exit(1);
                }
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [&window, &application, phase, immediateScale] {
                constexpr double tolerance = 1.0e-12;
                if ((*phase)++ == 0) {
                    if (std::abs(window.activeViewScaleForTest()
                            - *immediateScale)
                        > tolerance) {
                        application.exit(1);
                        return;
                    }
                    // A valid pan from the central crop must preserve the
                    // active panel's custom transform through the re-slice.
                    window.setActiveViewScaleForTest(4);
                    window.panActiveViewForTest(-5.0, 0.0);
                    return;
                }
                if (std::abs(window.activeViewScaleForTest() - 4.0)
                    > tolerance) {
                    application.exit(1);
                    return;
                }
                // The first pan reached the domain edge. Panning farther is a
                // no-op and must not refit the panel either.
                window.setActiveViewScaleForTest(3);
                window.panActiveViewForTest(-5.0, 0.0);
                application.exit(
                    std::abs(window.activeViewScaleForTest() - 3.0)
                            <= tolerance
                        ? 0 : 1);
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--range-cache-smoke-test") {
        // Regression for sequence-frame-range-cache-goes-stale: cache the
        // full-domain Visible range on frame 0, step to frame 1 (whose field is
        // 10x-scaled), zoom, and confirm the color bar tracks frame 1 instead
        // of reusing frame 0's cached range. Two signals interleave, sequenced
        // by a phase held in a shared_ptr so it outlives this branch scope.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        struct RangeCacheState { int phase = 0; double frame0Max = 0.0; };
        auto state = std::make_shared<RangeCacheState>();
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window](int index) {
                if (index == 0) {
                    window.enableVisibleRasterForTest();  // cache frame 0 range
                } else {
                    window.zoomActiveViewForTest();        // re-slice frame 1
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [&window, &application, state] {
                const auto probes = window.contourViewProbesForTest();
                if (probes.empty()) {
                    application.exit(1);
                    return;
                }
                const auto displayMax = probes.front().displayMaximum;
                if (state->phase == 0) {
                    state->frame0Max = displayMax;   // frame 0 full-domain max
                    state->phase = 1;
                    window.stepSequence(1);
                } else {
                    // Frame 1 is scaled 10x, so its zoomed max must far exceed
                    // frame 0's cached max. Reusing the stale cache (the bug)
                    // would instead leave them equal.
                    application.exit(
                        displayMax > 2.0 * state->frame0Max ? 0 : 1);
                }
            });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--raw-fab-smoke-test") {
        const std::filesystem::path path(argv[2]);
        // Shared, not a block-scoped local captured by reference: the
        // connection outlives this else-if block, and a by-reference phase
        // is a stack-use-after-scope once main moves on (caught by the
        // Qt-enabled ASan build).
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, phase](bool success) {
                auto* selector =
                    window.findChild<amrvis::qt::FabSelectorDock*>();
                const auto valid = success && selector != nullptr
                    && selector->isVisible() && selector->entries().size() >= 2
                    && fabSelectorIsAscending(*selector)
                    && fabSelectorColumnsMatch(*selector, false)
                    && fabSelectorPointFilterMatches(*selector, *phase == 0)
                    && fabRangeSelectorMatches(window);
                if (!valid) {
                    application.exit(1);
                } else if ((*phase)++ == 0) {
                    // The unique point match starts the FAB load.
                } else {
                    application.exit(
                        clearFabSelectorPointFilter(*selector) ? 0 : 1);
                }
            });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--multifab-fab-smoke-test") {
        const std::filesystem::path path(argv[2]);
        // Shared for the same lifetime reason as the raw-fab branch above.
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, phase](bool success) {
                auto* selector =
                    window.findChild<amrvis::qt::FabSelectorDock*>();
                if (!success || selector == nullptr
                    || selector->entries().size() < 2
                    || !fabSelectorIsAscending(*selector)
                    || !fabSelectorColumnsMatch(*selector, true)
                    || !fabSelectorPointFilterMatches(
                        *selector, *phase == 0)) {
                    application.exit(1);
                    return;
                }
                if (*phase == 0) {
                    ++*phase;
                } else if (*phase == 1) {
                    auto* back = selector->findChild<QPushButton*>(
                        QStringLiteral("fabBackButton"));
                    if (back == nullptr || !back->isVisible()
                        || !fabRangeSelectorMatches(window)
                        || !clearFabSelectorPointFilter(*selector)) {
                        application.exit(1);
                        return;
                    }
                    ++*phase;
                    QTimer::singleShot(0, back, &QPushButton::click);
                } else {
                    const auto* back = selector->findChild<QPushButton*>(
                        QStringLiteral("fabBackButton"));
                    application.exit(
                        back != nullptr && !back->isVisible() ? 0 : 1);
                }
            });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--fab-overlap-failure-smoke-test") {
        // Regression for the FAB selector rollback under overlapping opens.
        // Commit record 0 (call it X), then click record 1 twice in one slot so
        // both reads are in flight together, and make both fail. The second
        // click must inherit X as its rollback rather than snapshotting the
        // dock -- which by then shows record 1, a selection that was never
        // displayed. Without that, the failure restores record 1 and the dock
        // claims a FAB the window is not showing.
        //
        // The overlap is structural, not timed: the two viewFab calls run in
        // one event-loop slot, so neither completion can have been delivered,
        // and the pool gate additionally holds both reads until the file is
        // gone so both are guaranteed to fail.
        const std::filesystem::path path(argv[2]);
        auto phase = std::make_shared<int>(0);
        auto baselineErrors = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, phase, path,
                baselineErrors](bool success) {
                auto* selector =
                    window.findChild<amrvis::qt::FabSelectorDock*>();
                if (!success || selector == nullptr
                    || selector->entries().size() < 2) {
                    application.exit(1);
                    return;
                }
                if (*phase == 0) {
                    // Commit X = record 0 through the normal async path.
                    *phase = 1;
                    window.viewFabForTest(0);
                    return;
                }
                if (*phase != 1) {
                    return;
                }
                *phase = 2;
                if (selector->selectedOrdinal() != std::optional<std::size_t>{0}) {
                    application.exit(1);   // X did not commit
                    return;
                }
                *baselineErrors = window.backgroundErrorCountForTest();
                auto* pool = QThreadPool::globalInstance();
                pool->setMaxThreadCount(1);
                auto gate = std::make_shared<std::atomic<bool>>(false);
                pool->start(QRunnable::create([gate] {
                    while (!gate->load()) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(2));
                    }
                }));
                // Both queued behind the gate, in one slot: A is superseded by
                // B before either can complete.
                window.viewFabForTest(1);
                window.viewFabForTest(1);
                std::error_code removeError;
                std::filesystem::remove(path, removeError);
                gate->store(true);
                // Wait for the failure to be reported, then assert. A watchdog
                // below fails the run rather than letting it hang.
                auto* poll = new QTimer(&window);
                poll->setInterval(5);
                QObject::connect(poll, &QTimer::timeout, &window,
                    [&window, &application, selector, poll, baselineErrors] {
                        if (window.backgroundErrorCountForTest()
                            <= *baselineErrors) {
                            return;
                        }
                        poll->stop();
                        // The rollback must be X, not the record that was
                        // merely highlighted when the second click landed.
                        application.exit(selector->selectedOrdinal()
                                == std::optional<std::size_t>{0}
                            ? 0 : 1);
                    });
                poll->start();
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--fab-direct-open-failure-smoke-test") {
        // Regression for a superseding request that brings no rollback of its
        // own. Commit record 0 (X), click record 1 so the dock moves to it with
        // a read in flight, then take the direct "open a raw FAB file" path --
        // which the app reaches through a file dialog and which passes no
        // rollback -- for a file that does not exist. The direct open retires
        // the click, so the click restores nothing; if the direct open does not
        // inherit the click's rollback, its own failure restores nothing either
        // and the dock is left on record 1 while X is still displayed.
        const std::filesystem::path path(argv[2]);
        auto phase = std::make_shared<int>(0);
        auto baselineErrors = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, phase, path,
                baselineErrors](bool success) {
                auto* selector =
                    window.findChild<amrvis::qt::FabSelectorDock*>();
                if (!success || selector == nullptr
                    || selector->entries().size() < 2) {
                    application.exit(1);
                    return;
                }
                if (*phase == 0) {
                    *phase = 1;
                    window.viewFabForTest(0);
                    return;
                }
                if (*phase != 1) {
                    return;
                }
                *phase = 2;
                if (selector->selectedOrdinal()
                    != std::optional<std::size_t>{0}) {
                    application.exit(1);
                    return;
                }
                *baselineErrors = window.backgroundErrorCountForTest();
                auto* pool = QThreadPool::globalInstance();
                pool->setMaxThreadCount(1);
                auto gate = std::make_shared<std::atomic<bool>>(false);
                pool->start(QRunnable::create([gate] {
                    while (!gate->load()) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(2));
                    }
                }));
                // Both queued behind the gate and issued in one slot, so the
                // click is genuinely unresolved when the direct open supersedes
                // it. The direct open's target does not exist, so it fails.
                window.viewFabForTest(1);
                window.openStandaloneFabForTest(
                    path.parent_path() / "no_such_fab_file");
                gate->store(true);
                auto* poll = new QTimer(&window);
                poll->setInterval(5);
                QObject::connect(poll, &QTimer::timeout, &window,
                    [&window, &application, selector, poll, baselineErrors] {
                        if (window.backgroundErrorCountForTest()
                            <= *baselineErrors) {
                            return;
                        }
                        poll->stop();
                        application.exit(selector->selectedOrdinal()
                                == std::optional<std::size_t>{0}
                            ? 0 : 1);
                    });
                poll->start();
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--fab-zoom-smoke-test") {
        // Regression for fab-round-trip-loses-visible-region: zoom the MultiFab
        // slice, drill into a FAB, go back, and confirm the restored MultiFab
        // view still holds the zoom. Without the fix the round-trip resets it to
        // full domain. initialSliceFinished fires on each open (MultiFab, FAB,
        // restored MultiFab); interactiveSlicesSettled fires once for the zoom.
        // A shared phase sequences the two signals across this branch's scope.
        const std::filesystem::path path(argv[2]);
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, phase](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                if (*phase == 0) {
                    *phase = 1;
                    window.zoomActiveViewForTest();       // zoom the MultiFab
                } else if (*phase == 2) {
                    *phase = 3;
                    auto* back = window.findChild<QPushButton*>(
                        QStringLiteral("fabBackButton"));
                    if (back == nullptr) {
                        application.exit(1);
                        return;
                    }
                    QTimer::singleShot(0, back, &QPushButton::click);  // go back
                } else if (*phase == 3) {
                    application.exit(
                        window.activeViewIsZoomedForTest() ? 0 : 1);
                }
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [&window, phase] {
                if (*phase == 1) {
                    *phase = 2;
                    window.viewFabForTest(0);             // drill into FAB 0
                }
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--cache-budget-smoke-test") {
        // Regression for cache-budget-exceeded-hard-fails-after-load: load a
        // 2-D dataset at finest, shrink the cache budget just below the finest
        // working set, then switch field to force a non-cache finest re-slice
        // that overflows the budget. With the fix the slice degrades to a lower
        // composite level (the level combo drops from "Finest available", -1);
        // without it the slice hard-fails and the level is unchanged.
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                if (!success) {
                    application.exit(1);
                    return;
                }
                auto* levels = window.findChild<QComboBox*>(
                    QStringLiteral("levelSelector"));
                auto* fields = window.findChild<QComboBox*>(
                    QStringLiteral("fieldSelector"));
                if (levels == nullptr || fields == nullptr
                    || fields->count() < 2
                    || levels->currentData().toInt() != -1) {
                    application.exit(1);  // expected finest (-1) with >=2 fields
                    return;
                }
                const auto resident = window.cacheResidentBytesForTest();
                if (resident == 0) {
                    application.exit(1);
                    return;
                }
                window.setCacheBudgetForTest(resident - 1);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&application, levels] {
                        // With the fix the overflowing finest re-slice fell back
                        // to a lower composite level, so the combo no longer
                        // reads "Finest available" (-1).
                        application.exit(
                            levels->currentData().toInt() != -1 ? 0 : 1);
                    });
                fields->setCurrentIndex(1);  // non-cache finest re-slice
            });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--sequence-noop-smoke-test") {
        // Two sequence annoyances at once, because both are observed on the
        // same frame step. On frame 0: hide the Animation dock, then ask for
        // frame 0 again the way an idle slider press-and-release does. That
        // must not reload -- a reload would close the inspection windows,
        // cancel work, and re-render the frame already on screen -- so the
        // second displayed frame must be frame 1, not another frame 0. And on
        // reaching frame 1, the dock must still be hidden: a frame refresh has
        // no business reasserting the user's dock choice.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        auto displays = std::make_shared<std::vector<int>>();
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed, &application,
            [&window, &application, displays](int index) {
                displays->push_back(index);
                if (displays->size() == 1) {
                    if (index != 0) {
                        qCritical("sequence started on frame %d", index);
                        application.exit(1);
                        return;
                    }
                    window.setAnimationDockVisibleForTest(false);
                    window.requestSequenceFrameForTest(0);
                    // Do *not* step yet. Stepping immediately bumps the load
                    // generation and cancels the redundant frame-0 load before
                    // it can display, so the observed sequence is [0, 1]
                    // whether or not the request was suppressed -- which is to
                    // say the assertion below would pass against the bug it
                    // exists for. Give the reload time to arrive instead: if
                    // one was started, it displays frame 0 a second time and
                    // the branch below catches it.
                    //
                    // This margin is a timing assumption, and it fails open: a
                    // machine loaded enough to keep a redundant frame-0 reload
                    // of a small local fixture from displaying inside 500 ms
                    // would let this pass without testing anything. It is not
                    // the only cover. test_sequence_controller pins the same
                    // property deterministically by counting
                    // frameSwitchStarted, which is emitted synchronously
                    // exactly when a switch proceeds; what is left here is the
                    // end-to-end check that MainWindow's slider path reaches
                    // that suppression at all.
                    QTimer::singleShot(500, &window, [&window] {
                        window.stepSequence(1);
                    });
                    return;
                }
                if (index != 1 || displays->size() != 2) {
                    qCritical("a no-op frame request reloaded the frame");
                    application.exit(1);
                    return;
                }
                if (window.animationDockVisibleForTest()) {
                    qCritical("a frame refresh reopened the Animation dock");
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--sequence-failure-smoke-test") {
        // Playback wraps, so a frame that cannot be read comes back around
        // forever, raising a diagnostic every cycle. Start playing a sequence
        // whose second frame is unreadable and require playback to have
        // stopped by the time the failure is reported.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed, &application,
            [&window](int index) {
                if (index == 0 && !window.sequencePlayingForTest()) {
                    window.toggleSequencePlaybackForTest();
                }
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed, &application,
            [&window, &application] {
                // Queued: the failure handler stops playback around this
                // signal, so read the state once that handler has finished.
                QTimer::singleShot(0, &window, [&window, &application] {
                    if (window.sequencePlayingForTest()) {
                        qCritical("playback kept running past a failed frame");
                        application.exit(1);
                        return;
                    }
                    application.exit(0);
                });
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--sequence-smoke-test") {
        // Opens the two-frame sequence, waits for the first frame to display,
        // steps to frame 1 through the same slot the step button uses, and
        // exits 0 once frame 1 is on screen.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application](int index) {
                if (index == 0) {
                    if (window.particleSampleCountForTest() != 0) {
                        application.exit(1);
                        return;
                    }
                    // Opt in, start a frame load carrying that specification,
                    // then change it before the worker can complete. The final
                    // frame must reflect the new empty selection.
                    window.setParticleSelectionForTest({"Tracer"}, 1.0, 37);
                    window.stepSequence(1);
                    window.setParticleSelectionForTest({}, 1.0, 41);
                } else if (index == 1) {
                    application.exit(
                        window.particleSampleCountForTest() == 0
                                && window.particleSeedForTest() == 41
                            ? 0 : 1);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 5
        && std::string_view(argv[1]) == "--sequence-after-fab-smoke-test") {
        // Regression for open-sequence-stale-fab-state: open a raw FAB (enters
        // FAB mode -- selector dock visible, "— FAB" title suffix), then open a
        // plotfile sequence. openSequence does not go through openDatasetImpl,
        // so without the reset the FAB mode, dock, and title leak into the
        // frames. Exit 0 only if the first frame shows with the dock hidden and
        // no "— FAB" title.
        const std::filesystem::path fab(argv[2]);
        const std::filesystem::path first(argv[3]);
        const std::filesystem::path second(argv[4]);
        const auto inFabMode = [](const amrvis::qt::MainWindow& w) {
            const auto* selector = w.findChild<amrvis::qt::FabSelectorDock*>();
            return selector != nullptr && selector->isVisible()
                && w.windowTitle().endsWith(QStringLiteral(" FAB"));
        };
        auto opened = std::make_shared<bool>(false);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, first, second, opened,
                inFabMode](bool success) {
                if (*opened) {
                    return;  // later FAB re-slices are irrelevant
                }
                // Precondition: the raw FAB really did enter FAB mode, so the
                // sequence open below is exercising the leak.
                if (!success || !inFabMode(window)) {
                    application.exit(1);
                    return;
                }
                *opened = true;
                window.openSequence({first, second});
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application, inFabMode](int index) {
                if (index != 0) {
                    return;
                }
                application.exit(inFabMode(window) ? 1 : 0);
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, fab] { window.openDataset(fab); });
    } else if (argc == 5
        && std::string_view(argv[1])
            == "--remote-sequence-after-fab-smoke-test") {
        const std::filesystem::path fab(argv[2]);
        const std::string first(argv[3]);
        const std::string second(argv[4]);
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        auto opened = std::make_shared<bool>(false);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, first, second, opened,
                server = smokeServer](bool success) {
                if (*opened) {
                    return;
                }
                const auto* selector
                    = window.findChild<amrvis::qt::FabSelectorDock*>();
                if (!success || selector == nullptr || !selector->isVisible()
                    || !window.windowTitle().endsWith(
                        QStringLiteral(" FAB"))) {
                    application.exit(1);
                    return;
                }
                *opened = true;
                attachSmokeServer(window, server);
                window.openRemoteSequence({first, second});
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application](int index) {
                if (index == 0) {
                    application.exit(
                        window.fabStateClearedForTest() ? 0 : 1);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, fab] { window.openDataset(fab); });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--sequence-spec-change-smoke-test") {
        // Start frame 1, then immediately drive an ordinary slice-affecting
        // control through scheduleSliceRequest while its worker is in flight.
        // The queued completion for the obsolete specification must not strand
        // the sequence in its in-flight state.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application](int index) {
                if (index == 0) {
                    window.stepSequence(1);
                    window.enableVisibleRasterForTest();
                } else if (index == 1) {
                    application.exit(0);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--fixed-scale-arrival-smoke-test") {
        const std::filesystem::path path(argv[2]);
        const int factor = std::stoi(argv[3]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, factor](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                window.selectFixedScaleForTest(factor);
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, factor] {
                        application.exit(
                            window.fixedScaleStateMatchesForTest(factor)
                                ? 0 : 1);
                    }, Qt::SingleShotConnection);
                // Force an asynchronous replacement raster after selecting the
                // scale, reproducing the delayed-arrival race.
                window.enableVisibleRasterForTest();
            }, Qt::SingleShotConnection);
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 3 && std::string_view(argv[1])
            == "--fixed-scale-centre-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        // Regression for fixed-scale-switch-lands-off-center-remotely.
        // Selecting a fixed scale is supposed to keep looking at the same
        // place. Local does that implicitly, through the view's own
        // transformation anchor; remote has to re-centre explicitly, on the
        // centre viewCenterInData reports. Those two only agree if that centre
        // is the true one -- and it was a fraction of a raster pixel off, which
        // is many finest cells on a domain this wide. Open the same dataset
        // remotely, switch to 1x without touching the view, and require the
        // resulting window to be centred on the domain, which is where a
        // fitted view was looking.
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, phase](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application, phase] {
                        if (*phase != 0) {
                            return;
                        }
                        *phase = 1;
                        const auto shown
                            = window.activeViewVisibleDataWindowForTest();
                        const auto domain
                            = window.datasetPhysicalDomainForTest();
                        if (!(shown.width() > 0.0)) {
                            qCritical("no visible window after the switch");
                            application.exit(1);
                            return;
                        }
                        const auto drift = std::abs(
                            shown.center().x() - domain.center().x());
                        const auto cellSize
                            = window.activeViewFinestCellSizeForTest();
                        // One finest cell of slack: the fetch window is
                        // quantised to whole cells, and nothing more than that
                        // is explainable.
                        if (drift > cellSize) {
                            qCritical("the switch left the view %g off centre "
                                      "(%g finest cells)",
                                drift, cellSize > 0.0 ? drift / cellSize : 0.0);
                            application.exit(1);
                            return;
                        }
                        application.exit(0);
                    });
                window.selectFixedScaleForTest(1);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--effective-scale-smoke-test") {
        // A domain wider than maxSliceOutputDimension finest cells cannot have
        // a whole-domain raster at finest resolution, so a local fixed scale
        // magnifies it by less than the factor says. The UI has to state what
        // it actually applied, and the number it states has to be the one the
        // view is really using -- checked here against the visible window.
        const std::filesystem::path path(argv[2]);
        const int factor = std::stoi(argv[3]);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, factor](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                window.selectToolbarFixedScaleForTest(factor);
                // Measure past the layout pass the new scale's scroll bars
                // demand: they shrink the viewport, and the window below is
                // read in viewport pixels.
                QTimer::singleShot(200, &window,
                    [&window, &application, factor] {
                        const auto claimed
                            = window.effectiveFixedScaleForTest(factor);
                        if (!(claimed > 0.0)) {
                            qCritical("no reduced scale reported on a domain "
                                      "past the raster clamp");
                            application.exit(1);
                            return;
                        }
                        const auto label = window.scaleUiLabelForTest();
                        // →, not the raw character: QStringLiteral converts
                        // at compile time, and MSVC without a BOM or /utf-8
                        // (neither the windows preset nor
                        // amrexplorer_warnings.cmake passes it) reads the source
                        // as CP1252, so the three UTF-8 bytes would become three
                        // wrong code points here. The production label survives
                        // that because tr() takes a narrow literal and decodes
                        // it with fromUtf8 at run time, so only this comparison
                        // would break -- on windows-2022 alone.
                        if (!label.contains(QStringLiteral("\u2192"))) {
                            qCritical("the Scale button reports '%s', which "
                                      "does not state the applied scale",
                                qUtf8Printable(label));
                            application.exit(1);
                            return;
                        }
                        // The decorated label must not cost the menu its
                        // check: matching the radio on that string finds
                        // nothing, and the toolbar/menu split reopens on
                        // exactly the domains this reporting exists for.
                        const auto checked
                            = window.scaleMenuCheckedLabelForTest();
                        if (checked
                            != QStringLiteral("%1x").arg(factor)) {
                            qCritical("a clamped toolbar pick left View > "
                                      "Scale showing '%s'",
                                qUtf8Printable(checked));
                            application.exit(1);
                            return;
                        }
                        // What the view really does: viewport pixels per
                        // finest cell across the window it shows.
                        const auto window_ = window
                            .activeViewVisibleDataWindowForTest();
                        const auto viewport
                            = window.activeViewViewportSizeForTest();
                        const auto cellSize
                            = window.activeViewFinestCellSizeForTest();
                        if (!(window_.width() > 0.0) || !(cellSize > 0.0)) {
                            application.exit(1);
                            return;
                        }
                        const auto cells = window_.width() / cellSize;
                        const auto actual
                            = static_cast<double>(viewport[0]) / cells;
                        if (std::abs(actual - claimed) > 0.05 * claimed) {
                            qCritical("the UI claims %gx but the view applies "
                                      "%gx", claimed, actual);
                            application.exit(1);
                            return;
                        }
                        application.exit(0);
                    });
            }, ::Qt::SingleShotConnection);
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--scale-state-smoke-test") {
        // The toolbar Scale button and View > Scale are one state shown twice.
        // Pick 4x from the *toolbar* menu -- the path that used to leave the
        // View-menu radio unchecked -- and require the full agreement
        // fixedScaleStateMatchesForTest asserts. Then open a second dataset,
        // which arrives fitted, and require the report to have come back to
        // Fit rather than still claiming 4x.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, phase, second](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                if (*phase == 0) {
                    *phase = 1;
                    window.selectToolbarFixedScaleForTest(4);
                    if (!window.fixedScaleStateMatchesForTest(4)) {
                        qCritical("a toolbar scale pick left the state split");
                        application.exit(1);
                        return;
                    }
                    QTimer::singleShot(0, &window, [&window, second] {
                        window.openDataset(second);
                    });
                    return;
                }
                if (window.scaleUiLabelForTest() != QStringLiteral("Fit")) {
                    qCritical("a new dataset kept the old scale report '%s'",
                        qUtf8Printable(window.scaleUiLabelForTest()));
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, first] { window.openDataset(first); });
    } else if (argc == 5
        && std::string_view(argv[1]) == "--sequence-scale-report-smoke-test") {
        // The clamped scale report is computed from the active view's dataset,
        // and a sequence can carry a different domain than the dataset the
        // scale was picked on. A fixed scale is a persistent view mode and
        // survives the raster replacement, so the factor carries over -- but
        // what it *comes to* does not.
        //
        // Pick 4x on a narrow plotfile (literal, no clamp), then open a
        // sequence 8192 finest cells across, twice the largest whole-domain
        // raster. The same 4x now applies 2x, and the button has to say so
        // rather than keep the number it computed for the dataset before.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path frameOne(argv[3]);
        const std::filesystem::path frameTwo(argv[4]);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, frameOne, frameTwo](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                window.selectToolbarFixedScaleForTest(4);
                if (window.scaleUiLabelForTest() != QStringLiteral("4x")) {
                    qCritical("a narrow domain reported '%s', expected a "
                              "literal 4x",
                        qUtf8Printable(window.scaleUiLabelForTest()));
                    application.exit(1);
                    return;
                }
                QTimer::singleShot(0, &window, [&window, frameOne, frameTwo] {
                    window.openSequence({frameOne, frameTwo});
                });
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed, &application,
            [&window, &application](int) {
                const auto label = window.scaleUiLabelForTest();
                if (!label.startsWith(QStringLiteral("4x"))) {
                    qCritical("a sequence frame dropped the 4x scale: '%s'",
                        qUtf8Printable(label));
                    application.exit(1);
                    return;
                }
                if (label == QStringLiteral("4x")) {
                    qCritical("a wider sequence frame kept the previous "
                              "dataset's literal 4x, applying 2x");
                    application.exit(1);
                    return;
                }
                // ...and the number it now states must be the one in force.
                const auto effective = window.effectiveFixedScaleForTest(4);
                if (std::fabs(effective - 2.0) > 1.0e-9) {
                    qCritical("reported an effective scale of %f, expected 2",
                        effective);
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed, &application,
            [&application] { application.exit(2); });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, first] { window.openDataset(first); });
    } else if (argc == 5
        && std::string_view(argv[1]) == "--animation-dock-role-smoke-test") {
        // The Animation panel hosts two different sets of controls: the 3-D
        // slice sweep and the sequence transport. Hiding it while it holds the
        // sweep controls is not a standing refusal of the transport.
        //
        // Open a 3-D plotfile (the panel applies, and is shown), hide it, then
        // open a plotfile sequence. Testing one "applies" flag made that a
        // true -> true change, so neither the close nor the first frame was a
        // transition and the sequence arrived with its slider and play button
        // in a dock nothing would reopen.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        const std::filesystem::path bad(argv[4]);
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, first, second, bad, phase](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                if (!window.animationDockVisibleForTest()) {
                    qCritical("a 3-D dataset left the Animation panel hidden "
                              "(phase %d)", *phase);
                    application.exit(1);
                    return;
                }
                if (*phase == 0) {
                    // A failed open tears the 3-D dataset down, so the panel
                    // stops applying and must not stay up empty. The teardown's
                    // own call runs while the outgoing dataset is still
                    // installed, so it is not the one that can settle this.
                    *phase = 1;
                    QTimer::singleShot(0, &window,
                        [&window, bad] { window.openDataset(bad, true); });
                    return;
                }
                if (*phase == 2) {
                    // 3-D -> 3-D with the dock hidden. The teardown's
                    // closeSequence runs while the outgoing dataset is still
                    // installed, so the !applies branch that clears the flags
                    // never runs on this path; without an explicit reset the
                    // hide carried into the new dataset, while the same hide
                    // followed by a 2-D one reopened it.
                    *phase = 3;
                    window.setAnimationDockVisibleForTest(false);
                    QTimer::singleShot(0, &window,
                        [&window, first] { window.openDataset(first); });
                    return;
                }
                window.setAnimationDockVisibleForTest(false);
                QTimer::singleShot(0, &window, [&window, first, second] {
                    window.openSequence({first, second});
                });
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::datasetOpenFinished,
            &application, [&window, &application, first, phase](bool success) {
                if (*phase != 1) {
                    return;
                }
                if (success) {
                    qCritical("the bad path opened successfully");
                    application.exit(1);
                    return;
                }
                if (window.animationDockVisibleForTest()) {
                    qCritical("a failed open left an empty Animation panel up");
                    application.exit(1);
                    return;
                }
                *phase = 2;
                QTimer::singleShot(0, &window,
                    [&window, first] { window.openDataset(first); });
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed, &application,
            [&window, &application](int) {
                if (!window.animationDockVisibleForTest()) {
                    qCritical("a sequence opened with its transport controls "
                              "in a hidden Animation panel");
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed, &application,
            [&application] { application.exit(2); });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, first] { window.openDataset(first); });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--spherical-scale-report-smoke-test") {
        // A spherical view reports the plain factor, never a reduced one. Its
        // raster is warped, so one raster pixel does not stand for a fixed
        // number of finest cells and there is no single magnification to
        // state; effectiveFixedScale excludes it for the same reason
        // logicalImageSize does.
        //
        // The fixture is 8192 finest cells across -- twice the largest
        // whole-domain raster -- so a Cartesian view of the same size would
        // decorate. That is what makes this distinguish the exclusion from a
        // domain that simply does not clamp.
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                if (!window.displayIsSphericalForTest()) {
                    qCritical("the fixture did not open as a spherical view, "
                              "so this test proves nothing");
                    application.exit(1);
                    return;
                }
                // Pin the mode: choosing a display writes it through
                // saveSettings(), so it survives into the next run and this
                // test would otherwise inherit whatever the last one left.
                window.selectSphericalDisplayForTest(0);
                if (!window.displayIsSphericalWarpForTest()) {
                    qCritical("R-Z did not select, so the warp case is untested");
                    application.exit(1);
                    return;
                }
                window.selectToolbarFixedScaleForTest(32);
                const auto label = window.scaleUiLabelForTest();
                if (label != QStringLiteral("32x")) {
                    qCritical("an R-Z spherical view reported '%s', expected a "
                              "plain 32x",
                        qUtf8Printable(label));
                    application.exit(1);
                    return;
                }
                if (window.effectiveFixedScaleForTest(32) != 0.0) {
                    qCritical("an R-Z spherical view claimed a scale");
                    application.exit(1);
                    return;
                }
                // ...but only R-Z warps. r-theta draws the logical grid as-is
                // and theta-r transposes it, so both are clamped exactly like a
                // Cartesian raster and must report the reduction. Excluding
                // every spherical view left these two silently applying 16x
                // while the button said 32x.
                for (const auto mode : {1, 2}) {
                    window.selectSphericalDisplayForTest(mode);
                    if (window.displayIsSphericalWarpForTest()) {
                        qCritical("mode %d still reports as warped", mode);
                        application.exit(1);
                        return;
                    }
                    const auto effective = window.effectiveFixedScaleForTest(32);
                    if (std::fabs(effective - 16.0) > 1.0e-9) {
                        qCritical("unwarped spherical mode %d reported an "
                                  "effective scale of %f, expected 16",
                            mode, effective);
                        application.exit(1);
                        return;
                    }
                }
                window.selectSphericalDisplayForTest(0);
                application.exit(0);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--idle-ui-state-smoke-test") {
        // Two controls that are reachable before any dataset is, and used to
        // strand state there.
        //
        // The Animation panel: shown from the View menu with nothing open, it
        // holds no controls at all, and an edge trigger on "does it apply"
        // never fired on the following open because the answer stayed false --
        // so an empty dock stayed parked for the session.
        //
        // Reset Zoom: reachable by its shortcut with nothing open, where it
        // iterates no views. Reporting from inside the per-view reset meant it
        // reported nothing, and the button kept a factor nothing applied.
        const std::filesystem::path path(argv[2]);
        window.setAnimationDockVisibleForTest(true);
        window.selectFixedScaleForTest(4);
        // Checked before the reset, which would mask it: applyFixedScale only
        // touches currentViews(), and setFixedScale early-returns on a view
        // with no image, so with nothing open the factor reaches no view and
        // claiming it puts a number on the button nothing backs.
        if (window.scaleUiLabelForTest() != QStringLiteral("Fit")) {
            qCritical("picking 4x from the View menu with no dataset left the "
                      "button at '%s'",
                qUtf8Printable(window.scaleUiLabelForTest()));
            return 1;
        }
        // The toolbar menu is a separate call site with the same hazard.
        window.selectToolbarFixedScaleForTest(8);
        if (window.scaleUiLabelForTest() != QStringLiteral("Fit")) {
            qCritical("picking 8x from the toolbar with no dataset left the "
                      "button at '%s'",
                qUtf8Printable(window.scaleUiLabelForTest()));
            return 1;
        }
        window.resetZoomAllViewsForTest();
        if (window.scaleUiLabelForTest() != QStringLiteral("Fit")) {
            qCritical("Reset Zoom with no dataset left the button at '%s'",
                qUtf8Printable(window.scaleUiLabelForTest()));
            return 1;
        }
        window.setAnimationDockVisibleForTest(true);
        if (!window.animationDockVisibleForTest()) {
            qCritical("the Animation panel would not open with no dataset, so "
                      "this test proves nothing");
            return 1;
        }
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                // A 2-D plotfile: no sweep controls, no sequence, so the panel
                // has nothing to show and must not stay up.
                if (window.animationDockVisibleForTest()) {
                    qCritical("an empty Animation panel survived an open");
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--arrow-key-routing-smoke-test") {
        // The arrow keys pan the focused image view and nothing else. They
        // used to be window-context QShortcuts, which took Up/Down from every
        // toolbar spin box and combo -- Qt line edits claim Left/Right through
        // ShortcutOverride but not Up/Down, and non-editable combos claim no
        // arrows at all -- so a keyboard user stepping the level or a slice
        // position panned the image instead.
        //
        // Only a window-level test sees this. The ImageView unit test sends
        // its events to the view directly, which is the one delivery that
        // cannot tell a focused view from an unfocused one. Here the events go
        // to whatever holds focus, the way Qt delivers real key presses, so
        // the routing is the thing under test.
        const std::filesystem::path path(argv[2]);
        auto phase = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished, &application,
            [&window, &application, phase, path](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                const auto press = [&application](::Qt::Key key) {
                    auto* const target = QApplication::focusWidget();
                    if (target == nullptr) {
                        qCritical("no widget held focus");
                        application.exit(1);
                        return false;
                    }
                    QKeyEvent event(QEvent::KeyPress, key, ::Qt::NoModifier);
                    QApplication::sendEvent(target, &event);
                    return true;
                };
                const auto reopen = [&window, path] {
                    QTimer::singleShot(0, &window,
                        [&window, path] { window.openDataset(path); });
                };
                if (*phase == 0) {
                    *phase = 1;
                    // Scrollable, so a pan step has somewhere to go.
                    window.selectToolbarFixedScaleForTest(8);
                    // Precondition, not the property: a freshly shown window
                    // gives the view focus on its own. Phase 1 is where the
                    // open path's own focus handling is put to the question.
                    if (!window.activeViewHasFocusForTest()) {
                        qCritical("the view did not start focused");
                        application.exit(1);
                        return;
                    }
                    if (!press(::Qt::Key_Left) || !press(::Qt::Key_Up)) {
                        return;
                    }
                    if (window.panStepRequestsForTest() != 2) {
                        qCritical("arrow keys on the focused view produced %zu "
                                  "pan requests, expected 2",
                            window.panStepRequestsForTest());
                        application.exit(1);
                        return;
                    }
                    // The level combo. Up/Down belong to it -- this is the
                    // binding that used to be stolen -- and must not reach the
                    // view at all.
                    window.focusLevelSelectorForTest();
                    if (window.activeViewHasFocusForTest()) {
                        qCritical("the level selector did not take focus");
                        application.exit(1);
                        return;
                    }
                    if (!press(::Qt::Key_Up) || !press(::Qt::Key_Down)
                        || !press(::Qt::Key_Left) || !press(::Qt::Key_Right)) {
                        return;
                    }
                    if (window.panStepRequestsForTest() != 2) {
                        qCritical("an arrow key in the level selector reached the "
                                  "image view (%zu pan requests)",
                            window.panStepRequestsForTest());
                        application.exit(1);
                        return;
                    }
                    // Focus is nowhere in particular, the way it is when a file
                    // dialog closes. The open should claim it for the view, so
                    // the keys work without a click first.
                    window.clearFocusForTest();
                    reopen();
                    return;
                }
                if (*phase == 1) {
                    *phase = 2;
                    if (!window.activeViewHasFocusForTest()) {
                        qCritical("an open left the view unfocused, so the "
                                  "arrow keys need a click first");
                        application.exit(1);
                        return;
                    }
                    // ...but an open must not take focus away from a control
                    // the user is working in. This arrives from a watcher
                    // completion, which on a slow open lands long after the
                    // dialog closed and they moved on.
                    window.focusLevelSelectorForTest();
                    reopen();
                    return;
                }
                // Not necessarily the level selector by now -- teardown
                // disables it and Qt moves focus to a neighbouring control --
                // but it must not have landed in the view.
                if (window.activeViewHasFocusForTest()) {
                    qCritical("an open pulled focus into the view while a "
                              "control had it");
                    application.exit(1);
                    return;
                }
                application.exit(0);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--sequence-transform-preserve-smoke-test") {
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        auto before = std::make_shared<QRectF>();
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application, before](int index) {
                if (index == 0) {
                    window.wheelZoomAndPanActiveViewForTest();
                    *before = window.activeViewVisibleDataWindowForTest();
                    window.stepSequence(1);
                    return;
                }
                if (index != 1) {
                    return;
                }
                const auto after
                    = window.activeViewVisibleDataWindowForTest();
                const auto close = [](double lhs, double rhs) {
                    return std::fabs(lhs - rhs) <= 3.0e-2
                        * std::max({1.0, std::fabs(lhs), std::fabs(rhs)});
                };
                const bool preserved = !window.activeViewFitsWindowForTest()
                        && close(before->left(), after.left())
                        && close(before->top(), after.top())
                        && close(before->width(), after.width())
                        && close(before->height(), after.height());
                if (!preserved) {
                    std::fprintf(stderr,
                        "transform preservation mismatch: fit=%d "
                        "before=(%.17g,%.17g %.17gx%.17g) "
                        "after=(%.17g,%.17g %.17gx%.17g)\n",
                        window.activeViewFitsWindowForTest() ? 1 : 0,
                        before->x(), before->y(), before->width(),
                        before->height(), after.x(), after.y(), after.width(),
                        after.height());
                }
                application.exit(preserved ? 0 : 1);
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(2); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--sequence-density-preserve-smoke-test") {
        // Preserve a physical crop while moving from an 8x8 frame to an 8x12
        // frame. Pixel density changes, but the physical geometry is
        // compatible, so Custom mode must preserve the data window.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        auto zoomSettled = std::make_shared<bool>(false);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::interactiveSlicesSettled,
            &application, [&window, zoomSettled] {
                if (!*zoomSettled) {
                    *zoomSettled = true;
                    window.stepSequence(1);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application, zoomSettled](int index) {
                if (index == 0) {
                    window.rubberBandZoomActiveViewForTest();
                } else if (index == 1) {
                    application.exit(
                        *zoomSettled
                            && window.activeViewIsZoomedForTest()
                            && centeredHalfWindowPreserved(window)
                        ? 0 : 1);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--sequence-equal-size-transform-preserve-smoke-test") {
        // Exercise the timing edge directly: the first frame's full-domain
        // raster is 8x8. Rubber-band zoom changes the view transform and queues
        // a 4x4 crop, but stepping immediately cancels that work. The second
        // frame is 16x16, so its central-half crop is also 8x8. A size-only
        // transform policy must use physical compatibility rather than the
        // fresh dataset id or equal raster dimensions.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application](int index) {
                if (index == 0) {
                    window.rubberBandZoomActiveViewForTest();
                    window.stepSequence(1);
                } else if (index == 1) {
                    application.exit(
                        window.activeViewIsZoomedForTest()
                            && centeredHalfWindowPreserved(window)
                        ? 0 : 1);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 4
        && std::string_view(argv[1])
            == "--sequence-geometry-refit-smoke-test") {
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application](int index) {
                if (index == 0) {
                    window.wheelZoomAndPanActiveViewForTest();
                    window.stepSequence(1);
                } else if (index == 1) {
                    application.exit(
                        window.activeViewIsFitToWindowForTest() ? 0 : 1);
                }
            });
        QObject::connect(&window,
            &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(2); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--window-close-pool-smoke-test") {
        // Regression for window-close-clears-shared-thread-pool: opening and
        // closing a second window must not discard the first window's queued
        // work on the shared global pool (which would strand it on
        // "Loading..." forever). Constrain the pool to one thread and occupy
        // that thread with a gate runnable, so this window's initial-load
        // worker is genuinely queued (not running) when the second window
        // closes. The pre-fix closeEvent called QThreadPool::clear(), which
        // dropped that queued worker; the fix keeps clear() off the per-window
        // path, so the worker survives, runs once the gate releases, and the
        // load completes. A watchdog fails instead of hanging on a strand.
        const std::filesystem::path path(argv[2]);
        auto* pool = QThreadPool::globalInstance();
        pool->setMaxThreadCount(1);
        auto gate = std::make_shared<std::atomic<bool>>(false);
        pool->start(QRunnable::create([gate] {
            while (!gate->load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }));
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&application](bool success) {
                application.exit(success ? 0 : 1);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, path, gate] {
            // Queue this window's metadata/initial-load worker behind the gate.
            window.openDataset(path);
            // A second window, opened and closed while that worker is still
            // queued. Its closeEvent runs synchronously here, so the pre-fix
            // clear() would drop the queued worker before the gate releases.
            auto* second = new amrvis::qt::MainWindow;
            second->show();
            second->close();
            second->deleteLater();
            // Release the gate so the surviving worker (with the fix) can run.
            gate->store(true);
        });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--quit-smoke-test") {
        // Open a dataset, then quit through the main window once the initial
        // slice resolves (success or failure) and also mid-load. Passes if the
        // process exits promptly; a regression that blocks quit (an uncanceled
        // worker pinning the global pool, or a modal failure dialog) keeps it
        // alive until the watchdog fails the test.
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window](bool) {
                QTimer::singleShot(0, &window, [&window] { window.close(); });
            });
        QTimer::singleShot(300, &window, [&window] { window.close(); });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--export-quit-smoke-test") {
        // Open a two-frame sequence, start an animation export (bypassing the
        // interactive color-bar/save dialogs), and quit the instant FFmpeg
        // encoding begins. With a hung stand-in ffmpeg on PATH the encoder
        // workers block, so shutdown stays alive unless they are cancelled and
        // their process terminated on close; the ctest timeout fails the test
        // if the process never exits.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        const QString outputPath = QString::fromStdString(
            (first.parent_path() / "anim.png").string());
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, outputPath](int index) {
                if (index == 0) {
                    window.startAnimationExportForTest(outputPath, false);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::exportEncodingStarted,
            &application, [&window] {
                QTimer::singleShot(0, &window, [&window] { window.close(); });
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window, [&window, first, second] {
            window.openSequence({first, second});
        });
    }
#endif
    else if (argc >= 2 && !std::string_view(argv[1]).starts_with("--")) {
        // One or more plotfile paths: a single path opens a dataset, two or
        // more open a plotfile sequence (matching the GUI's Open Plotfile
        // Sequence, which also takes plotfile directories).
        std::vector<std::filesystem::path> paths;
        paths.reserve(static_cast<std::size_t>(argc - 1));
        for (int index = 1; index < argc; ++index) {
            paths.emplace_back(argv[index]);
        }
        QTimer::singleShot(0, &window, [&window, paths] {
            if (paths.size() == 1) {
                window.openDataset(paths.front());
            } else {
                window.openSequence(paths);
            }
        });
    } else if (argc >= 2) {
        // Anything starting with "--" that reached here matched no option, or
        // matched one with the wrong number of arguments. Both used to fall
        // through to an empty window with no diagnostic, which reads as the
        // option having been accepted and done nothing.
        std::fprintf(
            stderr, "amrexplorer: unrecognized option '%s'\n\n", argv[1]);
        printUsage(stderr);
        return 2;
    }
    const auto result = application.exec();
    if (smokeServer) {
        smokeServer->requestStop();
    }
    if (smokeServerThread) {
        smokeServerThread->join();
    }
    return result;
}
