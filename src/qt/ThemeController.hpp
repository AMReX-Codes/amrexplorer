#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QActionGroup;
class QMenu;
class QSettings;
class QWidget;

namespace amrvis::qt {

// The application's chrome appearance: menus, docks, dialogs, tables and
// toolbars. System is the default and reproduces the behaviour this
// application had before skins existed -- whatever Qt style and palette the
// desktop provides, untouched. Blue, Green and Maroon are Dark in a tint --
// Blue in the application icon's own colours.
//
// Adding one here is a compile error until every switch over Skin in
// ThemeController.cpp handles it and skinEntries there lists it; that is the
// intended guard, so do not give those switches a default case.
enum class Skin { System, Light, Dark, Blue, Green, Maroon };

// The skin currently applied to QApplication. Application-wide, not per
// window: File > New Window makes a second MainWindow, and both share one
// QApplication palette.
[[nodiscard]] Skin currentSkin();

// Applies a skin to QApplication and returns whether anything changed. Qt
// propagates the new palette to every widget that has not set an explicit one,
// so the switch is live -- no restart, no rebuilding of windows. The viewports
// (see Theme.hpp) do set explicit palettes and brushes, and so keep their
// neutral gray under every skin, which is intended.
//
// System restores the style and palette captured the first time a skin was
// applied. That is a best effort rather than a true reset: once
// QApplication::setPalette has been called, Qt stops following later desktop
// theme changes for the rest of the session.
bool applySkin(Skin skin);

// The settings string for a skin ("system", "light", "dark"). This value is on
// disk in every user's settings, so renaming one is a settings-format change.
[[nodiscard]] QString skinKey(Skin skin);
// The inverse; anything unrecognised falls back to System.
[[nodiscard]] Skin skinFromKey(const QString& key);

// The skin selection: the Skin menu that presents it, its QSettings
// persistence, and the application of the choice. Owned by its host window the
// way PaletteController is, but the state it drives lives in QApplication, so
// several instances stay in step: every live controller re-syncs its menu when
// any of them changes the skin.
class ThemeController final : public QObject {
    Q_OBJECT

public:
    explicit ThemeController(QObject* parent = nullptr);
    ~ThemeController() override;

    // The "S&kin" menu: one checkable action per skin in an exclusive group.
    // Owned by `parent`; the controller keeps its checks in step.
    QMenu* createMenu(QWidget* parent);

    [[nodiscard]] Skin skin() const { return currentSkin(); }

    // Re-selecting the applied skin resyncs the menu but emits nothing, so it
    // costs no settings write.
    void selectSkin(Skin skin);

    // The "theme/skin" application setting. restore() applies the stored skin
    // but does not emit skinChanged: it reproduces a saved selection rather
    // than changing one, so a host that has wired the signal to saveSettings
    // is not made to write the settings straight back.
    void restore(const QSettings& settings);
    void save(QSettings& settings) const;

    // The menu action labels, in menu order. Empty until the menu exists.
    [[nodiscard]] QStringList menuLabels() const;
    // The label of the checked action, empty if none is.
    [[nodiscard]] QString checkedMenuLabel() const;

signals:
    // The applied skin changed; the host persists settings.
    void skinChanged();

private:
    // applySkin re-checks every live controller's menu, this one included.
    friend bool applySkin(Skin skin);

    // Re-checks the menu action matching currentSkin(). Never emits, so
    // applySkin can call it on every live controller.
    void syncMenu();

    QPointer<QActionGroup> m_menuGroup;
};

} // namespace amrvis::qt
