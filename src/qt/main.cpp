#include "MainWindow.hpp"
#include "SshConnectArguments.hpp"

#include <amrexplorer/core/Version.hpp>
#ifdef AMREXPLORER_QT_TEST_ACCESS
#include "SmokeHarness.hpp"
#endif

#include <QApplication>
#include <QDialog>
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
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>

namespace {

QtMessageHandler g_previousMessageHandler = nullptr;

void printUsage(std::FILE* output)
{
    std::fprintf(output,
        "usage: amrexplorer [PLOTFILE...]\n"
        "       amrexplorer --ssh SSH_DESTINATION [--server PATH] [--] "
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
        "  -h, --help             show this help\n"
        "  -v, --version          show the version\n\n"
        "See docs/user-guide.md for details.\n");
}


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
    // Answered before QApplication exists, so they work without a display.
    if (argc >= 2
        && (std::string_view(argv[1]) == "--help"
            || std::string_view(argv[1]) == "-h")) {
        printUsage(stdout);
        return 0;
    }
    if (argc >= 2
        && (std::string_view(argv[1]) == "--version"
            || std::string_view(argv[1]) == "-v")) {
        std::printf("amrexplorer %s\n", amrvis::versionText().c_str());
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
#ifdef AMREXPLORER_QT_TEST_ACCESS
    // Before the window, whose constructor restores settings.
    amrvis::qt::smoke::isolateSettings(argc, argv);
#endif
    amrvis::qt::MainWindow window;
    window.show();
    // The smoke-test harnesses (SmokeHarness*.cpp) claim their options first;
    // they compile only where MainWindow's ForTest accessors do, and the
    // release binary carries neither. Their options and the production ones
    // below are disjoint, so the order carries no meaning beyond that.
    bool armedBySmokeHarness = false;
#ifdef AMREXPLORER_QT_TEST_ACCESS
    amrvis::qt::smoke::Context smoke{application, window, argc, argv, {}, {}};
    const auto smokeOutcome = amrvis::qt::smoke::dispatch(smoke);
    if (smokeOutcome.exitCode) {
        // No scenario starts its server before exiting this way; the call is
        // for the header's promise, not a known leak.
        amrvis::qt::smoke::shutdown(smoke);
        return *smokeOutcome.exitCode;
    }
    armedBySmokeHarness = smokeOutcome.handled;
#endif
    if (armedBySmokeHarness) {
        // The scenario's timers and connections are set; exec() runs it.
    } else if (argc >= 2 && std::string_view(argv[1]) == "--ssh") {
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
    } else if (argc >= 2 && !std::string_view(argv[1]).starts_with("-")) {
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
        // Anything starting with "-" that reached here matched no option, or
        // matched one with the wrong number of arguments. Both used to fall
        // through to an empty window with no diagnostic, which reads as the
        // option having been accepted and done nothing. A single dash counts:
        // now that -h and -v exist, a mistyped one would otherwise be taken
        // for a plotfile path and reported as an unopenable dataset.
        std::fprintf(
            stderr, "amrexplorer: unrecognized option '%s'\n\n", argv[1]);
        printUsage(stderr);
        return 2;
    }
    const auto result = application.exec();
#ifdef AMREXPLORER_QT_TEST_ACCESS
    amrvis::qt::smoke::shutdown(smoke);
#endif
    return result;
}
