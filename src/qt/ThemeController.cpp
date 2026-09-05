#include "ThemeController.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QMenu>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QPainter>
#include <QRectF>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QVariant>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QStyleHints>
#endif

#include <algorithm>
#include <array>
#include <vector>

namespace amrvis::qt {

namespace {

// The style and palette the desktop handed us, snapshotted the first time this
// is called. That call happens in ThemeController's constructor -- i.e. while
// MainWindow is being built, before any skin has been applied -- so a second
// window opened after a switch to Dark cannot mistake the dark palette for the
// system one.
struct SystemDefaults {
    QString styleName;
    QPalette palette;
};

const SystemDefaults& systemDefaults()
{
    static const SystemDefaults defaults{
        QApplication::style() != nullptr ? QApplication::style()->objectName()
                                         : QString(),
        QApplication::palette()};
    return defaults;
}

Skin g_currentSkin = Skin::System;

// Every live controller, so a skin picked in one window re-checks the menus of
// the others. Registration is in the constructor, removal in the destructor.
std::vector<ThemeController*>& liveControllers()
{
    static std::vector<ThemeController*> controllers;
    return controllers;
}

// A dark skin's five colours. Everything else a QPalette needs is derived
// from these, so the four dark skins differ only in this struct and cannot
// drift apart in the roles nobody remembers to set.
struct DarkSkinColors {
    QColor window;   // chrome: menus, toolbars, docks, buttons
    QColor deep;     // what text is entered and listed on
    QColor alternate; // alternating rows
    QColor text;     // on both of the above
    QColor accent;   // selection and links
    QColor muted;    // placeholder and disabled text
};

// Builds the palette. The two-colour QPalette constructor derives the frame
// shades (Light, Midlight, Mid, Dark, Shadow) from the window colour; hand
// listing those is how hand-rolled dark modes end up with invisible frames
// and separators.
QPalette darkSkinPalette(const DarkSkinColors& colors)
{
    QPalette palette(colors.window, colors.window);
    palette.setColor(QPalette::WindowText, colors.text);
    palette.setColor(QPalette::ButtonText, colors.text);
    palette.setColor(QPalette::Base, colors.deep);
    palette.setColor(QPalette::AlternateBase, colors.alternate);
    palette.setColor(QPalette::ToolTipBase, colors.deep);
    palette.setColor(QPalette::ToolTipText, colors.text);
    palette.setColor(QPalette::Text, colors.text);
    palette.setColor(QPalette::Link, colors.accent);
    palette.setColor(QPalette::LinkVisited, colors.accent.lighter(130));
    palette.setColor(QPalette::Highlight, colors.accent);
    // Selected text goes against the accent, not with the skin: a light accent
    // needs the deep colour on it, a dark one needs the text colour. Picking
    // per skin is how one of them ends up unreadable. The test holds every
    // skin's three text pairs to WCAG 2.1 AA (4.5:1); disabled text is exempt
    // under 1.4.3 and is only required to differ from the active colour.
    palette.setColor(QPalette::HighlightedText,
        colors.accent.lightness() > 140 ? colors.deep : colors.text);
    palette.setColor(QPalette::PlaceholderText, colors.muted);
    // The disabled group has to stay distinguishable from the active one, or
    // every grayed-out menu entry and spin box reads as enabled.
    palette.setColor(QPalette::Disabled, QPalette::WindowText, colors.muted);
    palette.setColor(QPalette::Disabled, QPalette::Text, colors.muted);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, colors.muted);
    palette.setColor(QPalette::Disabled, QPalette::Highlight,
        colors.window.lighter(140));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText,
        colors.muted);
    return palette;
}

// The neutral one: the conventional mid-gray Fusion dark set.
constexpr DarkSkinColors darkColors{QColor(0x35, 0x35, 0x35),
    QColor(0x2a, 0x2a, 0x2a), QColor(0x30, 0x30, 0x30), QColor(0xff, 0xff, 0xff),
    QColor(0x2a, 0x82, 0xda), QColor(0x8a, 0x8a, 0x8a)};

// The tinted skins. Each is the neutral set above at a hue: same lightnesses,
// deliberately, because Fusion derives menu grounds from Base and separator
// and check-box outlines from Window, and both derivations are subtle. A
// tinted skin that put Base well below Window -- an obvious way to write one,
// and what the first Blue did -- pushed the whole menu onto a near-black
// ground where those outlines vanished. Keeping the neutral skin's structure
// is what keeps every frame, separator and check box as legible as Dark's.
//
// Blue takes its accent from the application icon's lens (#43C6E8), which is
// the one colour here that is quoted rather than derived.
constexpr DarkSkinColors blueColors{QColor(0x20, 0x35, 0x4a),
    QColor(0x18, 0x2a, 0x3c), QColor(0x1d, 0x30, 0x43), QColor(0xde, 0xe8, 0xf2),
    QColor(0x4a, 0xc3, 0xe2), QColor(0x70, 0x8a, 0xa4)};

