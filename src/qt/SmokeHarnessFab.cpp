#include "SmokeHarnessInternal.hpp"

#include "FabSelectorDock.hpp"
#include "MainWindow.hpp"

#include <QApplication>
#include <QComboBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QRunnable>
#include <QTableView>
#include <QThreadPool>
#include <QTimer>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

// Fab: the FAB scenarios: raw FAB and MultiFab open, the FAB selector,
// overlap and direct-open failures, FAB zoom. Every branch drives MainWindow
// through its ForTest accessors and arms connections and timers for main()
// to run; see SmokeHarness.hpp.

namespace amrvis::qt::smoke {

namespace {

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

Outcome dispatchFab(Context& context)
{
    // The branches read these names as main() declared them; binding them
    // here keeps the moved code verbatim.
    auto& application = context.application;
    auto& window = context.window;
    const int argc = context.argc;
    char** argv = context.argv;

    if (argc == 3
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
    }
    else {
        return {false, std::nullopt};
    }
    return {true, std::nullopt};
}

} // namespace amrvis::qt::smoke
