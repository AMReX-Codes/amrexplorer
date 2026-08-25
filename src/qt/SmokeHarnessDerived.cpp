#include "SmokeHarnessInternal.hpp"

#include "DerivedFieldStore.hpp"
#include "ExpressionEditorDialog.hpp"
#include "MainWindow.hpp"

#include <amrexplorer/remote/Server.hpp>
#include "RangeController.hpp"

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

// Derived fields: the Variable menu's Expression Editor over a local
// plotfile, driven through the real action, the real dialog widgets and the
// real Apply, which reopens the dataset with the definition installed. What
// this covers and the data-layer tests cannot: that the action is reachable
// and enabled, that a refused definition is reported in the dialog and
// changes nothing, and that an accepted one reaches the field selector and
// the Variable menu and renders when selected. The values themselves are
// test_derived_field_query's business.

namespace amrvis::qt::smoke {

namespace {

QStringList fieldNames(const amrvis::qt::MainWindow& window)
{
    QStringList names;
    const auto* selector =
        window.findChild<QComboBox*>(QStringLiteral("fieldSelector"));
    if (selector == nullptr) {
        return names;
    }
    for (int index = 0; index < selector->count(); ++index) {
        // Fields carry their id as item data; the separator between the stored
        // and the derived ones carries none.
        if (!selector->itemData(index).isValid()) {
            continue;
        }
        names.append(selector->itemText(index));
    }
    return names;
}

// Fills the dialog's editor with one definition, replacing whatever is there:
// New, then the name and expression through the same widgets a user types in.
bool writeDefinition(amrvis::qt::ExpressionEditorDialog& dialog,
    const QString& name, const QString& expression)
{
    auto* add =
        dialog.findChild<QPushButton*>(QStringLiteral("newExpressionButton"));
    auto* nameEdit =
        dialog.findChild<QLineEdit*>(QStringLiteral("expressionName"));
    auto* source =
        dialog.findChild<QPlainTextEdit*>(QStringLiteral("expressionSource"));
    if (add == nullptr || nameEdit == nullptr || source == nullptr) {
        return false;
    }
    if (dialog.draft().empty()) {
        add->click();
    }
    nameEdit->setText(name);
    source->setPlainText(expression);
    return dialog.draft().size() == 1
        && dialog.draft().front().name == name.toStdString()
        && dialog.draft().front().expression == expression.toStdString();
}

// Adds a further definition rather than replacing the first.
bool appendDefinition(amrvis::qt::ExpressionEditorDialog& dialog,
    const QString& name, const QString& expression)
{
    auto* add =
        dialog.findChild<QPushButton*>(QStringLiteral("newExpressionButton"));
    auto* nameEdit =
        dialog.findChild<QLineEdit*>(QStringLiteral("expressionName"));
    auto* source =
        dialog.findChild<QPlainTextEdit*>(QStringLiteral("expressionSource"));
    if (add == nullptr || nameEdit == nullptr || source == nullptr) {
        return false;
    }
    const auto before = dialog.draft().size();
    add->click();
    nameEdit->setText(name);
    source->setPlainText(expression);
    return dialog.draft().size() == before + 1
        && dialog.draft().back().name == name.toStdString();
}

bool clickApply(amrvis::qt::ExpressionEditorDialog& dialog)
{
    auto* apply = dialog.findChild<QPushButton*>(
        QStringLiteral("applyExpressionsButton"));
    if (apply == nullptr) {
        return false;
    }
    apply->click();
    return true;
}

bool errorShown(const amrvis::qt::ExpressionEditorDialog& dialog)
{
    const auto* error =
        dialog.findChild<QLabel*>(QStringLiteral("expressionError"));
    return error != nullptr && error->isVisible() && !error->text().isEmpty();
}

// Arms the range-memory scenario: a User range set on one field must still be
// there when the user comes back to it. The memory is keyed by field name, and
// the name it is filed under comes from the host -- so the host has to tell the
// controller which field the widgets represent on *every* path that selects
// one, the plain open included.
void armRangeMemoryChecks(
    amrvis::qt::MainWindow& window, QApplication& application)
{
    auto phase = std::make_shared<int>(0);
    auto loaded = std::make_shared<bool>(false);
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&application, loaded](bool success) {
            if (!success) {
                application.exit(2);
                return;
            }
            *loaded = true;
        });

    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, loaded, timer] {
            const auto finish = [&application, timer](int code) {
                timer->stop();
                application.exit(code);
            };
            if (!*loaded || window.sliceRequestPendingForTest()
                || window.slicesInFlightForTest() != 0) {
                return;
            }
            auto* selector = window.findChild<QComboBox*>(
                QStringLiteral("fieldSelector"));
            auto* mode = window.findChild<QComboBox*>(
                QStringLiteral("rangeModeSelector"));
            // The two range bounds, found through the toolbar that holds the
            // mode selector: ScientificDoubleSpinBox has no Q_OBJECT of its
            // own, and other spin boxes live elsewhere in the window.
            const auto bounds = mode == nullptr
                ? QList<QDoubleSpinBox*>{}
                : mode->parentWidget()->findChildren<QDoubleSpinBox*>();
            if (selector == nullptr || mode == nullptr || bounds.size() < 2
                || selector->count() < 2) {
                qCritical("the range widgets or a second field are missing");
                finish(3);
                return;
            }
            if (*phase == 0) {
                const auto user = mode->findData(
                    static_cast<int>(amrvis::qt::RangeMode::User));
                if (user < 0) {
                    qCritical("the range mode selector has no User entry");
                    finish(4);
                    return;
                }
                mode->setCurrentIndex(user);
                bounds[0]->setValue(10.0);
                bounds[1]->setValue(20.0);
                *phase = 1;
                return;
            }
            if (*phase == 1) {
                selector->setCurrentIndex(1);  // another field
                *phase = 2;
                return;
            }
            if (*phase == 2) {
                selector->setCurrentIndex(0);  // and back to the first
                *phase = 3;
                return;
            }
            if (*phase == 3) {
                const auto restored = mode->currentData().toInt()
                    == static_cast<int>(amrvis::qt::RangeMode::User)
                    && bounds[0]->value() == 10.0
                    && bounds[1]->value() == 20.0;
                if (!restored) {
                    qCritical("the first field's User range did not come back "
                              "(mode %d, %g..%g)",
                        mode->currentData().toInt(), bounds[0]->value(),
                        bounds[1]->value());
                    finish(5);
                    return;
                }
                finish(0);
            }
        });
    timer->start();
    QTimer::singleShot(30000, &application, [&application] { application.exit(13); });
}

