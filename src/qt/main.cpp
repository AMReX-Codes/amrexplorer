#include "MainWindow.hpp"
#include "AnimationPanel.hpp"
#include "FabSelectorDock.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableView>
#include <QTextStream>
#include <QTemporaryDir>
#include <QTimer>

#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
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

bool exerciseExpressionEditor(amrvis::qt::MainWindow& window)
{
    auto* fieldSelector = window.findChild<QComboBox*>(
        QStringLiteral("fieldSelector"));
    if (fieldSelector == nullptr) {
        return false;
    }
    const auto nativeFieldIndex =
        fieldSelector->findText(QStringLiteral("temperature"));
    if (nativeFieldIndex < 0) {
        return false;
    }
    fieldSelector->setCurrentIndex(nativeFieldIndex);
    const auto selectedNativeField = fieldSelector->currentText();

    const auto edit = [&](const auto& interaction) {
        auto* action = window.findChild<QAction*>(
            QStringLiteral("expressionEditorAction"));
        if (action == nullptr) {
            return false;
        }
        bool completed = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(
                QStringLiteral("expressionEditor"));
            if (dialog == nullptr) {
                return;
            }
            completed = interaction(*dialog);
            if (!completed) {
                dialog->reject();
            }
        });
        action->trigger();
        return completed;
    };

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        return false;
    }
    const auto expressionListPath = temporary.filePath(QStringLiteral("expressions.json"));

    const auto created = edit([&expressionListPath](QDialog& dialog) {
        auto* add = dialog.findChild<QPushButton*>(
            QStringLiteral("newExpressionButton"));
        auto* remove = dialog.findChild<QPushButton*>(
            QStringLiteral("deleteExpressionButton"));
        auto* importDefinitions =
            dialog.findChild<QPushButton*>(QStringLiteral("importExpressionsButton"));
        auto* exportDefinitions =
            dialog.findChild<QPushButton*>(QStringLiteral("exportExpressionsButton"));
        auto* list = dialog.findChild<QListWidget*>(
            QStringLiteral("expressionList"));
        auto* name = dialog.findChild<QLineEdit*>(
            QStringLiteral("expressionName"));
        auto* source = dialog.findChild<QPlainTextEdit*>(
            QStringLiteral("expressionSource"));
        auto* apply = dialog.findChild<QPushButton*>(
            QStringLiteral("applyExpressionsButton"));
        if (add == nullptr || remove == nullptr || importDefinitions == nullptr ||
            exportDefinitions == nullptr || list == nullptr || name == nullptr ||
            source == nullptr || apply == nullptr) {
            return false;
        }
        add->click();
        name->setText(QStringLiteral("twice-density"));
        source->setPlainText(QStringLiteral("2*density"));

        bool exportCompleted = false;
        QTimer::singleShot(0, &dialog, [&] {
            auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            if (picker == nullptr) {
                return;
            }
            picker->selectFile(expressionListPath);
            exportCompleted = true;
            static_cast<QDialog*>(picker)->accept();
        });
        exportDefinitions->click();
        if (!exportCompleted || !QFileInfo::exists(expressionListPath)) {
            return false;
        }

        remove->click();
        if (list->count() != 0) {
            return false;
        }
        bool importCompleted = false;
        QTimer::singleShot(0, &dialog, [&] {
            auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            if (picker == nullptr) {
                return;
            }
            picker->selectFile(expressionListPath);
            importCompleted = true;
            static_cast<QDialog*>(picker)->accept();
        });
        importDefinitions->click();
        if (!importCompleted || list->count() != 1 ||
            name->text() != QStringLiteral("twice-density") ||
            source->toPlainText() != QStringLiteral("2*density")) {
            return false;
        }

        apply->click();
        return true;
    });
    if (!created || fieldSelector->findText(
            QStringLiteral("twice-density")) < 0
        || fieldSelector->currentText() != selectedNativeField) {
        return false;
    }

    fieldSelector->setCurrentIndex(
        fieldSelector->findText(QStringLiteral("twice-density")));
    const auto edited = edit([](QDialog& dialog) {
        auto* list = dialog.findChild<QListWidget*>(
            QStringLiteral("expressionList"));
        auto* name = dialog.findChild<QLineEdit*>(
            QStringLiteral("expressionName"));
        auto* source = dialog.findChild<QPlainTextEdit*>(
            QStringLiteral("expressionSource"));
        auto* apply = dialog.findChild<QPushButton*>(
            QStringLiteral("applyExpressionsButton"));
        if (list == nullptr || list->count() != 1 || name == nullptr
            || source == nullptr || apply == nullptr) {
            return false;
        }
        list->setCurrentRow(0);
        name->setText(QStringLiteral("triple-density"));
        source->setPlainText(QStringLiteral("3*density"));
        apply->click();
        return true;
    });
    if (!edited || fieldSelector->findText(
            QStringLiteral("twice-density")) >= 0
        || fieldSelector->findText(QStringLiteral("triple-density")) < 0
        || fieldSelector->currentText() != QStringLiteral("triple-density")) {
        return false;
    }

    const auto addedUnrelated = edit([](QDialog& dialog) {
        auto* add = dialog.findChild<QPushButton*>(
            QStringLiteral("newExpressionButton"));
        auto* name = dialog.findChild<QLineEdit*>(
            QStringLiteral("expressionName"));
        auto* source = dialog.findChild<QPlainTextEdit*>(
            QStringLiteral("expressionSource"));
        auto* apply = dialog.findChild<QPushButton*>(
            QStringLiteral("applyExpressionsButton"));
        if (add == nullptr || name == nullptr || source == nullptr
            || apply == nullptr) {
            return false;
        }
        add->click();
        name->setText(QStringLiteral("four-density"));
        source->setPlainText(QStringLiteral("4*density"));
        apply->click();
        return true;
    });
    if (!addedUnrelated
        || fieldSelector->findText(QStringLiteral("four-density")) < 0
        || fieldSelector->currentText() != QStringLiteral("triple-density")) {
        return false;
    }

    const auto editedUnrelated = edit([](QDialog& dialog) {
        auto* list = dialog.findChild<QListWidget*>(
            QStringLiteral("expressionList"));
        auto* source = dialog.findChild<QPlainTextEdit*>(
            QStringLiteral("expressionSource"));
        auto* apply = dialog.findChild<QPushButton*>(
            QStringLiteral("applyExpressionsButton"));
        if (list == nullptr || list->count() != 2 || source == nullptr
            || apply == nullptr) {
            return false;
        }
        list->setCurrentRow(1);
        source->setPlainText(QStringLiteral("5*density"));
        apply->click();
        return true;
    });
    if (!editedUnrelated
        || fieldSelector->currentText() != QStringLiteral("triple-density")) {
        return false;
    }

    const auto deleted = edit([](QDialog& dialog) {
        auto* list = dialog.findChild<QListWidget*>(
            QStringLiteral("expressionList"));
        auto* remove = dialog.findChild<QPushButton*>(
            QStringLiteral("deleteExpressionButton"));
        auto* apply = dialog.findChild<QPushButton*>(
            QStringLiteral("applyExpressionsButton"));
        if (list == nullptr || list->count() != 2 || remove == nullptr
            || apply == nullptr) {
            return false;
        }
        list->setCurrentRow(1);
        remove->click();
        list->setCurrentRow(0);
        remove->click();
        apply->click();
        return true;
    });
    return deleted
        && fieldSelector->findText(QStringLiteral("triple-density")) < 0
        && fieldSelector->findText(QStringLiteral("four-density")) < 0;
}

