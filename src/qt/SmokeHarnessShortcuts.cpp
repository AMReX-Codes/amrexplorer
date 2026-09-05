#include "SmokeHarnessInternal.hpp"

#include "MainWindow.hpp"

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QSet>
#include <QShortcut>
#include <QTimer>

#include <filesystem>
#include <map>
#include <string_view>
#include <vector>

// Qt documents this switch but only declares it in headers for QDoc.
QT_BEGIN_NAMESPACE
Q_GUI_EXPORT void qt_set_sequence_auto_mnemonic(bool enabled);
QT_END_NAMESPACE

namespace amrvis::qt::smoke {

namespace {

bool checkMenuMnemonics(const QList<QAction*>& actions, const QString& path,
    bool topLevel = false)
{
    std::map<QKeySequence, QString> siblings;
    bool valid = true;
    for (const auto* action : actions) {
        if (action->isSeparator()) {
            continue;
        }
        const auto label = action->text();
        const auto key = QKeySequence::mnemonic(label);
        // Numeric choices and data-derived labels need not have mnemonics.
        if (!key.isEmpty()) {
            const auto [previous, inserted] = siblings.emplace(key, label);
            if (!inserted) {
                qCritical("%s: '%s' and '%s' share mnemonic %s",
                    qUtf8Printable(path), qUtf8Printable(previous->second),
                    qUtf8Printable(label), qUtf8Printable(key.toString()));
                valid = false;
            }
        } else if (topLevel) {
            qCritical("%s: '%s' has no mnemonic", qUtf8Printable(path),
                qUtf8Printable(label));
            valid = false;
        }
        // Include disabled menus: they become reachable after opening data.
        if (const auto* menu = action->menu()) {
            if (!checkMenuMnemonics(menu->actions(),
                    path + QStringLiteral(" > ") + label)) {
                valid = false;
            }
        }
    }
    if (topLevel && siblings.empty()) {
        qCritical("No top-level menu mnemonics were checked");
        valid = false;
    }
    return valid;
}

struct ShortcutBinding {
    QKeySequence key;
    QString label;
    const QObject* owner;
};

bool checkGlobalShortcuts(MainWindow& window)
{
    // Start with attached actions, not every owned QAction: controllers can
    // retain actions after removing them from a menu. QSet also prevents a
    // shared menu/toolbar action from being counted twice.
    QSet<QAction*> actions;
    const auto collect = [&actions](const QList<QAction*>& entries,
                             auto&& self) -> void {
        for (auto* action : entries) {
            if (actions.contains(action)) {
                continue;
            }
            actions.insert(action);
            if (const auto* menu = action->menu()) {
                self(menu->actions(), self);
            }
        }
    };
    collect(window.menuBar()->actions(), collect);
    collect(window.actions(), collect);
    for (const auto* widget : window.findChildren<QWidget*>()) {
        // Separate top-level windows may legitimately reuse window shortcuts.
        if (widget->window() == &window) {
            collect(widget->actions(), collect);
        }
    }

    std::vector<ShortcutBinding> bindings;
    const auto global = [](Qt::ShortcutContext scope) {
        return scope == Qt::WindowShortcut || scope == Qt::ApplicationShortcut;
    };
    for (const auto* action : actions) {
        if (global(action->shortcutContext())) {
            for (const auto& key : action->shortcuts()) {
                if (!key.isEmpty()) {
                    bindings.push_back({key, action->text(), action});
                }
            }
        }
    }
    for (const auto* shortcut : window.findChildren<QShortcut*>()) {
        const auto* widget = qobject_cast<QWidget*>(shortcut->parent());
        if (widget != nullptr && widget->window() == &window
            && global(shortcut->context())) {
            for (const auto& key : shortcut->keys()) {
                if (!key.isEmpty()) {
                    bindings.push_back({key,
                        QStringLiteral("QShortcut (%1)")
                            .arg(shortcut->objectName()), shortcut});
                }
            }
        }
    }
    // Only menu-bar mnemonics are window-wide Alt shortcuts. Submenu
    // mnemonics are active inside that menu and are checked among siblings.
    // In particular, bare B (Boxes) and Alt+B are different bindings.
    for (const auto* action : window.menuBar()->actions()) {
        const auto key = QKeySequence::mnemonic(action->text());
        if (!key.isEmpty()) {
            bindings.push_back({key,
                QStringLiteral("Menu %1").arg(action->text()), action});
        }
    }

    bool valid = true;
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        for (std::size_t j = i + 1; j < bindings.size(); ++j) {
            const auto& a = bindings[i];
            const auto& b = bindings[j];
            if (a.owner == b.owner) {
                continue;
            }
            // Prefixes also conflict: a single-key shortcut can prevent a
            // longer multi-key sequence from ever completing.
            if (a.key.matches(b.key) != QKeySequence::NoMatch
                || b.key.matches(a.key) != QKeySequence::NoMatch) {
                qCritical("Global shortcuts: '%s' (%s) conflicts with '%s' (%s)",
                    qUtf8Printable(a.label), qUtf8Printable(a.key.toString()),
                    qUtf8Printable(b.label), qUtf8Printable(b.key.toString()));
                valid = false;
            }
        }
    }
    return valid;
}

} // namespace

Outcome dispatchShortcuts(Context& context)
{
    auto& application = context.application;
    auto& window = context.window;
    const int argc = context.argc;
    char** argv = context.argv;

    if (argc == 3
        && std::string_view(argv[1]) == "--menu-shortcuts-smoke-test") {
        // Qt handles escaped && and letter case. Enable extraction on macOS
        // too, where mnemonics default to off; this process only runs the test.
        qt_set_sequence_auto_mnemonic(true);
        const auto check = [&window] {
            const bool menus = checkMenuMnemonics(window.menuBar()->actions(),
                QStringLiteral("Menu bar"), true);
            const bool shortcuts = checkGlobalShortcuts(window);
            return menus && shortcuts;
        };
        if (!check()) {
            return {true, 1};
        }
        // Check again once the Variable and Level menus have been populated.
        QObject::connect(&window, &amrvis::qt::MainWindow::datasetOpenFinished,
            &application, [&application, check](bool success) {
                application.exit(success && check() ? 0 : 1);
            });
        QTimer::singleShot(0, &window,
            [&window, path = std::filesystem::path(argv[2])] {
                window.openDataset(path, true);
            });
    } else {
        return {false, std::nullopt};
    }
    return {true, std::nullopt};
}

} // namespace amrvis::qt::smoke