// Arms the one ordering that reading this code kept missing: an interaction
// that lands while a reload is still replacing the session. The reload skips
// its own display for that view, because a newer request for it exists, and
// the interaction's own raster came from the session being replaced -- so
// unless something re-slices, the window keeps the outgoing session's pixels
// while its catalog, field list and colour bar are the new session's. Exit 0
// once the raster on screen and the installed session agree again.
// `dispatch` decides how the injected interaction reaches the slice pipeline.
// True submits for the view there and then, which is the ordering an
// arrival-side check cannot sort out: the view is showing a raster the
// outgoing session produced. False leaves the request queued behind the
// debounce, as a real click does -- sliceGeneration has not moved when the
// reload completes, so only the queue says the user has been here.
void armReloadRaceChecks(amrvis::qt::MainWindow& window,
    QApplication& application, bool dispatch)
{
    auto phase = std::make_shared<int>(0);
    auto loads = std::make_shared<int>(0);
    auto ticks = std::make_shared<int>(0);
    // Fired once, on the reload's load rather than the opening one: the hook
    // runs inside requestInitialSlice with the work already dispatched and the
    // completion queued behind this call, which is the only moment a test can
    // submit against the session on its way out.
    auto injected = std::make_shared<bool>(false);
    // What the injected interaction switches to. The reload's completion
    // replays the field, level and range the load was launched with, so if it
    // does that after the user has moved them, the change is reverted and the
    // re-slice this load owes renders what they left. All three, because the
    // controls are one set per window: an interaction that supersedes the
    // reload can have moved any of them, and each is restored from the same
    // launch-time spec. Empty for a control the dataset gave nothing to move
    // to, which the checks below then skip.
    auto chosenField = std::make_shared<QString>();
    auto chosenLevel = std::make_shared<std::optional<int>>();
    auto chosenRangeMode = std::make_shared<std::optional<int>>();
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&application, loads](bool success) {
            if (!success) {
                application.exit(2);
                return;
            }
            ++*loads;
        });
    window.setInitialSliceLaunchedHookForTest(
        [&window, loads, injected, chosenField, chosenLevel, chosenRangeMode,
            dispatch] {
            if (*loads == 0 || *injected) {
                return;
            }
            *injected = true;
            // Move the controls, as a user reading a reloading window would,
            // and then submit for them. Both matter: the submission is what
            // supersedes the reload's display for this view, and the
            // selections are what the completion must not roll back.
            auto* selector = window.findChild<QComboBox*>(
                QStringLiteral("fieldSelector"));
            if (selector == nullptr || selector->count() < 2) {
                return;
            }
            const auto next = selector->currentIndex() == 0 ? 1 : 0;
            window.selectFieldItemForTest(next);
            *chosenField = selector->currentText();
            // Through the widgets, which is what a user's click reaches: the
            // level combo carries its selection as item data (the position
            // means nothing across a repopulate), and the range mode goes to
            // Visible, the one mode no dataset can leave unavailable.
            auto* levels = window.findChild<QComboBox*>(
                QStringLiteral("levelSelector"));
            if (levels != nullptr && levels->count() > 1) {
                levels->setCurrentIndex(levels->currentIndex() == 0 ? 1 : 0);
                *chosenLevel = levels->currentData().toInt();
            }
            auto* modes = window.findChild<QComboBox*>(
                QStringLiteral("rangeModeSelector"));
            if (modes != nullptr) {
                const auto visible = modes->findData(
                    static_cast<int>(amrvis::qt::RangeMode::Visible));
                if (visible >= 0 && modes->currentIndex() != visible) {
                    modes->setCurrentIndex(visible);
                    *chosenRangeMode = modes->currentData().toInt();
                }
            }
            if (dispatch) {
                window.requestActiveViewSliceForTest();
            }
        });

    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, loads, ticks, injected, chosenField,
            chosenLevel, chosenRangeMode, timer] {
            const auto finish = [&application, timer](int code) {
                timer->stop();
                application.exit(code);
            };
            ++*ticks;
            if (*ticks > 400) {
                qCritical("the reload race scenario never settled");
                finish(20);
                return;
            }
            if (*loads == 0) {
                return;
            }
            if (*phase == 0) {
                // A definition every 2-D plotfile here can compute, so the
                // reload it triggers succeeds and installs a new session. Set
                // on the shared store, which is what an Apply in any window
                // does and what makes every window reload.
                amrvis::qt::DerivedFieldStore::session().set(
                    {{"twice", "density * 2"}});
                *phase = 1;
                return;
            }
            if (*phase == 1) {
                // Wait for the reload's own load to have finished, not just
                // for the view queues to be quiet: slicesInFlightForTest sums
                // per-view pendingRequests and does not count the initial-slice
                // worker, so without the load count this tick can fire while
                // the reload is still running -- and then the outgoing session
                // is still installed and the comparison below is against
                // itself, which passes whatever the code does. `loads` is 1
                // after the open and 2 once the reload has installed.
                if (!*injected || *loads < 2
                    || window.slicesInFlightForTest() != 0
                    || window.sliceRequestPendingForTest()) {
                    return;
                }
                if (window.backgroundErrorCountForTest() != 0) {
                    qCritical("the reload race reported an error");
                    finish(22);
                    return;
                }
                // The invariant: no view keeps a raster from a session that is
                // no longer installed.
                if (!window.activeViewSliceMatchesSessionForTest()) {
                    qCritical("the displayed slice outlived its session");
                    finish(23);
                    return;
                }
                // And the interaction that superseded the reload was not
                // quietly undone by the reload's own control restoration.
                auto* selector = window.findChild<QComboBox*>(
                    QStringLiteral("fieldSelector"));
                if (selector == nullptr
                    || (!chosenField->isEmpty()
                        && selector->currentText() != *chosenField)) {
                    qCritical("the reload reverted the field the user chose");
                    finish(24);
                    return;
                }
                auto* levels = window.findChild<QComboBox*>(
                    QStringLiteral("levelSelector"));
                if (chosenLevel->has_value()
                    && (levels == nullptr
                        || levels->currentData().toInt() != **chosenLevel)) {
                    qCritical("the reload reverted the level the user chose");
                    finish(25);
                    return;
                }
                auto* modes = window.findChild<QComboBox*>(
                    QStringLiteral("rangeModeSelector"));
                if (chosenRangeMode->has_value()
                    && (modes == nullptr
                        || modes->currentData().toInt()
                            != **chosenRangeMode)) {
                    qCritical(
                        "the reload reverted the range mode the user chose");
                    finish(26);
                    return;
                }
                finish(0);
                return;
            }
        });
    timer->start();
}

