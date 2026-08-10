#include "MainWindow.hpp"
#include "FabSelectorDock.hpp"
#include "RemoteConnectArguments.hpp"
#include "RemoteEndpoint.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QInputDialog>
#include <QLineEdit>
#include <QLoggingCategory>
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
#include <amrexplorer/remote/Frame.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace {

QtMessageHandler g_previousMessageHandler = nullptr;

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
void ensureDesktopEntry()
{
#ifndef Q_OS_LINUX
    // Desktop entry + hicolor icons are a GNOME/KDE (Linux) mechanism. On
    // other platforms the writes land in nonsensical locations and the
    // cache-refresh helpers do not exist, so do nothing.
    return;
#endif
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
            << "Exec=\"" << execPath << "\" %F\n"
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
    // Best-effort cache refresh. Detached processes inherit the terminal, so
    // route them through a shell that discards output (otherwise they print
    // "Cache file created successfully." on every install). Failures harmless.
    const auto runSilent = [](const QString& command) {
        QProcess::startDetached("sh",
            QStringList{"-c", command + " >/dev/null 2>&1"});
    };
    runSilent("gtk-update-icon-cache -f '" + hicolorDir + "'");
    runSilent("update-desktop-database '" + dataDir + "/applications'");
}

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

    QApplication application(argc, argv);
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
    std::optional<std::thread> smokePeerThread;
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
                    probe->localShowsWholeImage
                        = window.activeViewShowsWholeImageForTest();
                    probe->localImage = window.activeViewImageSizeForTest();
                    probe->localViewport
                        = window.activeViewViewportSizeForTest();
                    probe->localWindow
                        = window.activeViewVisibleDataWindowForTest();
                    probe->localViewportImage
                        = window.activeViewViewportImageForTest();
                    probe->phase = 1;
                    QTimer::singleShot(0, &window,
                        [&window, endpoint, token, remotePath] {
                            window.openRemoteDataset(endpoint.host,
                                endpoint.port, remotePath, token);
                        });
                    return;
                }
                const auto remoteShowsWholeImage
                    = window.activeViewShowsWholeImageForTest();
                const auto remoteImage = window.activeViewImageSizeForTest();
                const auto remoteViewport
                    = window.activeViewViewportSizeForTest();
                const auto remoteWindow
                    = window.activeViewVisibleDataWindowForTest();
                const auto remoteViewportImage
                    = window.activeViewViewportImageForTest();
                const auto printWindow = [](const QRectF& value) {
                    std::cout << std::setprecision(17)
                              << value.x() << ',' << value.y() << ','
                              << value.width() << ',' << value.height();
                };
                std::cout << "local 1x: image=" << probe->localImage[0]
                          << 'x' << probe->localImage[1]
                          << " viewport=" << probe->localViewport[0]
                          << 'x' << probe->localViewport[1]
                          << " whole-image-visible="
                          << (probe->localShowsWholeImage ? "yes" : "no")
                          << " data-window=";
                printWindow(probe->localWindow);
                std::cout << "\nremote 1x: image=" << remoteImage[0]
                          << 'x' << remoteImage[1]
                          << " viewport=" << remoteViewport[0]
                          << 'x' << remoteViewport[1]
                          << " whole-image-visible="
                          << (remoteShowsWholeImage ? "yes" : "no")
                          << " data-window=";
                printWindow(remoteWindow);
                std::cout << '\n';
                const auto fillsViewport = [](const std::array<int, 2>& image,
                                               const std::array<int, 2>& viewport) {
                    return image[0] <= viewport[0] && image[1] <= viewport[1]
                        && (std::abs(image[0] - viewport[0]) <= 1
                            || std::abs(image[1] - viewport[1]) <= 1);
                };
                const auto localActsFit = probe->localShowsWholeImage
                    && fillsViewport(probe->localImage, probe->localViewport);
                const auto remoteActsFit = remoteShowsWholeImage
                    && fillsViewport(remoteImage, remoteViewport);
                const auto scaleMatches = probe->localImage == remoteImage
                    && probe->localShowsWholeImage == remoteShowsWholeImage
                    && localActsFit == remoteActsFit;
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
                        std::cerr << "failed to save screenshot comparison to "
                                  << screenshotPath.toStdString() << '\n';
                        application.exit(5);
                        return;
                    }
                    std::cout << "screenshot="
                              << screenshotPath.toStdString() << '\n';
                }
                std::cout << (scaleMatches ? "MATCH" : "MISMATCH")
                          << ": remote 1x "
                          << (scaleMatches
                                ? "has the same native extent as local 1x"
                                : "does not have the same scale semantics as local 1x")
                          << '\n';
                application.exit(scaleMatches ? 0 : 1);
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
                window.openRemoteDataset(
                    "127.0.0.1", server->port(), path, server->token());
            });
    } else if (argc == 3
        && std::string_view(argv[1])
            == "--remote-fixed-scale-flicker-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        // Regression: in a window far too small for the whole domain at 32x,
        // the demand-driven fixed scale must settle with the viewport fully
        // backed by fetched raster, stay quiet with no input (the demand used
        // to re-issue itself endlessly through the as-needed scrollbars,
        // flickering through one remote render per flip), refetch exactly
        // once when the virtual scroll bars pan to unfetched cells, and drop
        // the domain-spanning scroll bars on a rubber-band zoom, whose
        // selection is re-rendered fitted to the pane exactly as for local
        // data.
        auto phase = std::make_shared<int>(0);
        auto settles = std::make_shared<int>(0);
        QObject::connect(&window,
            &amrvis::qt::MainWindow::initialSliceFinished,
            &application,
            [&window, &application, phase, settles](bool success) {
                if (!success) {
                    application.exit(2);
                    return;
                }
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application,
                    [&window, &application, phase, settles] {
                        ++*settles;
                        if (*phase == 0) {
                            *phase = 1;
                            if (!window.fixedScaleStateMatchesForTest(32)
                                || !window
                        .allViewsFixedScaleRasterCoversViewportForTest()) {
                                application.exit(1);
                                return;
                            }
                            // A quiet period several render round-trips long:
                            // any settle in here means the demand feeds back
                            // on itself.
                            const auto armed = *settles;
                            QTimer::singleShot(2000, &application,
                                [&window, &application, phase, settles,
                                    armed] {
                                    if (*settles != armed
                                        || !window
                        .allViewsFixedScaleRasterCoversViewportForTest()) {
                                        application.exit(1);
                                        return;
                                    }
                                    *phase = 2;
                                    // Five cells' worth of pixels at 32x,
                                    // sent through the real Shift+left mouse
                                    // event path: the newly visible cells must
                                    // be fetched, giving exactly one settle.
                                    window.shiftDragActiveViewForTest(-160, 0);
                                });
                            return;
                        }
                        if (*phase == 2) {
                            *phase = 3;
                            // The scrolled fixed scale keeps the fetched
                            // raster under the whole viewport, with the
                            // domain-spanning scroll bars present.
                            if (!window.fixedScaleStateMatchesForTest(32)
                                || !window
                        .allViewsFixedScaleRasterCoversViewportForTest()
                                || !window
                                    .activeViewScrollBarsVisibleForTest()) {
                                application.exit(1);
                                return;
                            }
                            window.rubberBandZoomActiveViewForTest();
                            return;
                        }
                        if (*phase == 3) {
                            *phase = 4;
                            // The re-rendered selection stands alone, fitted
                            // to the pane without scroll bars, as for local
                            // data.
                            application.exit(
                                window.activeViewIsZoomedForTest()
                                    && !window
                                        .activeViewScrollBarsVisibleForTest()
                                    ? 0 : 1);
                        }
                    });
                window.selectFixedScaleForTest(32);
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(4); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                window.resize(420, 301);
                window.openRemoteDataset(
                    "127.0.0.1", server->port(), path, server->token());
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
                window.openRemoteDataset(
                    "127.0.0.1", server->port(), path, server->token());
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
                window.openRemoteDataset(
                    "127.0.0.1", server->port(), path, server->token());
            });
    } else if (argc == 2
        && (std::string_view(argv[1])
                == "--remote-silent-hello-close-smoke-test"
            || std::string_view(argv[1])
                == "--remote-silent-verification-close-smoke-test")) {
        const bool verification
            = std::string_view(argv[1])
            == "--remote-silent-verification-close-smoke-test";
        auto listener = std::make_shared<amrvis::remote::Listener>(
            amrvis::remote::listenOnLoopback(0));
        smokePeerThread.emplace([listener, &window] {
            auto peer = amrvis::remote::acceptConnection(listener->socket);
            // Wait until the client has entered the hello transaction before
            // closing the window. A second read then remains silent until the
            // cancelled Connection constructor releases its socket.
            static_cast<void>(amrvis::remote::readFrame(peer));
            QMetaObject::invokeMethod(&window,
                [&window] { window.close(); }, Qt::QueuedConnection);
            static_cast<void>(amrvis::remote::readFrame(peer));
        });
        QTimer::singleShot(5000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, port = listener->port, verification] {
                if (verification) {
                    window.verifyRemoteEndpoint(
                        "127.0.0.1", port, "test-token");
                } else {
                    window.openRemoteDataset(
                        "127.0.0.1", port, "/not-opened-before-hello",
                        "test-token");
                }
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
                QObject::connect(&window,
                    &amrvis::qt::MainWindow::interactiveSlicesSettled,
                    &application, [&window, &application] {
                        application.exit(window.activeViewIsZoomedForTest()
                                && window.activeViewHasPhysicalAspectForTest(
                                    9.0 / 4.0)
                            ? 0 : 1);
                    }, Qt::SingleShotConnection);
                window.rubberBandZoomRectangularActiveViewForTest();
            });
        QTimer::singleShot(15000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                window.openRemoteDataset(
                    "127.0.0.1", server->port(), path, server->token());
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
                window.openRemoteDataset(
                    "127.0.0.1", server->port(), path, server->token());
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
                window.openRemoteSequence(
                    "127.0.0.1", server->port(), {path, path},
                    server->token());
            });
    } else if (argc >= 2
        && std::string_view(argv[1]) == "--connect") {
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(argc - 2));
        for (int index = 2; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }
        auto parsed
            = amrvis::qt::parseRemoteConnectArguments(arguments, std::cin);
        if (!parsed.request) {
            qCritical("%s", parsed.error.c_str());
            return 2;
        }
        QTimer::singleShot(0, &window,
            [&window, request = std::move(*parsed.request)] {
                if (request.paths.size() == 1) {
                    window.openRemoteDataset(request.endpoint.host,
                        request.endpoint.port, request.paths.front(),
                        request.endpoint.token);
                } else {
                    window.openRemoteSequence(request.endpoint.host,
                        request.endpoint.port, request.paths,
                        request.endpoint.token);
                }
            });
    } else if (argc == 3 && std::string_view(argv[1]) == "--smoke-test") {
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::datasetOpenFinished,
            &application, [&application](bool success) {
                application.exit(success ? 0 : 1);
            });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path, true); });
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
                window.openRemoteSequence("127.0.0.1", server->port(),
                    {first, second}, server->token());
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
                            && !window.activeViewIsFitToWindowForTest()
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
                            && !window.activeViewIsFitToWindowForTest()
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
    } else if (argc >= 2 && !std::string_view(argv[1]).starts_with("--")) {
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
    }
    const auto result = application.exec();
    if (smokeServer) {
        smokeServer->requestStop();
    }
    if (smokeServerThread) {
        smokeServerThread->join();
    }
    if (smokePeerThread) {
        smokePeerThread->join();
    }
    return result;
}
