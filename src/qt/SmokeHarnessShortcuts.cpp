#include "SmokeHarnessInternal.hpp"

#include "MainWindow.hpp"

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QSet>
#include <QShortcut>
#include <QTimer>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

// Qt documents this switch but only declares it in headers for QDoc.
QT_BEGIN_NAMESPACE
Q_GUI_EXPORT void qt_set_sequence_auto_mnemonic(bool enabled);
QT_END_NAMESPACE

namespace amrvis::qt::smoke {

namespace {

struct ShortcutBinding {
    QKeySequence key;
    QString label;
    const QObject* owner;
    bool mnemonic = false;
};

bool checkMenuMnemonics(const QList<QAction*>& actions, const QString& path,
    std::vector<ShortcutBinding>& menuBindings, bool topLevel = false)
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
            if (topLevel && action->isEnabled() && action->isVisible()) {
                menuBindings.push_back({key,
                    QStringLiteral("Menu %1").arg(label), action, true});
            }
        } else if (topLevel) {
            qCritical("%s: '%s' has no mnemonic", qUtf8Printable(path),
                qUtf8Printable(label));
            valid = false;
        }
        // Include disabled menus: they become reachable after opening data.
        if (const auto* menu = action->menu()) {
            if (!checkMenuMnemonics(menu->actions(),
                    path + QStringLiteral(" > ") + label, menuBindings)) {
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

bool checkGlobalShortcuts(MainWindow& window,
    std::vector<ShortcutBinding> bindings)
{
    // Start with attached actions, not every owned QAction: controllers can
    // retain actions after removing them from a menu. QSet also prevents a
    // shared menu/toolbar action from being counted twice.
    QSet<QAction*> actions;
    const auto collect = [&actions](const QList<QAction*>& entries,
                             auto&& self) -> void {
        for (auto* action : entries) {
            if (!action->isEnabled() || !action->isVisible()
                || actions.contains(action)) {
                continue;
            }
            actions.insert(action);
            if (const auto* menu = action->menu()) {
                // A closed popup still contributes window shortcuts. Its
                // menu action's enabled/visible state controls reachability.
                self(menu->actions(), self);
            }
        }
    };
    collect(window.menuBar()->actions(), collect);
    collect(window.actions(), collect);
    for (const auto* widget : window.findChildren<QWidget*>()) {
        // Separate top-level windows may legitimately reuse window shortcuts.
        if (widget->window() == &window && widget->isEnabled()
            && widget->isVisible()) {
            collect(widget->actions(), collect);
        }
    }

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
        if (shortcut->isEnabled() && widget != nullptr
            && widget->window() == &window && widget->isEnabled()
            && widget->isVisible()
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
    bool valid = true;
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        for (std::size_t j = i + 1; j < bindings.size(); ++j) {
            const auto& a = bindings[i];
            const auto& b = bindings[j];
            // Menu/menu conflicts were already diagnosed by the recursive
            // mnemonic check. Here only compare them with explicit shortcuts.
            if (a.owner == b.owner || (a.mnemonic && b.mnemonic)) {
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
            std::vector<ShortcutBinding> menuBindings;
            const bool menus = checkMenuMnemonics(window.menuBar()->actions(),
                QStringLiteral("Menu bar"), menuBindings, true);
            const bool shortcuts = checkGlobalShortcuts(window,
                std::move(menuBindings));
            return menus && shortcuts;
        };
        if (!check()) {
            return {true, 1};
        }
        // Metadata completion precedes menu population. Only a full initial
        // slice configures the controls and installs the data-driven actions.
        QObject::connect(&window, &amrvis::qt::MainWindow::datasetOpenFinished,
            &application, [&application](bool success) {
                if (!success) {
                    qCritical("Menu shortcut test: dataset metadata failed");
                    application.exit(1);
                }
            });
        QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
            &application, [&application, &window, check](bool success) {
                if (!success) {
                    qCritical("Menu shortcut test: initial slice failed");
                    application.exit(1);
                    return;
                }
                // This fixture has two fields and two AMR levels. Assert the
                // actual menus, so an early signal cannot silently erase the
                // populated-state coverage again.
                const auto menus = window.findChildren<QMenu*>();
                for (const auto* name : {"Variable", "Level"}) {
                    const auto found = std::find_if(menus.begin(), menus.end(),
                        [name](const QMenu* menu) {
                            return QString(menu->title()).remove('&')
                                == QString::fromLatin1(name);
                        });
                    if (found == menus.end() || (*found)->actions().size() < 3) {
                        qCritical("Menu shortcut test: %s menu was not populated",
                            name);
                        application.exit(1);
                        return;
                    }
                }
                application.exit(check() ? 0 : 1);
            });
        QTimer::singleShot(20000, &application, [&application] {
            qCritical("Menu shortcut test: timed out waiting for initial slice");
            application.exit(1);
        });
        QTimer::singleShot(0, &window,
            [&window, path = std::filesystem::path(argv[2])] {
                window.openDataset(path);
            });
    } else {
        return {false, std::nullopt};
    }
    return {true, std::nullopt};
}

} // namespace amrvis::qt::smoke
