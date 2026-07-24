#include "MainWindow.hpp"
#include "FabSelectorDock.hpp"

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
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableView>
#include <QTextStream>
#include <QTimer>

#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

namespace {

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
            continue;  // already installed; skip the no-op rewrite
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

} // namespace

int main(int argc, char* argv[])
{
    // Disable Wayland warnings
    QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.wayland.textinput=false"));

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
    if (argc == 3 && std::string_view(argv[1]) == "--smoke-test") {
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
        && std::string_view(argv[1]) == "--raw-fab-smoke-test") {
        const std::filesystem::path path(argv[2]);
        int phase = 0;
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, &phase](bool success) {
                auto* selector =
                    window.findChild<amrvis::qt::FabSelectorDock*>();
                const auto valid = success && selector != nullptr
                    && selector->isVisible() && selector->entries().size() >= 2
                    && fabSelectorIsAscending(*selector)
                    && fabSelectorColumnsMatch(*selector, false)
                    && fabSelectorPointFilterMatches(*selector, phase == 0)
                    && fabRangeSelectorMatches(window);
                if (!valid) {
                    application.exit(1);
                } else if (phase++ == 0) {
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
        int phase = 0;
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, &phase](bool success) {
                auto* selector =
                    window.findChild<amrvis::qt::FabSelectorDock*>();
                if (!success || selector == nullptr
                    || selector->entries().size() < 2
                    || !fabSelectorIsAscending(*selector)
                    || !fabSelectorColumnsMatch(*selector, true)
                    || !fabSelectorPointFilterMatches(
                        *selector, phase == 0)) {
                    application.exit(1);
                    return;
                }
                if (phase == 0) {
                    ++phase;
                } else if (phase == 1) {
                    auto* back = selector->findChild<QPushButton*>(
                        QStringLiteral("fabBackButton"));
                    if (back == nullptr || !back->isVisible()
                        || !fabRangeSelectorMatches(window)
                        || !clearFabSelectorPointFilter(*selector)) {
                        application.exit(1);
                        return;
                    }
                    ++phase;
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
                    window.stepSequence(1);
                } else if (index == 1) {
                    application.exit(0);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameFailed,
            &application, [&application] { application.exit(1); });
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
    return application.exec();
}