constexpr DarkSkinColors greenColors{QColor(0x20, 0x4a, 0x35),
    QColor(0x18, 0x3c, 0x2a), QColor(0x1d, 0x43, 0x30), QColor(0xde, 0xf2, 0xe8),
    QColor(0x5c, 0xd0, 0x96), QColor(0x70, 0xa4, 0x8a)};

constexpr DarkSkinColors maroonColors{QColor(0x4a, 0x20, 0x27),
    QColor(0x3c, 0x18, 0x1e), QColor(0x43, 0x1d, 0x23), QColor(0xf2, 0xde, 0xe1),
    QColor(0xe1, 0x87, 0x96), QColor(0xa4, 0x70, 0x79)};

// Qt >= 6.8 can tell the platform which scheme we are in, which is what makes
// portal-backed native dialogs and standard icons follow the skin. It is an
// addition to the palette work above, not a replacement: the project builds
// against Qt 6.4 too, where the palette is the whole mechanism.
void setColorSchemeHint([[maybe_unused]] Skin skin)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    auto* hints = QGuiApplication::styleHints();
    if (hints == nullptr) {
        return;
    }
    switch (skin) {
    case Skin::System:
        // Unknown is "follow the platform", not "no scheme".
        hints->setColorScheme(Qt::ColorScheme::Unknown);
        break;
    case Skin::Light:
        hints->setColorScheme(Qt::ColorScheme::Light);
        break;
    case Skin::Dark:
    case Skin::Blue:
    case Skin::Green:
    case Skin::Maroon:
        hints->setColorScheme(Qt::ColorScheme::Dark);
        break;
    }
#endif
}

// Installs a style by factory name, keeping the current one if the name is not
// one this Qt can build. QApplication takes ownership of the new style and
// deletes the old, which is why only the name of the system style is kept.
void setStyleByName(const QString& name)
{
    if (name.isEmpty()) {
        return;
    }
    if (auto* style = QStyleFactory::create(name)) {
        QApplication::setStyle(style);
    }
}

// Fusion draws menu and toolbar separators with an outline it derives from the
// window colour itself (window.darker(140) on a dark palette), which no
// palette role can reach -- on any dark skin it lands around 1.15:1 and all
// but disappears. Overriding that one primitive is the whole of this class.
//
// A style, not a stylesheet: an application stylesheet with a
// QMenu::separator rule does make the rule visible, but it hands the whole
// menu to QStyleSheetStyle, which then drops the sub-elements it has no rule
// for -- the submenu arrows and the unchecked check boxes both vanished.
class SeparatorStyle final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
        QPainter* painter, const QWidget* widget) const override
    {
        if (element == PE_IndicatorToolBarSeparator) {
            // The separator's line is perpendicular to the toolbar's axis.
            paintSeparator(option, painter,
                (option->state & State_Horizontal) != 0
                    ? Qt::Vertical : Qt::Horizontal);
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
        // Fusion frames check boxes and radio buttons with the same
        // disappearing outline. Overdrawing the frame keeps its check mark and
        // its fill and only makes the empty box findable; drawing the whole
        // indicator here would mean reproducing the mark as well.
        if ((element == PE_IndicatorCheckBox
                || element == PE_IndicatorRadioButton)
            && (option->state & State_Enabled) != 0) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(rule(option));
            painter->setBrush(Qt::NoBrush);
            const QRectF box = QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5);
            if (element == PE_IndicatorCheckBox) {
                painter->drawRoundedRect(box, 2, 2);
            } else {
                painter->drawEllipse(box);
            }
            painter->restore();
        }
    }

    void drawControl(ControlElement element, const QStyleOption* option,
        QPainter* painter, const QWidget* widget) const override
    {
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (element == CE_MenuItem && item != nullptr
            && item->menuItemType == QStyleOptionMenuItem::Separator) {
            paintSeparator(option, painter, Qt::Horizontal);
            return;
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }

private:
    // Lighter than the chrome, which is the direction that reads on a dark
    // ground; Fusion's own outline goes darker and disappears into it.
    static QColor rule(const QStyleOption* option)
    {
        return option->palette.window().color().lighter(165);
    }

    static void paintSeparator(
        const QStyleOption* option, QPainter* painter, Qt::Orientation lineAxis)
    {
        const auto line = rule(option);
        const auto rect = option->rect;
        if (rect.isEmpty()) {
            return;
        }
        const int length = lineAxis == Qt::Horizontal ? rect.width() : rect.height();
        // Retain at least one pixel even in a compressed separator rectangle.
        const int inset = std::min(kInset, (length - 1) / 2);
        if (lineAxis == Qt::Horizontal) {
            const int y = rect.center().y();
            painter->fillRect(rect.left() + inset, y,
                rect.width() - 2 * inset, 1, line);
        } else {
            const int x = rect.center().x();
            painter->fillRect(x, rect.top() + inset, 1,
                rect.height() - 2 * inset, line);
        }
    }

    static constexpr int kInset = 4;
};