bool applyExpressionDefinition(amrvis::qt::MainWindow& window,
    const QString& fieldName, const QString& parserExpression,
    bool expectPlaybackPaused = false)
{
    auto* action = window.findChild<QAction*>(
        QStringLiteral("expressionEditorAction"));
    if (action == nullptr) {
        return false;
    }
    bool completed = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(
            QStringLiteral("expressionEditor"));
        if (dialog == nullptr) {
            return;
        }
        auto* add = dialog->findChild<QPushButton*>(
            QStringLiteral("newExpressionButton"));
        auto* list = dialog->findChild<QListWidget*>(
            QStringLiteral("expressionList"));
        auto* name = dialog->findChild<QLineEdit*>(
            QStringLiteral("expressionName"));
        auto* source = dialog->findChild<QPlainTextEdit*>(
            QStringLiteral("expressionSource"));
        auto* apply = dialog->findChild<QPushButton*>(
            QStringLiteral("applyExpressionsButton"));
        auto* playbackTimer = window.findChild<QTimer*>(
            QStringLiteral("playbackTimer"));
        if (add == nullptr || list == nullptr || name == nullptr
            || source == nullptr || apply == nullptr
            || (expectPlaybackPaused
                && (playbackTimer == nullptr || playbackTimer->isActive()))) {
            dialog->reject();
            return;
        }
        if (list->count() == 0) {
            add->click();
        } else {
            list->setCurrentRow(0);
        }
        name->setText(fieldName);
        source->setPlainText(parserExpression);
        completed = true;
        apply->click();
    });
    action->trigger();
    return completed;
}