// Arms the skipped-definition race: the user is displaying a derived field
// when an edit arrives that this dataset cannot resolve. The reload lists that
// definition greyed out -- same name, no field id -- while a view superseded
// meanwhile has the completion restore the user's field *by that name*. Exit 0
// if what ends up selected is a field; a nonzero code names the step that
// failed.
void armSkippedFieldChecks(
    amrvis::qt::MainWindow& window, QApplication& application)
{
    auto phase = std::make_shared<int>(0);
    auto loads = std::make_shared<int>(0);
    auto ticks = std::make_shared<int>(0);
    auto armed = std::make_shared<bool>(false);
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&application, loads](bool success) {
            if (!success) {
                application.exit(2);
                return;
            }
            ++*loads;
        });
    window.setInitialSliceLaunchedHookForTest([&window, armed] {
        if (!*armed) {
            return;
        }
        *armed = false;
        // Supersedes the reload's display for this view without touching the
        // selection: the field the completion then restores is the one the
        // user is on, which is the whole of what the restore is for.
        window.requestActiveViewSliceForTest();
    });

    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, loads, ticks, armed, timer] {
            const auto finish = [&application, timer](int code) {
                timer->stop();
                application.exit(code);
            };
            ++*ticks;
            if (*ticks > 400) {
                qCritical("the skipped-definition scenario never settled");
                finish(40);
                return;
            }
            if (*loads == 0) {
                return;
            }
            auto* selector = window.findChild<QComboBox*>(
                QStringLiteral("fieldSelector"));
            if (selector == nullptr) {
                qCritical("the window has no field selector");
                finish(41);
                return;
            }
            const auto settled = [&window] {
                return window.slicesInFlightForTest() == 0
                    && !window.sliceRequestPendingForTest();
            };
            if (*phase == 0) {
                amrvis::qt::DerivedFieldStore::session().set(
                    {{"twice", "density * 2"}});
                *phase = 1;
                return;
            }
            if (*phase == 1) {
                if (*loads < 2 || !settled()) {
                    return;
                }
                const auto index = selector->findText(QStringLiteral("twice"));
                if (index < 0 || !selector->itemData(index).isValid()) {
                    qCritical("a definition this dataset can compute was not "
                              "installed as a field");
                    finish(42);
                    return;
                }
                window.selectFieldItemForTest(index);
                if (selector->currentText() != QStringLiteral("twice")) {
                    qCritical("the derived field could not be selected");
                    finish(42);
                    return;
                }
                *phase = 2;
                return;
            }
            if (*phase == 2) {
                if (!settled()) {
                    return;
                }
                // The same name, now reading a field this dataset does not
                // have: the session skips it and the window lists it greyed.
                *armed = true;
                amrvis::qt::DerivedFieldStore::session().set(
                    {{"twice", "nonesuch * 2"}});
                *phase = 3;
                return;
            }
            if (*phase == 3) {
                if (*loads < 3 || !settled()) {
                    return;
                }
                const auto index = selector->findText(QStringLiteral("twice"));
                if (index < 0 || selector->itemData(index).isValid()) {
                    qCritical("a definition this dataset cannot resolve is "
                              "not listed greyed out");
                    finish(43);
                    return;
                }
                // The row carrying no id is exactly what setCurrentIndex will
                // take and every reader of currentData() will call field 0.
                if (!selector->currentData().isValid()) {
                    qCritical("the reload selected a row that is not a field");
                    finish(44);
                    return;
                }
                finish(0);
                return;
            }
        });
    timer->start();
}

// Arms the retry scenario: a reload that fails leaves the list committed and
// uninstalled, and the store emits nothing for a list that has not moved, so
// the Apply the user presses again has to ask for the reload itself. Exit 0
// once the definition the first Apply could not install is on the field list;
// a nonzero code names the step that failed.
void armReloadRetryChecks(
    amrvis::qt::MainWindow& window, QApplication& application)
{
    auto phase = std::make_shared<int>(0);
    auto loads = std::make_shared<int>(0);
    auto failures = std::make_shared<int>(0);
    auto ticks = std::make_shared<int>(0);
    // The tick the retry was pressed on, so the wait for it to land is bounded
    // by itself rather than by the scenario's own budget -- what the bug looks
    // like is nothing happening, and it should say so in those words.
    auto retryTick = std::make_shared<int>(0);
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [loads, failures](bool success) {
            if (success) {
                ++*loads;
            } else {
                ++*failures;
            }
        });

    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, loads, failures, ticks, retryTick,
            timer] {
            const auto finish = [&application, timer](int code) {
                timer->stop();
                application.exit(code);
            };
            ++*ticks;
            if (*ticks > 400) {
                qCritical("the reload retry scenario never settled");
                finish(30);
                return;
            }
            if (*loads == 0) {
                return;
            }
            auto* dialog =
                window.findChild<amrvis::qt::ExpressionEditorDialog*>();
            if (*phase == 0) {
                auto* action = window.findChild<QAction*>(
                    QStringLiteral("expressionEditorAction"));
                if (action == nullptr || !action->isEnabled()) {
                    qCritical("the Expression Editor action is not available");
                    finish(31);
                    return;
                }
                action->trigger();
                *phase = 1;
                return;
            }
            if (*phase == 1) {
                if (dialog == nullptr) {
                    qCritical("the Expression Editor did not open");
                    finish(32);
                    return;
                }
                // The reload this Apply starts goes down its failure arm,
                // which is what a server refusing the reopen -- or a
                // connection that has gone -- leaves behind: the list
                // committed, the previous session still installed, and the
                // editor reporting a definition that never arrived.
                window.failNextInitialSliceForTest();
                if (!writeDefinition(*dialog, QStringLiteral("twice"),
                        QStringLiteral("density * 2"))
                    || !clickApply(*dialog)) {
                    qCritical("the editor did not take the definition");
                    finish(33);
                    return;
                }
                if (errorShown(*dialog)) {
                    qCritical("a definition this dataset can compute was "
                              "refused");
                    finish(33);
                    return;
                }
                *phase = 2;
                return;
            }
            if (*phase == 2) {
                if (*failures == 0) {
                    return;  // the reload is still running
                }
                if (fieldNames(window).contains(QStringLiteral("twice"))) {
                    qCritical("a failed reload installed the definition");
                    finish(34);
                    return;
                }
                if (dialog == nullptr) {
                    qCritical("the editor closed over a failed reload");
                    finish(35);
                    return;
                }
                // The same list a second time, which the store does not emit
                // for. Nothing else in the window can ask for that reload
                // again, so without the Apply doing it the definition stays
                // uninstalled for good.
                if (!clickApply(*dialog)) {
                    qCritical("the editor has no Apply button");
                    finish(35);
                    return;
                }
                *retryTick = *ticks;
                *phase = 3;
                return;
            }
            if (*phase == 3) {
                if (fieldNames(window).contains(QStringLiteral("twice"))) {
                    if (window.slicesInFlightForTest() != 0
                        || window.sliceRequestPendingForTest()) {
                        return;
                    }
                    finish(0);
                    return;
                }
                if (*ticks - *retryTick > 120) {
                    qCritical("an unchanged Apply did not retry the reload "
                              "that failed");
                    finish(36);
                    return;
                }
                return;
            }
        });
    timer->start();
}

