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
    for (int slot = 0; slot < amrvis::Palette::slotCount; ++slot) {
        if (left.slotArgb(slot) != right.slotArgb(slot)) {
            return false;
        }
    }
    return true;
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
    using amrvis::qt::builtinPaletteLabel;

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
            controller.restore(settings);
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
            controller.restore(settings);
            require(controller.state().fromFile
                    && controller.state().filePath == path
                    && controller.palette().slotArgb(200) == 0xFFC80000U,
                "restore did not reload the file palette");
        }
        require(QFile::remove(path), "could not remove the palette fixture");
        {
            QSettings settings(settingsPath, QSettings::IniFormat);
            PaletteController controller;
            controller.restore(settings);
            require(!controller.state().fromFile
                    && controller.state().builtinIndex == 4,
                "a vanished file palette did not fall back to the builtin");
        }
        {
            QSettings settings(settingsPath, QSettings::IniFormat);
            settings.setValue(QStringLiteral("palette/fromFile"), false);
            settings.setValue(QStringLiteral("palette/builtin"),
                QStringLiteral("no-such-palette"));
            PaletteController controller;
            controller.restore(settings);
            require(controller.state().builtinIndex == 0,
                "an unknown builtin name did not fall back to the first");
        }
    }

    // apply(State) is the viewer-state import path: it restores a selection
    // wholesale and does emit, since consumers are wired by then.
    {
        PaletteController controller;
        int changes = 0;
        QObject::connect(&controller, &PaletteController::paletteChanged,
            [&changes] { ++changes; });
        controller.apply(PaletteController::State{6, false, {}, true});
        require(changes == 1 && controller.state().builtinIndex == 6
                && controller.state().reversed,
            "apply did not install the state");
        controller.apply(PaletteController::State{
            2, true, QStringLiteral("/nonexistent/palette.pal"), false});
        require(changes == 2 && !controller.state().fromFile
                && controller.state().builtinIndex == 2,
            "apply with an unloadable file did not fall back");
    }

    std::cout << "palette controller tests passed\n";
    return 0;
}