bool applyExpressionDefinitions(amrvis::qt::MainWindow& window,
    const std::vector<std::pair<QString, QString>>& definitions)
{
    auto* action = window.findChild<QAction*>(
        QStringLiteral("expressionEditorAction"));
    if (action == nullptr) {
        return false;
    }
    bool completed = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(
            QStringLiteral("expressionEditor"));
        if (dialog == nullptr) {
            return;
        }
        auto* add = dialog->findChild<QPushButton*>(
            QStringLiteral("newExpressionButton"));
        auto* list = dialog->findChild<QListWidget*>(
            QStringLiteral("expressionList"));
        auto* name = dialog->findChild<QLineEdit*>(
            QStringLiteral("expressionName"));
        auto* source = dialog->findChild<QPlainTextEdit*>(
            QStringLiteral("expressionSource"));
        auto* apply = dialog->findChild<QPushButton*>(
            QStringLiteral("applyExpressionsButton"));
        if (add == nullptr || list == nullptr || name == nullptr
            || source == nullptr || apply == nullptr || list->count() != 0) {
            dialog->reject();
            return;
        }
        for (const auto& [fieldName, parserExpression] : definitions) {
            add->click();
            name->setText(fieldName);
            source->setPlainText(parserExpression);
        }
        completed = true;
        apply->click();
    });
    action->trigger();
    return completed;
}

bool expressionDefinitionMatches(amrvis::qt::MainWindow& window,
    const QString& fieldName, const QString& parserExpression)
{
    auto* action = window.findChild<QAction*>(
        QStringLiteral("expressionEditorAction"));
    if (action == nullptr) {
        return false;
    }
    bool matches = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(
            QStringLiteral("expressionEditor"));
        if (dialog == nullptr) {
            return;
        }
        auto* list = dialog->findChild<QListWidget*>(
            QStringLiteral("expressionList"));
        auto* name = dialog->findChild<QLineEdit*>(
            QStringLiteral("expressionName"));
        auto* source = dialog->findChild<QPlainTextEdit*>(
            QStringLiteral("expressionSource"));
        if (list != nullptr && list->count() == 1
            && name != nullptr && source != nullptr) {
            list->setCurrentRow(0);
            matches = name->text() == fieldName
                && source->toPlainText() == parserExpression;
        }
        dialog->reject();
    });
    action->trigger();
    return matches;
}