// Arms the scenario on `window`: exit 0 once a derived field has been
// refused, accepted, and rendered; a nonzero code names the step that failed.
void armDerivedChecks(amrvis::qt::MainWindow& window, QApplication& application)
{
    auto phase = std::make_shared<int>(0);
    auto loads = std::make_shared<int>(0);
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&application, loads](bool success) {
            if (!success) {
                application.exit(2);
                return;
            }
            ++*loads;
        });

    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, loads, timer] {
            const auto finish = [&application, timer](int code) {
                timer->stop();
                application.exit(code);
            };
            if (*loads == 0) {
                return;  // the dataset is still opening
            }
            auto* dialog =
                window.findChild<amrvis::qt::ExpressionEditorDialog*>();

            if (*phase == 0) {
                if (fieldNames(window)
                    != QStringList{QStringLiteral("density"),
                        QStringLiteral("temperature")}) {
                    qCritical("the fixture's own fields are not listed");
                    finish(3);
                    return;
                }
                auto* action = window.findChild<QAction*>(
                    QStringLiteral("expressionEditorAction"));
                if (action == nullptr || !action->isEnabled()) {
                    qCritical("the Expression Editor action is not available "
                              "over a local plotfile");
                    finish(4);
                    return;
                }
                action->trigger();
                *phase = 1;
                return;
            }
            if (*phase == 1) {
                if (dialog == nullptr) {
                    qCritical("the Expression Editor did not open");
                    finish(5);
                    return;
                }
                // A refusal must be reported in place and change nothing: not
                // the field list, and not the dataset (no reload). What is
                // refused is what is wrong whatever the data -- here, an
                // expression that does not parse. A name this dataset happens
                // not to have is *not* refused; it is dimmed, which phase 4
                // checks.
                if (!writeDefinition(*dialog, QStringLiteral("product"),
                        QStringLiteral("density *"))) {
                    qCritical("the editor did not take the definition");
                    finish(6);
                    return;
                }
                if (!clickApply(*dialog)) {
                    qCritical("the editor has no Apply button");
                    finish(6);
                    return;
                }
                if (!errorShown(*dialog)) {
                    qCritical("an unparsable expression was not refused");
                    finish(7);
                    return;
                }
                if (*loads != 1 || fieldNames(window).size() != 2) {
                    qCritical("a refused definition still changed the fields");
                    finish(7);
                    return;
                }
                *phase = 2;
                return;
            }
            if (*phase == 2) {
                if (dialog == nullptr) {
                    qCritical("the editor closed on a refusal");
                    finish(5);
                    return;
                }
                // Written over two lines, as a long expression would be.
                if (!writeDefinition(*dialog, QStringLiteral("product"),
                        QStringLiteral("density *\n    temperature"))
                    || !clickApply(*dialog)) {
                    qCritical("the editor did not take the corrected "
                              "definition");
                    finish(6);
                    return;
                }
                if (errorShown(*dialog)) {
                    qCritical("a resolvable definition was refused");
                    finish(8);
                    return;
                }
                *phase = 3;
                return;
            }
            if (*phase == 3) {
                // Apply reopens the dataset: wait for that load to land.
                if (*loads < 2) {
                    return;
                }
                const auto names = fieldNames(window);
                if (names
                    != QStringList{QStringLiteral("density"),
                        QStringLiteral("temperature"),
                        QStringLiteral("product")}) {
                    qCritical("the derived field did not reach the field "
                              "selector");
                    finish(9);
                    return;
                }
                // And the Variable menu, which is the same selection shown
                // twice and has its own rebuild. That menu alone: scanning
                // every menu would pass on the name turning up anywhere at
                // all, which is not what the failure below claims.
                const QMenu* variableMenu = nullptr;
                for (const auto* menuAction : window.menuBar()->actions()) {
                    auto* menu = menuAction->menu();
                    if (menu != nullptr
                        && menuAction->text().remove(QLatin1Char('&'))
                            == QStringLiteral("Variable")) {
                        variableMenu = menu;
                    }
                }
                if (variableMenu == nullptr) {
                    qCritical("there is no Variable menu to look in");
                    finish(10);
                    return;
                }
                bool inVariableMenu = false;
                for (const auto* entry : variableMenu->actions()) {
                    inVariableMenu = inVariableMenu
                        || entry->text() == QStringLiteral("product");
                }
                if (!inVariableMenu) {
                    qCritical("the derived field did not reach the Variable "
                              "menu");
                    finish(10);
                    return;
                }
                // And the Dataset Metadata dock, which the open path filled
                // from the file's own metadata before this session installed
                // anything -- so it is refreshed exactly when the session's
                // fields are not the ones already listed.
                auto* metadataTree = window.findChild<QTreeWidget*>(
                    QStringLiteral("metadataTree"));
                if (metadataTree == nullptr
                    || metadataTree
                           ->findItems(QStringLiteral("product"),
                               Qt::MatchExactly | Qt::MatchRecursive)
                           .isEmpty()) {
                    qCritical("the derived field did not reach the metadata "
                              "dock");
                    finish(18);
                    return;
                }
                // A definition this dataset cannot satisfy is accepted and
                // shown greyed out rather than refused: the list is shared
                // with windows that may well be able to satisfy it.
                if (!appendDefinition(*dialog, QStringLiteral("elsewhere"),
                        QStringLiteral("nonesuch * 2"))
                    || !clickApply(*dialog) || errorShown(*dialog)) {
                    qCritical("a definition for another dataset was refused");
                    finish(14);
                    return;
                }
                *phase = 4;
                return;
            }
            if (*phase == 4) {
                if (*loads < 3) {
                    return;
                }
                auto* selector = window.findChild<QComboBox*>(
                    QStringLiteral("fieldSelector"));
                const auto unavailable =
                    selector->findText(QStringLiteral("elsewhere"));
                if (unavailable < 0
                    || selector->itemData(unavailable).isValid()) {
                    qCritical("the unavailable definition is not listed, or "
                              "is listed as a field");
                    finish(15);
                    return;
                }
                const auto* model = qobject_cast<const QStandardItemModel*>(
                    selector->model());
                if (model == nullptr
                    || model->item(unavailable)->isEnabled()) {
                    qCritical("the unavailable definition is not dimmed");
                    finish(16);
                    return;
                }
                if (!selector->itemData(unavailable, Qt::ToolTipRole)
                        .toString()
                        .contains(QStringLiteral("unavailable"))) {
                    qCritical("the dimmed entry does not say why");
                    finish(17);
                    return;
                }
                // The derived field is set apart from the stored ones, and
                // says what it is: the separator sits directly before it, and
                // the entry carries its expression as a tooltip.
                const auto product = selector->findData(2U);
                if (product < 1 || selector->itemData(product - 1).isValid()) {
                    qCritical("no separator precedes the derived field");
                    finish(14);
                    return;
                }
                // The tooltip folds the layout back onto one line, and is
                // rich text so that whatever the user wrote survives the
                // escaping Qt would otherwise show as itself.
                if (selector->itemData(product, Qt::ToolTipRole).toString()
                    != QStringLiteral("<qt>density * temperature</qt>")) {
                    qCritical("the derived field carries no expression: %s",
                        qPrintable(selector
                                ->itemData(product, Qt::ToolTipRole)
                                .toString()));
                    finish(15);
                    return;
                }
                selector->setCurrentIndex(product);
                *phase = 5;
                return;
            }
            if (*phase == 5) {
                if (window.sliceRequestPendingForTest()
                    || window.slicesInFlightForTest() != 0) {
                    return;  // the debounced re-slice is still coming
                }
                const auto size = window.activeViewImageSizeForTest();
                if (size[0] <= 0 || size[1] <= 0) {
                    qCritical("selecting the derived field rendered nothing");
                    finish(11);
                    return;
                }
                if (window.backgroundErrorCountForTest() != 0) {
                    qCritical("the derived field's slice reported an error");
                    finish(12);
                    return;
                }
                // A vector component pointed at the derived field, which is
                // then renamed out from under it. The ids do not move, so a
                // component that carried its old id over would now be reading
                // a field the user never chose -- silently, since the id is
                // still in range.
                window.setVectorFieldsForTest(2, 1, 1);
                if (dialog == nullptr) {
                    qCritical("the editor closed before the rename");
                    finish(18);
                    return;
                }
                {
                    auto* list = dialog->findChild<QListWidget*>(
                        QStringLiteral("expressionList"));
                    auto* nameEdit = dialog->findChild<QLineEdit*>(
                        QStringLiteral("expressionName"));
                    if (list == nullptr || nameEdit == nullptr) {
                        qCritical("the editor has no list to rename in");
                        finish(18);
                        return;
                    }
                    list->setCurrentRow(0);
                    nameEdit->setText(QStringLiteral("speed"));
                    if (!clickApply(*dialog) || errorShown(*dialog)) {
                        qCritical("the rename was refused");
                        finish(18);
                        return;
                    }
                }
                *phase = 6;
                return;
            }
            if (*phase == 6) {
                if (*loads < 4 || window.sliceRequestPendingForTest()
                    || window.slicesInFlightForTest() != 0) {
                    return;
                }
                auto* selector = window.findChild<QComboBox*>(
                    QStringLiteral("fieldSelector"));
                if (selector == nullptr) {
                    qCritical("there is no field selector");
                    finish(19);
                    return;
                }
                if (fieldNames(window)
                    != QStringList{QStringLiteral("density"),
                        QStringLiteral("temperature"),
                        QStringLiteral("speed")}) {
                    qCritical("the rename did not reach the field list: %s",
                        qPrintable(fieldNames(window).join(
                            QStringLiteral(", "))));
                    finish(19);
                    return;
                }
                // Read back through the selector, which is the same mapping
                // from field id to name the glyphs are drawn through.
                const auto vectors = window.vectorFieldsForTest();
                for (const auto field : vectors) {
                    if (field < 0
                        || selector->findData(
                               static_cast<unsigned int>(field))
                            < 0) {
                        qCritical("a vector component names no field: %d",
                            field);
                        finish(20);
                        return;
                    }
                }
                // `product` is gone, so the component set to it has to have
                // been re-detected rather than left on the id, which now
                // belongs to `speed`.
                if (selector->itemText(selector->findData(
                        static_cast<unsigned int>(vectors[0])))
                    == QStringLiteral("speed")) {
                    qCritical("the vector component followed its id onto "
                              "\"speed\", which was never chosen");
                    finish(21);
                    return;
                }
                if (window.backgroundErrorCountForTest() != 0) {
                    qCritical("the rename reported an error");
                    finish(12);
                    return;
                }
                finish(0);
            }
        });
    timer->start();
    QTimer::singleShot(30000, &application, [&application] { application.exit(13); });
}

