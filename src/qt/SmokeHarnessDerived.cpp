#include "SmokeHarnessInternal.hpp"

#include "ExpressionEditorDialog.hpp"
#include "MainWindow.hpp"

#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTimer>

#include <filesystem>
#include <memory>
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
                // the field list, and not the dataset (no reload).
                if (!writeDefinition(*dialog, QStringLiteral("product"),
                        QStringLiteral("density * nonesuch"))) {
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
                    qCritical("an unresolvable definition was not refused");
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
                if (!writeDefinition(*dialog, QStringLiteral("product"),
                        QStringLiteral("density * temperature"))
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
                auto* selector = window.findChild<QComboBox*>(
                    QStringLiteral("fieldSelector"));
                // The derived field is set apart from the stored ones, and
                // says what it is: the separator sits directly before it, and
                // the entry carries its expression as a tooltip.
                const auto product = selector->findData(2U);
                if (product < 1 || selector->itemData(product - 1).isValid()) {
                    qCritical("no separator precedes the derived field");
                    finish(14);
                    return;
                }
                if (selector->itemData(product, Qt::ToolTipRole).toString()
                    != QStringLiteral("density * temperature")) {
                    qCritical("the derived field carries no expression: %s",
                        qPrintable(selector
                                ->itemData(product, Qt::ToolTipRole)
                                .toString()));
                    finish(15);
                    return;
                }
                selector->setCurrentIndex(product);
                *phase = 4;
                return;
            }
            if (*phase == 4) {
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
void armSequenceChecks(amrvis::qt::MainWindow& window, QApplication& application)
{
    auto phase = std::make_shared<int>(0);
    auto frames = std::make_shared<int>(0);
    const QStringList expected{QStringLiteral("density"),
        QStringLiteral("temperature"), QStringLiteral("product")};
    QObject::connect(&window, &amrvis::qt::MainWindow::sequenceFrameDisplayed,
        &application, [frames](int) { ++*frames; });

    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, &application,
        [&window, &application, phase, frames, timer, expected] {
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
                window
                    .findChild<QAction*>(
                        QStringLiteral("expressionEditorAction"))
                    ->trigger();
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
                if (*frames < 3) {
                    return;
                }
                if (fieldNames(window) != expected) {
                    qCritical("the derived field did not survive a frame "
                              "switch");
                    finish(10);
                    return;
                }
                if (window.backgroundErrorCountForTest() != 0) {
                    qCritical("the sequence reported an error");
                    finish(12);
                    return;
                }
                finish(0);
            }
        });
    timer->start();
    QTimer::singleShot(30000, &application, [&application] { application.exit(13); });
}

Outcome dispatchDerivedSequence(Context& context)
{
    if (context.argc != 4
        || std::string_view(context.argv[1])
            != "--derived-field-sequence-smoke-test") {
        return {false, std::nullopt};
    }
    const std::vector<std::filesystem::path> frames{
        std::filesystem::path(context.argv[2]),
        std::filesystem::path(context.argv[3])};
    armSequenceChecks(context.window, context.application);
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
    if (option != "--derived-field-smoke-test") {
        return {false, std::nullopt};
    }
    armDerivedChecks(window, application);
    QTimer::singleShot(
        0, &window, [&window, path] { window.openDataset(path); });
    return {true, std::nullopt};
}

} // namespace amrvis::qt::smoke
