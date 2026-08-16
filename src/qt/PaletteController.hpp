#pragma once

#include <amrexplorer/render2d/Palette.hpp>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <array>
#include <cstddef>
#include <optional>

class QAction;
class QActionGroup;
class QComboBox;
class QMenu;
class QSettings;
class QWidget;

namespace amrvis::qt {

inline constexpr std::array<BuiltinPalette, 7> builtinPalettes{
    BuiltinPalette::Rainbow, BuiltinPalette::Turbo, BuiltinPalette::Viridis,
    BuiltinPalette::Plasma, BuiltinPalette::Parula, BuiltinPalette::Coolwarm,
    BuiltinPalette::Blackbody};

// This array is what the Palette menu, the selector and settings restore all
// enumerate, so a palette missing from it exists in render2d and nowhere a user
// can reach. test_palette pins the seven names but cannot catch that: adding an
// enumerator leaves this at seven and every name it does check still matches.
static_assert(builtinPalettes.size()
        == static_cast<std::size_t>(BuiltinPalette::Count),
    "every BuiltinPalette must be listed in builtinPalettes");

// The QSettings value for a builtin palette. This string is on disk in every
// user's settings, and it comes from amrvis::builtinPaletteName(), which
// render2d documents as the menu label *and* the settings key -- so renaming a
// palette there is a settings-format change, not a presentation tweak.
// test_palette pins the seven strings so that cannot happen silently.
inline QString builtinPaletteKey(std::size_t index)
{
    if (index >= builtinPalettes.size()) {
        return {};
    }
    const auto name = builtinPaletteName(builtinPalettes[index]);
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

// The palette's name for the menu and the selector, and the one definition of
// its capitalization -- where the settings key above stays lowercase. Not the
// only presentation rule: the selector still appends the "_r" reversal suffix
// to its labels alone, so with reversal on the two surfaces differ by that
// suffix. Kept apart from the key precisely so this rule can live here without
// rewriting anyone's stored settings.
inline QString builtinPaletteLabel(std::size_t index)
{
    auto label = builtinPaletteKey(index);
    if (!label.isEmpty()) {
        label[0] = label[0].toUpper();
    }
    return label;
}

// The palette selection extracted from MainWindow: which palette is chosen (a
// builtin by index, or a legacy .pal file), whether it is reversed, the
// effective Palette the renderer, color bar and overlays use, its QSettings
// persistence, and the two widgets that present and drive it -- the Palette
// menu and the toolbar selector. The host builds those widgets into its chrome
// through createMenu/createSelector, pushes the palette to its consumers when
// paletteChanged fires, and runs the file dialog when loadFileRequested fires.
//
// State is the explicit, serialisable selection; the Palette itself is derived
// from it and never stored anywhere else, which is what lets a viewer-state
// export/import restore a palette by round-tripping State alone.
class PaletteController final : public QObject {
    Q_OBJECT

public:
    struct State {
        int builtinIndex = 0;
        bool fromFile = false;
        QString filePath;
        bool reversed = false;
    };

    explicit PaletteController(QObject* parent = nullptr);

    // The "&Palette" menu: one checkable action per builtin, the Reverse
    // Colormap toggle, and Load Palette File... (which emits loadFileRequested).
    // Owned by `parent`; the controller keeps its checks in step with the state.
    QMenu* createMenu(QWidget* parent);
    // The toolbar selector: the builtins (with the "_r" suffix while reversal
    // is on), a transient "Custom: <file>" entry while a file palette is
    // active, and a Reverse Colormap toggle item anchored at the bottom. Owned
    // by `parent`; the controller keeps it in step with the state.
    QComboBox* createSelector(QWidget* parent);

    // The effective palette: the selection with reversal applied. The
    // reference is stable for the controller's lifetime, so consumers that
    // hold a pointer (the color bar) need not be re-pointed on every change.
    [[nodiscard]] const Palette& palette() const noexcept { return m_palette; }
    [[nodiscard]] const State& state() const noexcept { return m_state; }

    void selectBuiltin(int index);
    // Loads a legacy palette file and makes it the selection. On failure the
    // selection is unchanged and the error text is returned for the host to
    // show.
    [[nodiscard]] std::optional<QString> loadFile(const QString& path);
    void setReversed(bool reversed);
    // Restores a whole selection (settings restore, viewer-state import). A
    // file palette that no longer loads falls back to the builtin index the
    // state carries, so the caller always ends up with a usable palette.
    void apply(const State& state);

    // The palette keys of the application settings ("palette/…"). restore()
    // does not emit paletteChanged: it runs before the host's consumers exist,
    // and the host reads palette() itself once it has built them.
    void restore(const QSettings& settings);
    void save(QSettings& settings) const;

    // The builtin names as the menu actions and the selector items show them
    // (the selector's with the reversal suffix, if on). Empty until the widget
    // exists. For tests and diagnostics: the two must agree on every name.
    [[nodiscard]] QStringList menuLabels() const;
    [[nodiscard]] QStringList selectorLabels() const;

signals:
    // The effective palette changed (a new selection or the reversal toggle):
    // the host pushes it to the color bar, overlays and a re-render, and
    // persists settings.
    void paletteChanged();
    // The menu's Load Palette File... was chosen; the host runs its dialog and
    // calls loadFile.
    void loadFileRequested();

private:
    // Installs a selection and its base palette, syncs the widgets, derives
    // the effective palette and emits paletteChanged.
    void install(Palette basePalette, State state);
    void syncMenu();
    void syncSelector();

    // The selected palette before any reversal; m_palette is derived from it as
    // m_state.reversed ? m_basePalette.reversed() : m_basePalette.
    Palette m_basePalette = builtinPalette(BuiltinPalette::Rainbow);
    Palette m_palette = builtinPalette(BuiltinPalette::Rainbow);
    State m_state;
    QPointer<QActionGroup> m_menuGroup;
    QPointer<QAction> m_reverseAction;
    QPointer<QComboBox> m_selector;
};

} // namespace amrvis::qt