// Arms the sequence scenario: applying a definition while a plotfile sequence
// is open must reload the frame on screen *and* leave the sequence navigable,
// with the definition surviving the next frame switch. A reload that went
// through the ordinary open path would end the sequence instead.
void armSequenceChecks(
    amrvis::qt::MainWindow& window, QApplication& application)
{
    auto phase = std::make_shared<int>(0);
    auto frames = std::make_shared<int>(0);
    // Bounds the wait for a prefetch in phase 4.
    auto ticks = std::make_shared<int>(0);
    // The frame count when playback was stopped, so the reload that follows
    // can be told from the frames playback itself displayed.
    auto framesAtStop = std::make_shared<int>(0);
    // Both frames list the same fields, so the frame stepped to lists what
    // this one does; frames that disagree are armFrameIdentityChecks.
    const QStringList expected{QStringLiteral("density"),
        QStringLiteral("temperature"), QStringLiteral("product")};
    QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
        &application, [frames](int) { ++*frames; });

    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, frames, ticks, framesAtStop, timer,
            expected] {
            ++*ticks;
            const auto finish = [&application, timer](int code) {
                timer->stop();
                application.exit(code);
            };
            if (*frames == 0) {
                return;  // frame 0 is still loading
            }
            auto* dialog =
                window.findChild<amrvis::qt::ExpressionEditorDialog*>();
            if (*phase == 0) {
                auto* action = window.findChild<QAction*>(
                    QStringLiteral("expressionEditorAction"));
                if (action == nullptr || !action->isEnabled()) {
                    qCritical("the Expression Editor action is not available "
                              "over a sequence");
                    finish(4);
                    return;
                }
                action->trigger();
                *phase = 1;
                return;
            }
            if (*phase == 1) {
                if (dialog == nullptr
                    || !writeDefinition(*dialog, QStringLiteral("product"),
                        QStringLiteral("density * temperature"))
                    || !clickApply(*dialog) || errorShown(*dialog)) {
                    qCritical("the editor refused the definition over a "
                              "sequence");
                    finish(6);
                    return;
                }
                *phase = 2;
                return;
            }
            if (*phase == 2) {
                // The reload displays a frame of its own.
                if (*frames < 2) {
                    return;
                }
                if (fieldNames(window) != expected) {
                    qCritical("the reload did not install the definition");
                    finish(9);
                    return;
                }
                // And the sequence is still a sequence: stepping to the other
                // frame must work and keep the derived field.
                window.requestSequenceFrameForTest(1);
                *phase = 3;
                return;
            }
            if (*phase == 3) {
                if (*frames < 3 || window.sliceRequestPendingForTest()
                    || window.slicesInFlightForTest() != 0) {
                    return;
                }
                const auto names = fieldNames(window);
                if (names != expected) {
                    qCritical("the stepped-to frame lists: %s",
                        qPrintable(names.join(QStringLiteral(", "))));
                    finish(10);
                    return;
                }
                // Whatever it lists, the combo must name the field the frame
                // was actually rendered with.
                auto* selector = window.findChild<QComboBox*>(
                    QStringLiteral("fieldSelector"));
                if (selector == nullptr
                    || !selector->currentData().isValid()) {
                    qCritical("the field selector is on no field at all");
                    finish(11);
                    return;
                }
                if (window.backgroundErrorCountForTest() != 0) {
                    qCritical("the sequence reported an error");
                    finish(12);
                    return;
                }
                *ticks = 0;
                *phase = 4;
                return;
            }
            if (*phase == 4) {
                // Playback is the case where the reload stands aside and lets
                // the next frame read the list for itself -- which is only
                // true of a frame that is actually loaded. The one already in
                // the prefetch slot was rendered against the list as it was.
                if (!window.sequencePlayingForTest()) {
                    window.toggleSequencePlaybackForTest();
                    if (!window.sequencePlayingForTest()) {
                        qCritical("sequence playback did not start");
                        finish(14);
                        return;
                    }
                    return;
                }
                if (!window.prefetchedSequenceFrameForTest()) {
                    if (*ticks > 200) {
                        qCritical("no frame was ever prefetched");
                        finish(15);
                        return;
                    }
                    return;
                }
                auto* editor =
                    window.findChild<amrvis::qt::ExpressionEditorDialog*>();
                if (editor == nullptr
                    || !appendDefinition(*editor, QStringLiteral("late"),
                        QStringLiteral("temperature + 1"))
                    || !clickApply(*editor) || errorShown(*editor)) {
                    qCritical("the editor refused the definition mid-playback");
                    finish(16);
                    return;
                }
                // Synchronously: apply commits, the store tells this window,
                // and the reload it asks for is what drops the prefetch.
                if (window.prefetchedSequenceFrameForTest()) {
                    qCritical("the frame prefetched before the change "
                              "survived it");
                    finish(17);
                    return;
                }
                // Stopping now is the case the prefetch drop cannot cover:
                // there is no next frame to read the list, so the frame on
                // screen would go on computing the old expression.
                *framesAtStop = *frames;
                *ticks = 0;
                window.toggleSequencePlaybackForTest();
                *phase = 5;
                return;
            }
            if (*phase == 5) {
                if (*frames == *framesAtStop
                    || window.sliceRequestPendingForTest()
                    || window.slicesInFlightForTest() != 0) {
                    if (*ticks > 200) {
                        qCritical("stopping playback after an Apply never "
                                  "reloaded the frame on screen");
                        finish(18);
                        return;
                    }
                    return;
                }
                if (!fieldNames(window).contains(QStringLiteral("late"))) {
                    qCritical("the frame on screen lists %s, without the "
                              "definition applied during playback",
                        qPrintable(fieldNames(window).join(
                            QStringLiteral(", "))));
                    finish(19);
                    return;
                }
                if (window.backgroundErrorCountForTest() != 0) {
                    qCritical("the reload after playback reported an error");
                    finish(12);
                    return;
                }
                finish(0);
            }
        });
    timer->start();
    QTimer::singleShot(30000, &application, [&application] { application.exit(13); });
}