bool expressionEditorMatchesInstalledFrame(amrvis::qt::MainWindow& window)
{
    auto* action = window.findChild<QAction*>(
        QStringLiteral("expressionEditorAction"));
    if (action == nullptr) {
        return false;
    }
    bool matches = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(
            QStringLiteral("expressionEditor"));
        if (dialog == nullptr) {
            return;
        }
        const auto* list = dialog->findChild<QListWidget*>(
            QStringLiteral("expressionList"));
        const auto* help = dialog->findChild<QLabel*>(
            QStringLiteral("expressionHelp"));
        matches = list != nullptr && list->count() == 2
            && list->item(0)->text() == QStringLiteral("derived-b")
            && list->item(1)->text() == QStringLiteral("derived-c")
            && help != nullptr
            && help->text().contains(QStringLiteral("temperature"));
        dialog->reject();
    });
    action->trigger();
    return matches;
}

bool setUserRange(amrvis::qt::MainWindow& window,
    double minimum, double maximum)
{
    auto* mode = window.findChild<QComboBox*>(
        QStringLiteral("rangeModeSelector"));
    auto* lower = window.findChild<QDoubleSpinBox*>(
        QStringLiteral("rangeMinimum"));
    auto* upper = window.findChild<QDoubleSpinBox*>(
        QStringLiteral("rangeMaximum"));
    if (mode == nullptr || lower == nullptr || upper == nullptr) {
        return false;
    }
    const auto userIndex = mode->findData(
        static_cast<int>(amrvis::qt::RangeMode::User));
    if (userIndex < 0) {
        return false;
    }
    mode->setCurrentIndex(userIndex);
    lower->setValue(minimum);
    upper->setValue(maximum);
    return true;
}