// The one list of skins: the settings key and the menu label of each, in menu
// order. The switches above and below are what force a new enumerator to be
// handled; this is what makes it reachable.
struct SkinEntry {
    Skin skin;
    const char* key;
    const char* label;
};

constexpr std::array<SkinEntry, 6> skinEntries{
    SkinEntry{Skin::System, "system", "&System"},
    SkinEntry{Skin::Light, "light", "&Light"},
    SkinEntry{Skin::Dark, "dark", "&Dark"},
    SkinEntry{Skin::Blue, "blue", "&Blue"},
    SkinEntry{Skin::Green, "green", "&Green"},
    SkinEntry{Skin::Maroon, "maroon", "&Maroon"}};

} // namespace

Skin currentSkin()
{
    return g_currentSkin;
}

QString skinKey(Skin skin)
{
    const auto entry = std::ranges::find(skinEntries, skin, &SkinEntry::skin);
    return entry != skinEntries.end() ? QString::fromLatin1(entry->key)
                                      : QStringLiteral("system");
}

Skin skinFromKey(const QString& key)
{
    const auto entry = std::ranges::find_if(skinEntries,
        [&key](const SkinEntry& candidate) {
            return key == QLatin1String(candidate.key);
        });
    return entry != skinEntries.end() ? entry->skin : Skin::System;
}

bool applySkin(Skin skin)
{
    // Snapshot before the first override, whatever the caller asked for.
    const auto& defaults = systemDefaults();
    if (skin == g_currentSkin) {
        return false;
    }
    g_currentSkin = skin;
    // The hint goes first: on Qt >= 6.8 it is itself a theme change, and
    // letting it land after our palette would undo it.
    setColorSchemeHint(skin);
    const auto dark = [](const DarkSkinColors& colors) {
        // The proxy wraps a fresh Fusion; QApplication takes both.
        QApplication::setStyle(
            new SeparatorStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
        QApplication::setPalette(darkSkinPalette(colors));
    };
    switch (skin) {
    case Skin::System:
        setStyleByName(defaults.styleName);
        QApplication::setPalette(defaults.palette);
        break;
    case Skin::Light:
        setStyleByName(QStringLiteral("Fusion"));
        // Fusion's own light palette, rather than a hand-authored one.
        QApplication::setPalette(QApplication::style()->standardPalette());
        break;
    case Skin::Dark:
        dark(darkColors);
        break;
    case Skin::Blue:
        dark(blueColors);
        break;
    case Skin::Green:
        dark(greenColors);
        break;
    case Skin::Maroon:
        dark(maroonColors);
        break;
    }
    for (auto* controller : liveControllers()) {
        controller->syncMenu();
    }
    return true;
}

ThemeController::ThemeController(QObject* parent)
    : QObject(parent)
{
    // Forces the snapshot now, while the desktop's style and palette are still
    // the ones in effect.
    static_cast<void>(systemDefaults());
    liveControllers().push_back(this);
}

ThemeController::~ThemeController()
{
    auto& controllers = liveControllers();
    controllers.erase(
        std::remove(controllers.begin(), controllers.end(), this),
        controllers.end());
}

QMenu* ThemeController::createMenu(QWidget* parent)
{
    auto* menu = new QMenu(tr("S&kin"), parent);
    m_menuGroup = new QActionGroup(menu);
    for (const auto& entry : skinEntries) {
        auto* action = new QAction(tr(entry.label), menu);
        action->setCheckable(true);
        action->setActionGroup(m_menuGroup);
        action->setData(static_cast<int>(entry.skin));
        // triggered, not toggled: syncMenu's setChecked must not re-enter.
        connect(action, &QAction::triggered, this,
            [this, skin = entry.skin] { selectSkin(skin); });
        menu->addAction(action);
    }
    syncMenu();
    return menu;
}

void ThemeController::selectSkin(Skin skin)
{
    if (!applySkin(skin)) {
        // Already applied; the click that landed here unchecked nothing, but
        // resync so a stray state cannot survive.
        syncMenu();
        return;
    }
    emit skinChanged();
}

void ThemeController::restore(const QSettings& settings)
{
    applySkin(skinFromKey(
        settings.value(QStringLiteral("theme/skin")).toString()));
}

void ThemeController::save(QSettings& settings) const
{
    settings.setValue(QStringLiteral("theme/skin"), skinKey(currentSkin()));
}

QStringList ThemeController::menuLabels() const
{
    QStringList labels;
    if (!m_menuGroup) {
        return labels;
    }
    for (const auto* action : m_menuGroup->actions()) {
        labels.append(action->text());
    }
    return labels;
}

QString ThemeController::checkedMenuLabel() const
{
    if (!m_menuGroup) {
        return {};
    }
    for (const auto* action : m_menuGroup->actions()) {
        if (action->isChecked()) {
            return action->text();
        }
    }
    return {};
}

void ThemeController::syncMenu()
{
    if (!m_menuGroup) {
        return;
    }
    for (auto* action : m_menuGroup->actions()) {
        action->setChecked(action->data().toInt()
            == static_cast<int>(currentSkin()));
    }
}

} // namespace amrvis::qt