// Arms the frame-identity scenario. Two frames that list different fields, and
// a selection whose *id* means a different field on the second: frame 0 is
// [density, temperature, double], the user is on `temperature` (id 1), and
// frame 1 has no `density`, so it lists [temperature, double] and id 1 is
// `double`. Carrying the selection as an index lands on `double` while the
// combo and the range still say `temperature`; carrying the name does not.
void armFrameIdentityChecks(
    amrvis::qt::MainWindow& window, QApplication& application)
{
    auto phase = std::make_shared<int>(0);
    auto frames = std::make_shared<int>(0);
    QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
        &application, [frames](int) { ++*frames; });

    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, frames, timer] {
            const auto finish = [&application, timer](int code) {
                timer->stop();
                application.exit(code);
            };
            if (*frames == 0 || window.sliceRequestPendingForTest()
                || window.slicesInFlightForTest() != 0) {
                return;
            }
            auto* dialog =
                window.findChild<amrvis::qt::ExpressionEditorDialog*>();
            auto* selector = window.findChild<QComboBox*>(
                QStringLiteral("fieldSelector"));
            if (selector == nullptr) {
                qCritical("there is no field selector");
                finish(3);
                return;
            }
            if (*phase == 0) {
                auto* action = window.findChild<QAction*>(
                    QStringLiteral("expressionEditorAction"));
                if (action == nullptr || !action->isEnabled()) {
                    qCritical("the Expression Editor is unavailable");
                    finish(4);
                    return;
                }
                action->trigger();
                *phase = 1;
                return;
            }
            if (*phase == 1) {
                // Two, in this order on purpose. The first reads `density`,
                // which the stepped-to frame does not have, so there it is
                // listed greyed out -- immediately after the separator, which
                // puts two rows that are not fields next to each other. The
                // second reads only `temperature`, so it survives the step at
                // a different id.
                if (dialog == nullptr
                    || !writeDefinition(*dialog,
                        QStringLiteral("fromDensity"),
                        QStringLiteral("density * 2"))
                    || !appendDefinition(*dialog, QStringLiteral("double"),
                        QStringLiteral("temperature * 2"))
                    || !clickApply(*dialog) || errorShown(*dialog)) {
                    qCritical("the editor refused the definitions");
                    finish(6);
                    return;
                }
                *phase = 2;
                return;
            }
            if (*phase == 2) {
                if (*frames < 2) {
                    return;
                }
                const auto temperature = selector->findText(
                    QStringLiteral("temperature"));
                // density, temperature, the separator, and both definitions,
                // which this frame can resolve.
                if (temperature < 0 || selector->count() != 5) {
                    qCritical("frame 0 lists %d row(s), not the 5 expected",
                        selector->count());
                    finish(9);
                    return;
                }
                selector->setCurrentIndex(temperature);
                *phase = 3;
                return;
            }
            if (*phase == 3) {
                window.requestSequenceFrameForTest(1);
                *phase = 4;
                return;
            }
            if (*phase == 4) {
                if (*frames < 3) {
                    return;
                }
                if (fieldNames(window)
                    != QStringList{QStringLiteral("temperature"),
                        QStringLiteral("double")}) {
                    qCritical("the stepped-to frame lists: %s",
                        qPrintable(fieldNames(window).join(
                            QStringLiteral(", "))));
                    finish(10);
                    return;
                }
                if (selector->currentText() != QStringLiteral("temperature")) {
                    qCritical("the selection became \"%s\" across the frame "
                              "switch",
                        qPrintable(selector->currentText()));
                    finish(11);
                    return;
                }
                if (window.backgroundErrorCountForTest() != 0) {
                    qCritical("the frame switch reported an error");
                    finish(12);
                    return;
                }
                // temperature, the separator, the greyed-out `fromDensity`
                // and `double`: the two adjacent rows that are not fields are
                // what the sweep below needs to be about anything.
                if (selector->count() != 4) {
                    qCritical("the stepped-to frame lists %d row(s), not the "
                              "4 expected",
                        selector->count());
                    finish(14);
                    return;
                }
                {
                    // Wherever a selection is aimed -- a restored index, a
                    // clamped one -- it has to come to rest on a field. A row
                    // with no item data is read as field 0 by everything
                    // downstream while the combo names something else.
                    const QSignalBlocker blocker(selector);
                    const auto restore = selector->currentIndex();
                    for (int index = 0; index < selector->count(); ++index) {
                        window.selectFieldItemForTest(index);
                        if (!selector->currentData().isValid()) {
                            qCritical("selecting row %d came to rest on row "
                                      "%d, which is not a field",
                                index, selector->currentIndex());
                            finish(15);
                            return;
                        }
                    }
                    selector->setCurrentIndex(restore);
                }
                finish(0);
            }
        });
    timer->start();
    QTimer::singleShot(30000, &application, [&application] { application.exit(13); });
}