bool visibleRangeMatches(
    const amrvis::qt::MainWindow& window, double minimum, double maximum)
{
    const auto* lower = window.findChild<QDoubleSpinBox*>(
        QStringLiteral("rangeMinimum"));
    const auto* upper = window.findChild<QDoubleSpinBox*>(
        QStringLiteral("rangeMaximum"));
    constexpr auto tolerance = 1.0e-6;
    return lower != nullptr && upper != nullptr
        && std::abs(lower->value() - minimum) < tolerance
        && std::abs(upper->value() - maximum) < tolerance;
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
        && std::string_view(argv[1]) == "--expression-editor-smoke-test") {
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application](bool success) {
                const auto valid = success && exerciseExpressionEditor(window);
                application.exit(valid ? 0 : 1);
            });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--expression-range-smoke-test") {
        struct SmokeState {
            int phase = 0;
            int completedSlices = 0;
        };
        const auto state = std::make_shared<SmokeState>();
        const std::filesystem::path path(argv[2]);
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&window, &application, state](bool success) {
                auto* animationPanel =
                    window.findChild<amrvis::qt::AnimationPanel*>();
                auto* playbackTimer = window.findChild<QTimer*>(
                    QStringLiteral("playbackTimer"));
                if (!success || animationPanel == nullptr
                    || playbackTimer == nullptr) {
                    application.exit(1);
                    return;
                }
                animationPanel->setSpeedValue(600);
                animationPanel->sweepPlayToggled();
                if (!playbackTimer->isActive()
                    || !applyExpressionDefinition(window,
                        QStringLiteral("scaled-q"),
                        QStringLiteral("2*q"), true)
                    || !playbackTimer->isActive()) {
                    application.exit(1);
                    return;
                }
                auto* fieldSelector = window.findChild<QComboBox*>(
                    QStringLiteral("fieldSelector"));
                const auto scaledIndex = fieldSelector == nullptr
                    ? -1
                    : fieldSelector->findText(QStringLiteral("scaled-q"));
                if (scaledIndex < 0) {
                    application.exit(1);
                    return;
                }
                fieldSelector->setCurrentIndex(scaledIndex);
                animationPanel->sweepPlayToggled();
                if (playbackTimer->isActive()) {
                    application.exit(1);
                    return;
                }
                state->phase = 1;
                state->completedSlices = 0;
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::sliceRequestFinished,
            &application, [&window, &application, state] {
                if (state->phase == 0 || ++state->completedSlices < 3) {
                    return;
                }
                if (state->phase == 1) {
                    state->phase = 2;
                    state->completedSlices = 0;
                    if (!applyExpressionDefinition(window,
                            QStringLiteral("scaled-q"),
                            QStringLiteral("3*q"))) {
                        application.exit(1);
                    }
                    return;
                }
                const auto valid = visibleRangeMatches(
                    window, 2.0 / 3.0, 8.0 / 3.0);
                application.exit(valid ? 0 : 1);
            });
        QTimer::singleShot(20000, &application,
            [&application] { application.exit(1); });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
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
                    || !fabSelectorColumnsMatch(*selector, true)) {
                    application.exit(1);
                    return;
                }
                if (phase == 0) {
                    if (!applyExpressionDefinition(window,
                            QStringLiteral("scaled"),
                            QStringLiteral("2*MultiFab_0"))
                        || !fabSelectorPointFilterMatches(*selector, true)) {
                        application.exit(1);
                        return;
                    }
                    ++phase;
                } else if (phase == 1) {
                    auto* back = selector->findChild<QPushButton*>(
                        QStringLiteral("fabBackButton"));
                    if (back == nullptr || !back->isVisible()
                        || !fabRangeSelectorMatches(window)
                        || !expressionDefinitionMatches(window,
                            QStringLiteral("scaled"),
                            QStringLiteral("2*MultiFab_0"))
                        || !applyExpressionDefinition(window,
                            QStringLiteral("scaled"),
                            QStringLiteral("3*MultiFab_0"))
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
                        back != nullptr && !back->isVisible()
                            && expressionDefinitionMatches(window,
                                QStringLiteral("scaled"),
                                QStringLiteral("3*MultiFab_0"))
                            ? 0 : 1);
                }
            });
        QTimer::singleShot(0, &window,
            [&window, path] { window.openDataset(path); });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--sequence-smoke-test") {
        // The second fixture renames density, so the first derived definition
        // is skipped there. The selected later definition must still be
        // restored by name after the remaining derived IDs compact.
        const std::filesystem::path first(argv[2]);
        const std::filesystem::path second(argv[3]);
        QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
            &application, [&window, &application](int index) {
                if (index == 0) {
                    auto* fieldSelector = window.findChild<QComboBox*>(
                        QStringLiteral("fieldSelector"));
                    if (fieldSelector == nullptr
                        || !applyExpressionDefinitions(window, {
                            {QStringLiteral("derived-a"),
                                QStringLiteral("2*density")},
                            {QStringLiteral("derived-b"),
                                QStringLiteral("3*temperature")},
                            {QStringLiteral("derived-c"),
                                QStringLiteral("4*temperature")}})
                        || fieldSelector->findText(
                            QStringLiteral("derived-b")) < 0) {
                        application.exit(1);
                        return;
                    }
                    fieldSelector->setCurrentIndex(fieldSelector->findText(
                        QStringLiteral("derived-b")));
                    if (!setUserRange(window, 10.0, 20.0)) {
                        application.exit(1);
                        return;
                    }
                    fieldSelector->setCurrentIndex(fieldSelector->findText(
                        QStringLiteral("derived-c")));
                    if (!setUserRange(window, 30.0, 40.0)) {
                        application.exit(1);
                        return;
                    }
                    fieldSelector->setCurrentIndex(fieldSelector->findText(
                        QStringLiteral("derived-b")));
                    window.stepSequence(1);
                } else if (index == 1) {
                    auto* fieldSelector = window.findChild<QComboBox*>(
                        QStringLiteral("fieldSelector"));
                    const auto selectedRangeMatches = fieldSelector != nullptr
                        && fieldSelector->currentText()
                            == QStringLiteral("derived-b")
                        && visibleRangeMatches(window, 10.0, 20.0);
                    if (!selectedRangeMatches) {
                        application.exit(1);
                        return;
                    }
                    fieldSelector->setCurrentIndex(fieldSelector->findText(
                        QStringLiteral("derived-c")));
                    application.exit(
                        visibleRangeMatches(window, 30.0, 40.0)
                            && expressionEditorMatchesInstalledFrame(window)
                        ? 0 : 1);
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
