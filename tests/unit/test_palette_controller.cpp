#include "PaletteController.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool samePalette(const amrvis::Palette& left, const amrvis::Palette& right)
{
    // Slots and alpha ramp alike: operator== compares both planes.
    return left == right;
}

// The checked action of the menu group, or -1 (none checked: a file palette).
int checkedMenuIndex(const QMenu& menu)
{
    const auto* group = menu.findChild<QActionGroup*>();
    if (group == nullptr) {
        return -2;
    }
    const auto actions = group->actions();
    for (int index = 0; index < actions.size(); ++index) {
        if (actions[index]->isChecked()) {
            return index;
        }
    }
    return -1;
}

const QAction* actionNamed(const QMenu& menu, const QString& text)
{
    for (const auto* action : menu.actions()) {
        if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

// A legacy sequential palette file: 768 bytes, red ramp only.
QString writePaletteFile(const QTemporaryDir& dir, const char* name)
{
    const auto path = dir.filePath(QString::fromLatin1(name));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "could not write palette fixture");
    QByteArray bytes(768, '\0');
    for (int slot = 0; slot < 256; ++slot) {
        bytes[slot] = static_cast<char>(slot);
    }
    require(file.write(bytes) == bytes.size(), "short palette fixture write");
    return path;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    using amrvis::qt::PaletteController;
    using amrvis::qt::builtinPalettes;

    // Menu and selector present the same builtin names, in order, and the
    // first is "Rainbow" (pinned as a literal, not derived from the helper).
    {
        PaletteController controller;
        QWidget host;
        auto* menu = controller.createMenu(&host);
        auto* selector = controller.createSelector(&host);
        const auto menuLabels = controller.menuLabels();
        require(menuLabels.size() == static_cast<int>(builtinPalettes.size())
                && menuLabels == controller.selectorLabels(),
            "menu and selector disagree on the builtin names");
        require(menuLabels.front() == QStringLiteral("Rainbow"),
            "the first palette is not labelled Rainbow");
        require(checkedMenuIndex(*menu) == 0
                && selector->currentData().toInt() == 0,
            "the initial selection is not the first builtin");
        require(samePalette(controller.palette(),
                    amrvis::builtinPalette(amrvis::BuiltinPalette::Rainbow)),
            "the initial palette is not Rainbow");
    }

    // Selecting a builtin: state, palette, both widgets, one signal. The
    // widgets drive it too: the selector's current-index change and the menu
    // action both route to the same selection.
    {
        PaletteController controller;
        QWidget host;
        auto* menu = controller.createMenu(&host);
        auto* selector = controller.createSelector(&host);
        int changes = 0;
        QObject::connect(&controller, &PaletteController::paletteChanged,
            [&changes] { ++changes; });
        controller.selectBuiltin(3);
        require(changes == 1, "selectBuiltin did not emit paletteChanged once");
        require(controller.state().builtinIndex == 3
                && !controller.state().fromFile
                && controller.state().filePath.isEmpty(),
            "selectBuiltin left the wrong state");
        require(samePalette(controller.palette(),
                    amrvis::builtinPalette(builtinPalettes[3])),
            "selectBuiltin did not install the palette");
        require(checkedMenuIndex(*menu) == 3
                && selector->currentData().toInt() == 3,
            "selectBuiltin did not sync the widgets");
        controller.selectBuiltin(99);
        controller.selectBuiltin(-1);
        require(changes == 1 && controller.state().builtinIndex == 3,
            "an out-of-range builtin index was accepted");
        selector->setCurrentIndex(selector->findData(5));
        require(changes == 2 && controller.state().builtinIndex == 5
                && checkedMenuIndex(*menu) == 5,
            "the selector did not drive the selection");
        menu->findChild<QActionGroup*>()->actions()[1]->trigger();
        require(changes == 3 && controller.state().builtinIndex == 1
                && selector->currentData().toInt() == 1,
            "the menu action did not drive the selection");
    }

    // Reversal: the effective palette flips, the selector labels carry "_r"
    // and its toggle item a check mark, the menu action follows, and the
    // menu's names stay bare. Setting the same value again is a no-op.
    {
        PaletteController controller;
        QWidget host;
        auto* menu = controller.createMenu(&host);
        auto* selector = controller.createSelector(&host);
        int changes = 0;
        QObject::connect(&controller, &PaletteController::paletteChanged,
            [&changes] { ++changes; });
        controller.setReversed(true);
        require(changes == 1 && controller.state().reversed,
            "setReversed did not take");
        require(samePalette(controller.palette(),
                    amrvis::builtinPalette(amrvis::BuiltinPalette::Rainbow)
                        .reversed()),
            "the reversed palette is not the base palette reversed");
        for (const auto& label : controller.selectorLabels()) {
            require(label.endsWith(QStringLiteral("_r")),
                "a selector label lacks the reversal suffix");
        }
        require(controller.menuLabels().front() == QStringLiteral("Rainbow"),
            "the menu label picked up the selector's suffix");
        require(selector->itemText(selector->findData(-3))
                    .startsWith(QStringLiteral("✓")),
            "the reverse toggle item shows no check mark");
        require(actionNamed(*menu, QStringLiteral("&Reverse Colormap"))
                    ->isChecked(),
            "the menu's reverse action is not checked");
        require(selector->currentData().toInt() == 0,
            "reversal moved the selector off the palette");
        controller.setReversed(true);
        require(changes == 1, "a no-op setReversed emitted paletteChanged");
        // The selector's toggle item flips it back and restores the current
        // index to the palette.
        selector->setCurrentIndex(selector->findData(-3));
        require(changes == 2 && !controller.state().reversed
                && selector->currentData().toInt() == 0,
            "the selector's reverse toggle did not flip and restore");
        require(controller.selectorLabels().front() == QStringLiteral("Rainbow"),
            "the reversal suffix was not removed");
    }

    // File palettes: a bad path is reported and changes nothing; a good one
    // becomes a "Custom: <file>" entry with no builtin checked, and reversal
    // applies on top of it. Selecting a builtin again drops the entry.
    {
        QTemporaryDir dir;
        require(dir.isValid(), "no temporary directory");
        PaletteController controller;
        QWidget host;
        auto* menu = controller.createMenu(&host);
        auto* selector = controller.createSelector(&host);
        int changes = 0;
        QObject::connect(&controller, &PaletteController::paletteChanged,
            [&changes] { ++changes; });
        const auto error = controller.loadFile(dir.filePath("missing.pal"));
        require(error.has_value() && changes == 0
                && !controller.state().fromFile,
            "a missing palette file was accepted");
        const auto path = writePaletteFile(dir, "ramp.pal");
        require(!controller.loadFile(path).has_value() && changes == 1,
            "a valid palette file was rejected");
        require(controller.state().fromFile
                && controller.state().filePath == path,
            "loadFile left the wrong state");
        require(controller.palette().slotArgb(200) == 0xFFC80000U,
            "the file palette's ramp was not installed");
        require(checkedMenuIndex(*menu) == -1,
            "a builtin stayed checked under a file palette");
        require(selector->currentData().toInt() == -2
                && selector->currentText()
                    == QStringLiteral("Custom: ramp.pal"),
            "the selector does not show the custom entry");
        controller.setReversed(true);
        require(controller.palette().slotArgb(200) != 0xFFC80000U
                && selector->currentText()
                    == QStringLiteral("Custom: ramp.pal_r"),
            "reversal did not apply on top of the file palette");
        controller.selectBuiltin(2);
        require(selector->findData(-2) < 0 && checkedMenuIndex(*menu) == 2,
            "selecting a builtin did not drop the custom entry");
        // Load Palette File... asks the host; it does not load anything itself.
        bool asked = false;
        QObject::connect(&controller, &PaletteController::loadFileRequested,
            [&asked] { asked = true; });
        const_cast<QAction*>(
            actionNamed(*menu, QStringLiteral("&Load Palette File...")))
            ->trigger();
        require(asked, "Load Palette File... did not ask the host");
    }

    // Settings round trip through the "palette/…" keys; restore is silent; a
    // file that no longer loads falls back to the stored builtin; an unknown
    // builtin name falls back to the first.
    {
        QTemporaryDir dir;
        require(dir.isValid(), "no temporary directory");
        const auto path = writePaletteFile(dir, "saved.pal");
        const auto settingsPath = dir.filePath("settings.ini");
        {
            QSettings settings(settingsPath, QSettings::IniFormat);
            PaletteController controller;
            controller.selectBuiltin(4);
            controller.setReversed(true);
            controller.save(settings);
            require(settings.value(QStringLiteral("palette/builtin")).toString()
                        == QStringLiteral("parula")
                    && settings.value(QStringLiteral("palette/reversed")).toBool()
                    && !settings.value(QStringLiteral("palette/fromFile"))
                            .toBool(),
                "save wrote the wrong keys");
        }
        {
            QSettings settings(settingsPath, QSettings::IniFormat);
            PaletteController controller;
            int changes = 0;
            QObject::connect(&controller, &PaletteController::paletteChanged,
                [&changes] { ++changes; });
            require(!controller.restore(settings).has_value(),
                "restore of a builtin selection reported an error");
            require(changes == 0, "restore emitted paletteChanged");
            require(controller.state().builtinIndex == 4
                    && controller.state().reversed
                    && !controller.state().fromFile,
                "restore did not reproduce the saved builtin selection");
            require(samePalette(controller.palette(),
                        amrvis::builtinPalette(builtinPalettes[4]).reversed()),
                "restore did not derive the reversed palette");
        }
        {
            // A file palette on top of builtin 4: the state keeps carrying the
            // builtin, which is what the fallback below returns to.
            QSettings settings(settingsPath, QSettings::IniFormat);
            PaletteController controller;
            controller.selectBuiltin(4);
            require(!controller.loadFile(path).has_value(), "fixture load");
            controller.setReversed(false);
            controller.save(settings);
        }
        {
            QSettings settings(settingsPath, QSettings::IniFormat);
            PaletteController controller;
            require(!controller.restore(settings).has_value()
                    && controller.state().fromFile
                    && controller.state().filePath == path
                    && controller.palette().slotArgb(200) == 0xFFC80000U,
                "restore did not reload the file palette");
        }
        require(QFile::remove(path), "could not remove the palette fixture");
        {
            // A vanished file: the builtin the state carries is shown, the
            // failure is reported -- and the file stays the wanted selection
            // on disk through later, unrelated saves, so a transient failure
            // does not drop the preference.
            QSettings settings(settingsPath, QSettings::IniFormat);
            PaletteController controller;
            const auto error = controller.restore(settings);
            require(error.has_value() && !error->isEmpty()
                    && !controller.state().fromFile
                    && controller.state().builtinIndex == 4,
                "a vanished file palette did not fall back to the builtin");
            controller.setReversed(true);
            controller.save(settings);
            require(settings.value(QStringLiteral("palette/fromFile")).toBool()
                    && settings.value(QStringLiteral("palette/filePath"))
                            .toString()
                        == path
                    && settings.value(QStringLiteral("palette/reversed"))
                           .toBool(),
                "a failed file load was persisted as a dropped preference");
        }
        {
            // The file is back: the kept preference loads again.
            require(writePaletteFile(dir, "saved.pal") == path, "fixture path");
            QSettings settings(settingsPath, QSettings::IniFormat);
            PaletteController controller;
            require(!controller.restore(settings).has_value()
                    && controller.state().fromFile
                    && controller.state().filePath == path
                    && controller.state().reversed,
                "the kept file preference did not load once available");
        }
        {
            // An explicit selection replaces the wanted file -- even the
            // builtin the failed file fell back to, which changes nothing
            // visible. That still has to emit: the host persists only on
            // paletteChanged, and what save() writes just changed.
            require(QFile::remove(path), "could not remove the palette fixture");
            QSettings settings(settingsPath, QSettings::IniFormat);
            PaletteController controller;
            require(controller.restore(settings).has_value()
                    && controller.state().builtinIndex == 4,
                "fixture restore");
            int changes = 0;
            QObject::connect(&controller, &PaletteController::paletteChanged,
                [&changes] { ++changes; });
            controller.selectBuiltin(4);
            require(changes == 1,
                "re-selecting the fallback builtin did not emit paletteChanged");
            controller.selectBuiltin(4);
            require(changes == 1,
                "re-selecting the builtin again emitted paletteChanged");
            controller.save(settings);
            require(!settings.value(QStringLiteral("palette/fromFile")).toBool()
                    && settings.value(QStringLiteral("palette/filePath"))
                           .toString()
                           .isEmpty(),
                "selecting a builtin did not replace the wanted file");
        }
        {
            QSettings settings(settingsPath, QSettings::IniFormat);
            settings.setValue(QStringLiteral("palette/fromFile"), false);
            settings.setValue(QStringLiteral("palette/builtin"),
                QStringLiteral("no-such-palette"));
            PaletteController controller;
            require(!controller.restore(settings).has_value()
                    && controller.state().builtinIndex == 0,
                "an unknown builtin name did not fall back to the first");
        }
    }

    // apply(State) is the viewer-state import path: it restores a selection
    // wholesale and does emit, since consumers are wired by then. A builtin
    // index it does not have falls back to the first, as restore does for an
    // unknown name.
    {
        PaletteController controller;
        int changes = 0;
        QObject::connect(&controller, &PaletteController::paletteChanged,
            [&changes] { ++changes; });
        require(!controller.apply(PaletteController::State{6, false, {}, true})
                    .has_value(),
            "apply of a builtin state reported an error");
        require(changes == 1 && controller.state().builtinIndex == 6
                && controller.state().reversed,
            "apply did not install the state");
        require(controller.apply(PaletteController::State{2, true,
                    QStringLiteral("/nonexistent/palette.pal"), false})
                    .has_value(),
            "apply with an unloadable file did not report it");
        require(changes == 2 && !controller.state().fromFile
                && controller.state().builtinIndex == 2,
            "apply with an unloadable file did not fall back");
        require(!controller.apply(PaletteController::State{99, false, {}, false})
                    .has_value()
                && changes == 3 && controller.state().builtinIndex == 0,
            "apply with an unknown builtin index did not fall back");
        // (Reversed, so the fallback is a change from the one just above.)
        require(!controller.apply(PaletteController::State{-1, false, {}, true})
                    .has_value()
                && changes == 4 && controller.state().builtinIndex == 0
                && controller.state().reversed,
            "apply with a negative builtin index did not fall back");
        // A state without a file replaces the wanted file an earlier failed
        // apply kept (see the settings round trip above for the keeping).
        QTemporaryDir dir;
        require(dir.isValid(), "no temporary directory");
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        controller.save(settings);
        require(!settings.value(QStringLiteral("palette/fromFile")).toBool()
                && settings.value(QStringLiteral("palette/filePath"))
                       .toString()
                       .isEmpty(),
            "a later fileless apply did not replace the wanted file");
    }

    // Re-selecting what is already selected is not a change: the checked
    // menu action (which the ExclusiveOptional group has just unchecked) is
    // re-checked, but nothing is emitted, so the host neither writes settings
    // nor re-renders. Reloading the same file path is a change only if the
    // file's contents changed.
    {
        QTemporaryDir dir;
        require(dir.isValid(), "no temporary directory");
        PaletteController controller;
        QWidget host;
        auto* menu = controller.createMenu(&host);
        auto* selector = controller.createSelector(&host);
        int changes = 0;
        QObject::connect(&controller, &PaletteController::paletteChanged,
            [&changes] { ++changes; });
        controller.selectBuiltin(3);
        require(changes == 1, "fixture selection");
        controller.selectBuiltin(3);
        require(changes == 1 && checkedMenuIndex(*menu) == 3,
            "re-selecting the current builtin emitted paletteChanged");
        auto* checked = menu->findChild<QActionGroup*>()->actions()[3];
        checked->trigger();
        require(changes == 1 && checked->isChecked()
                && checkedMenuIndex(*menu) == 3
                && selector->currentData().toInt() == 3,
            "clicking the checked menu action emitted or unchecked it");
        require(!controller.apply(controller.state()).has_value()
                && changes == 1,
            "re-applying the current state emitted paletteChanged");
        const auto path = writePaletteFile(dir, "same.pal");
        require(!controller.loadFile(path).has_value() && changes == 2,
            "loading a file did not emit");
        require(!controller.loadFile(path).has_value() && changes == 2,
            "reloading the unchanged file emitted paletteChanged");
        {
            // Edit the file in place: the same path now holds a green ramp.
            QFile file(path);
            require(file.open(QIODevice::WriteOnly), "could not rewrite");
            QByteArray bytes(768, '\0');
            for (int slot = 0; slot < 256; ++slot) {
                bytes[256 + slot] = static_cast<char>(slot);
            }
            require(file.write(bytes) == bytes.size(), "short rewrite");
        }
        require(!controller.loadFile(path).has_value() && changes == 3
                && controller.palette().slotArgb(200) == 0xFF00C800U,
            "reloading an edited file did not install the new contents");
    }

    std::cout << "palette controller tests passed\n";
    return 0;
}