// Arms the playback/late-change scenario. Two ways a committed definition can
// fail to reach the session on screen, neither of which the other scenarios
// pass through: a plane sweep, which moves the slice position on the session
// already open and never reopens it, so the reload an Apply asks for is the
// only thing that can install the definition; and a list that changes while
// this window is opening a dataset, when the window counts as unable to take
// derived fields and so is not told to reload at all.
void armPlaybackChecks(amrvis::qt::MainWindow& window,
    QApplication& application, const std::filesystem::path& path)
{
    auto phase = std::make_shared<int>(0);
    auto loads = std::make_shared<int>(0);
    // Bounds the waits below, so a change that never arrives fails saying so
    // rather than running into the watchdog.
    auto ticks = std::make_shared<int>(0);
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&application, loads](bool success) {
            if (!success) {
                application.exit(2);
                return;
            }
            ++*loads;
        });

    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, loads, ticks, timer, path] {
            const auto finish = [&application, timer](int code) {
                timer->stop();
                application.exit(code);
            };
            ++*ticks;
            if (*loads == 0) {
                return;  // the dataset is still opening
            }
            auto* selector = window.findChild<QComboBox*>(
                QStringLiteral("fieldSelector"));
            if (selector == nullptr) {
                qCritical("there is no field selector");
                finish(3);
                return;
            }
            if (*phase == 0) {
                window.toggleSweepPlaybackForTest();
                if (!window.sweepPlayingForTest()) {
                    qCritical("the plane sweep did not start");
                    finish(4);
                    return;
                }
                auto* action = window.findChild<QAction*>(
                    QStringLiteral("expressionEditorAction"));
                if (action == nullptr || !action->isEnabled()) {
                    qCritical("the Expression Editor is unavailable");
                    finish(5);
                    return;
                }
                action->trigger();
                *phase = 1;
                return;
            }
            auto* dialog =
                window.findChild<amrvis::qt::ExpressionEditorDialog*>();
            if (*phase == 1) {
                if (dialog == nullptr
                    || !writeDefinition(*dialog, QStringLiteral("swept"),
                        QStringLiteral("q * 2"))
                    || !clickApply(*dialog) || errorShown(*dialog)) {
                    qCritical("the editor refused the definition");
                    finish(6);
                    return;
                }
                *ticks = 0;
                *phase = 2;
                return;
            }
            if (*phase == 2) {
                if (!fieldNames(window).contains(QStringLiteral("swept"))) {
                    if (*ticks > 200) {
                        qCritical("applying during a plane sweep never "
                                  "reached the field list");
                        finish(7);
                        return;
                    }
                    return;
                }
                if (!window.sweepPlayingForTest()) {
                    qCritical("the reload stopped the plane sweep");
                    finish(8);
                    return;
                }
                window.toggleSweepPlaybackForTest();
                // What another window's Apply looks like from here: the list
                // moves while this window has no dataset, which is the whole
                // of an open rather than just its start.
                *ticks = 0;
                // Committed while the load is on a worker, which is the only
                // time it can be missed: the spec that load carries was built
                // a moment earlier, and this window is not told to reload
                // because it has no dataset until the load lands. Left armed
                // for the reload that follows, where it re-commits the same
                // list and so changes nothing.
                window.setInitialSliceLaunchedHookForTest([] {
                    amrvis::qt::DerivedFieldStore::session().set(
                        {{"swept", "q * 2"}, {"raced", "q + 1"}});
                });
                window.openDataset(path);
                *phase = 3;
                return;
            }
            if (*phase == 3) {
                const auto raced = selector->findText(QStringLiteral("raced"));
                // Listed *and* a field: a definition reaches the selector
                // either way, greyed out when the open session could not
                // install it.
                if (raced < 0 || !selector->itemData(raced).isValid()) {
                    if (*ticks > 200) {
                        qCritical("a definition committed while the dataset "
                                  "was opening never reached the session: %s",
                            qPrintable(fieldNames(window).join(
                                QStringLiteral(", "))));
                        finish(9);
                        return;
                    }
                    return;
                }
                if (window.backgroundErrorCountForTest() != 0) {
                    qCritical("the late change reported an error");
                    finish(10);
                    return;
                }
                finish(0);
            }
        });
    timer->start();
    QTimer::singleShot(30000, &application, [&application] { application.exit(13); });
}

// Arms the remote scenario. Everything the local one checks, but with the
// session on the far side of the protocol: the editor is available over a
// remote dataset (which it was not before 1.4), Apply reopens the dataset on
// its own connection with the definitions, the computed field arrives as the
// tail of the catalog and renders, and a definition the plotfile cannot
// satisfy comes back greyed with the *server's* reason.
void armRemoteDerivedChecks(
    amrvis::qt::MainWindow& window, QApplication& application)
{
    auto phase = std::make_shared<int>(0);
    auto loads = std::make_shared<int>(0);
    auto ticks = std::make_shared<int>(0);
    // The load count when the Apply settled, so the idle turns that follow can
    // be told from the loads the Apply itself caused. Read on every tick it
    // would compare against itself and never fail.
    auto loadsAtSettle = std::make_shared<int>(0);
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&application, loads](bool success) {
            if (!success) {
                application.exit(2);
                return;
            }
            ++*loads;
        });

    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, loads, ticks, loadsAtSettle, timer] {
            const auto finish = [&application, timer](int code) {
                timer->stop();
                application.exit(code);
            };
            ++*ticks;
            if (*loads == 0) {
                return;  // the remote dataset is still opening
            }
            auto* selector = window.findChild<QComboBox*>(
                QStringLiteral("fieldSelector"));
            auto* action = window.findChild<QAction*>(
                QStringLiteral("expressionEditorAction"));
            if (selector == nullptr || action == nullptr) {
                qCritical("the window is missing its field selector or action");
                finish(3);
                return;
            }
            auto* dialog =
                window.findChild<amrvis::qt::ExpressionEditorDialog*>();
            if (*phase == 0) {
                if (fieldNames(window)
                    != QStringList{QStringLiteral("density"),
                        QStringLiteral("temperature")}) {
                    qCritical("the remote dataset lists: %s",
                        qPrintable(fieldNames(window).join(
                            QStringLiteral(", "))));
                    finish(4);
                    return;
                }
                // The whole point of 1.4: over a remote session this used to
                // be disabled, saying derived fields needed a local dataset.
                if (!action->isEnabled()) {
                    qCritical("the Expression Editor is unavailable over a "
                              "remote dataset: %s",
                        qPrintable(action->toolTip()));
                    finish(5);
                    return;
                }
                action->trigger();
                *phase = 1;
                return;
            }
            if (*phase == 1) {
                if (dialog == nullptr
                    || !writeDefinition(*dialog, QStringLiteral("product"),
                        QStringLiteral("density * temperature"))
                    || !appendDefinition(*dialog,
                        QStringLiteral("elsewhere"),
                        QStringLiteral("nonesuch * 2"))
                    || !clickApply(*dialog) || errorShown(*dialog)) {
                    qCritical("the editor refused the definitions");
                    finish(6);
                    return;
                }
                *ticks = 0;
                *phase = 2;
                return;
            }
            if (*phase == 2) {
                // Apply reopens the remote dataset on its own connection.
                if (!fieldNames(window).contains(QStringLiteral("product"))) {
                    if (*ticks > 400) {
                        qCritical("applying over a remote session never "
                                  "reached the field list: %s",
                            qPrintable(fieldNames(window).join(
                                QStringLiteral(", "))));
                        finish(7);
                        return;
                    }
                    return;
                }
                // The one the plotfile cannot satisfy is listed, greyed, with
                // the server's own reason on it.
                const auto unavailable
                    = selector->findText(QStringLiteral("elsewhere"));
                if (unavailable < 0
                    || selector->itemData(unavailable).isValid()) {
                    qCritical("the unavailable definition is not listed, or "
                              "is listed as a field");
                    finish(8);
                    return;
                }
                if (!selector->itemData(unavailable, Qt::ToolTipRole)
                        .toString()
                        .contains(QStringLiteral("unavailable"))) {
                    qCritical("the dimmed entry does not say why");
                    finish(9);
                    return;
                }
                const auto product
                    = selector->findText(QStringLiteral("product"));
                selector->setCurrentIndex(product);
                *ticks = 0;
                *phase = 3;
                return;
            }
            if (*phase == 3) {
                if (window.sliceRequestPendingForTest()
                    || window.slicesInFlightForTest() != 0) {
                    if (*ticks > 400) {
                        qCritical("the computed field never finished slicing");
                        finish(10);
                        return;
                    }
                    return;
                }
                const auto size = window.activeViewImageSizeForTest();
                if (size[0] <= 0 || size[1] <= 0) {
                    qCritical("the computed field rendered nothing");
                    finish(11);
                    return;
                }
                if (window.backgroundErrorCountForTest() != 0) {
                    qCritical("the remote computed field reported an error");
                    finish(12);
                    return;
                }
                // And the reload settled: one Apply must not leave the window
                // reopening the dataset for ever, which is what would happen
                // if the session and the editor disagreed about the list.
                *loadsAtSettle = *loads;
                *ticks = 0;
                *phase = 4;
                return;
            }
            if (*phase == 4) {
                // A few idle turns with nothing in flight: a reload loop shows
                // up here as loads climbing without anything asking.
                if (*ticks < 20) {
                    return;
                }
                if (*loads != *loadsAtSettle) {
                    qCritical("the window is still reopening the dataset");
                    finish(13);
                    return;
                }
                finish(0);
            }
        });
    timer->start();
    QTimer::singleShot(40000, &application, [&application] { application.exit(14); });
}

Outcome dispatchDerivedSequence(Context& context)
{
    if (context.argc != 4) {
        return {false, std::nullopt};
    }
    const std::string_view option(context.argv[1]);
    if (option != "--derived-field-sequence-smoke-test"
        && option != "--derived-field-frames-smoke-test") {
        return {false, std::nullopt};
    }
    const std::vector<std::filesystem::path> frames{
        std::filesystem::path(context.argv[2]),
        std::filesystem::path(context.argv[3])};
    if (option == "--derived-field-frames-smoke-test") {
        armFrameIdentityChecks(context.window, context.application);
    } else {
        armSequenceChecks(context.window, context.application);
    }
    QTimer::singleShot(0, &context.window,
        [&window = context.window, frames] { window.openSequence(frames); });
    return {true, std::nullopt};
}

} // namespace

Outcome dispatchDerived(Context& context)
{
    if (auto outcome = dispatchDerivedSequence(context); outcome.handled) {
        return outcome;
    }
    auto& application = context.application;
    auto& window = context.window;
    if (context.argc != 3) {
        return {false, std::nullopt};
    }
    const std::string_view option(context.argv[1]);
    const std::filesystem::path path(context.argv[2]);
    if (option == "--field-range-memory-smoke-test") {
        armRangeMemoryChecks(window, application);
    } else if (option == "--derived-field-smoke-test") {
        armDerivedChecks(window, application);
    } else if (option == "--derived-field-reload-race-smoke-test") {
        armReloadRaceChecks(window, application, true);
    } else if (option == "--derived-field-reload-debounce-smoke-test") {
        armReloadRaceChecks(window, application, false);
    } else if (option == "--derived-field-reload-retry-smoke-test") {
        armReloadRetryChecks(window, application);
    } else if (option == "--derived-field-skipped-race-smoke-test") {
        armSkippedFieldChecks(window, application);
    } else if (option == "--derived-field-playback-smoke-test") {
        armPlaybackChecks(window, application, path);
    } else if (option == "--remote-derived-field-smoke-test") {
        // The in-process loopback server, as the remote theme starts one: the
        // handshake takes milliseconds and runs on the GUI thread.
        context.server = std::make_shared<amrvis::remote::Server>();
        context.serverThread.emplace(
            [server = context.server] { server->run(); });
        armRemoteDerivedChecks(window, application);
        QTimer::singleShot(0, &window,
            [&window, remotePath = path.string(),
                server = context.server] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(remotePath);
            });
        return {true, std::nullopt};
    } else {
        return {false, std::nullopt};
    }
    QTimer::singleShot(
        0, &window, [&window, path] { window.openDataset(path); });
    return {true, std::nullopt};
}

} // namespace amrvis::qt::smoke
