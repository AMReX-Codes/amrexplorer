#include "ThemeController.hpp"
#include "Theme.hpp"
#include "ExpressionEditorDialog.hpp"
#include "SetContoursDialog.hpp"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPalette>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QToolBar>
#include <QWidget>

#include <algorithm>
#include <cmath>
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

bool isDark(const QPalette& palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

// WCAG relative luminance, so a skin cannot pass by being merely different
// from its background -- 4.5:1 is the readable-body-text threshold.
double relativeLuminance(const QColor& color)
{
    const auto channel = [](double value) {
        value /= 255.0;
        return value <= 0.03928 ? value / 12.92
                                : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.red()) + 0.7152 * channel(color.green())
        + 0.0722 * channel(color.blue());
}

double contrast(const QColor& left, const QColor& right)
{
    const auto first = relativeLuminance(left);
    const auto second = relativeLuminance(right);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

void checkToolbarSeparator(Qt::Orientation orientation, QSize size = {})
{
    QToolBar toolbar;
    toolbar.setOrientation(orientation);
    toolbar.addAction(QStringLiteral("First"));
    auto* separator = toolbar.addSeparator();
    toolbar.addAction(QStringLiteral("Second"));
    toolbar.show();
    QApplication::processEvents();

    // Use the real toolbar's separator dimensions, but paint the primitive
    // alone so its pixels cannot be confused with the toolbar background.
    QStyleOption option;
    option.initFrom(&toolbar);
    option.rect = QRect(QPoint(0, 0), toolbar.actionGeometry(separator).size());
    if (!size.isEmpty()) {
        option.rect.setSize(size);
    }
    option.state.setFlag(QStyle::State_Horizontal,
        orientation == Qt::Horizontal);
    require(!option.rect.isEmpty(), "the toolbar separator has no geometry");
    QImage image(option.rect.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        toolbar.style()->drawPrimitive(QStyle::PE_IndicatorToolBarSeparator,
            &option, &painter, &toolbar);
    }
    QRect painted;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) != 0) {
                painted = painted.united(QRect(x, y, 1, 1));
            }
        }
    }
    // A horizontal toolbar needs a vertical rule, and vice versa. Checking
    // span as well as direction catches the negative-width two-pixel dash.
    if (orientation == Qt::Horizontal) {
        require(painted.height() >= std::max(1, image.height() / 2)
                && painted.width() == 1,
            "a horizontal toolbar has no full-height vertical separator");
    } else {
        require(painted.width() >= std::max(1, image.width() / 2)
                && painted.height() == 1,
            "a vertical toolbar has no full-width horizontal separator");
    }
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    using amrvis::qt::Skin;
    using amrvis::qt::ThemeController;

    // The palette the desktop gave us, captured before any controller exists.
    // Every case below ends back on System, so this is what the application
    // palette must read as between cases.
    const QPalette systemPalette = QApplication::palette();
    // The dark skins install a proxy style over Fusion, so System has to put
    // the style back as well as the palette.
    const QString systemStyle = QApplication::style()->objectName();

    // All four dark skins use the proxy painter. Exercise both toolbar axes.
    {
        ThemeController controller;
        for (const auto skin : {Skin::Dark, Skin::Blue, Skin::Green,
                 Skin::Maroon}) {
            controller.selectSkin(skin);
            checkToolbarSeparator(Qt::Horizontal);
            checkToolbarSeparator(Qt::Vertical);
            // Shorter than the usual insets, and with an aspect ratio that
            // contradicts the toolbar's orientation: the state is authoritative.
            checkToolbarSeparator(Qt::Horizontal, QSize(6, 3));
            checkToolbarSeparator(Qt::Vertical, QSize(3, 6));
            checkToolbarSeparator(Qt::Horizontal, QSize(1, 1));
            checkToolbarSeparator(Qt::Vertical, QSize(1, 1));
        }
        controller.selectSkin(Skin::System);
    }

    // Keep each message on screen throughout a round trip: writing it again
    // after the switch would conceal stale stylesheet colours.
    {
        ThemeController controller;
        controller.selectSkin(Skin::Light);
        amrvis::qt::ExpressionEditorDialog errorDialog({}, nullptr);
        amrvis::qt::ExpressionEditorDialog warningDialog({}, nullptr);
        errorDialog.showError(QStringLiteral("Invalid expression"), std::nullopt);
        errorDialog.show();
        warningDialog.show();
        auto* error = errorDialog.findChild<QLabel*>(
            QStringLiteral("expressionError"));
        auto* warning = warningDialog.findChild<QLabel*>(
            QStringLiteral("expressionWarning"));
        require(error != nullptr && warning != nullptr,
            "the editor's message labels are missing");
        for (const bool blocking : {false, true, false}) {
            warningDialog.showResolutionWarning(
                QStringLiteral("Resolution warning"), blocking);
            for (const auto skin : {Skin::Light, Skin::Dark, Skin::Light}) {
                controller.selectSkin(skin);
                QApplication::processEvents();
                require(error->isVisible() && warning->isVisible(),
                    "a palette change hid a standing message");
                require(error->text() == QLatin1String("Invalid expression")
                        && warning->text() == QLatin1String("Resolution warning"),
                    "a palette change changed a standing message");
                require(error->palette().color(QPalette::WindowText)
                        == amrvis::qt::errorTextColor(),
                    "a standing error did not follow the skin");
                require(warning->palette().color(QPalette::WindowText)
                        == (blocking ? amrvis::qt::errorTextColor()
                                     : amrvis::qt::warningTextColor()),
                    "a standing warning lost its skin colour or severity");
            }
        }
        controller.selectSkin(Skin::System);
    }

    // Set Contours is modeless too: leave its field-conflict warning visible
    // while changing skins, without touching the selected vector fields.
    {
        ThemeController controller;
        controller.selectSkin(Skin::Light);
        amrvis::qt::SetContoursDialog dialog({"u", "v"}, false);
        dialog.setMode(amrvis::qt::DisplayMode::VelocityVectors);
        dialog.setVectorFields(0, 0, 0);
        dialog.show();
        QLabel* warning = nullptr;
        for (auto* label : dialog.findChildren<QLabel*>()) {
            if (label->text() == QLatin1String("U and V fields must be different")) {
                warning = label;
            }
        }
        require(warning != nullptr, "the vector-field warning is missing");
        for (const auto skin : {Skin::Light, Skin::Dark, Skin::Light}) {
            controller.selectSkin(skin);
            QApplication::processEvents();
            require(warning->isVisible(), "a palette change hid the vector warning");
            require(warning->palette().color(QPalette::WindowText)
                    == amrvis::qt::errorTextColor(),
                "a standing vector-field warning did not follow the skin");
        }
        controller.selectSkin(Skin::System);
    }

    // The keys are the on-disk settings format: pin them as literals.
    {
        require(amrvis::qt::skinKey(Skin::System) == QLatin1String("system")
                && amrvis::qt::skinKey(Skin::Light) == QLatin1String("light")
                && amrvis::qt::skinKey(Skin::Dark) == QLatin1String("dark")
                && amrvis::qt::skinKey(Skin::Blue) == QLatin1String("blue")
                && amrvis::qt::skinKey(Skin::Green) == QLatin1String("green")
                && amrvis::qt::skinKey(Skin::Maroon) == QLatin1String("maroon"),
            "skin settings keys changed");
        require(amrvis::qt::skinFromKey(QStringLiteral("dark")) == Skin::Dark,
            "a stored key did not round-trip");
        require(amrvis::qt::skinFromKey(QStringLiteral("chartreuse"))
                == Skin::System,
            "an unrecognised stored skin did not fall back to System");
        require(amrvis::qt::skinFromKey(QString()) == Skin::System,
            "a missing stored skin did not fall back to System");
    }

    // The menu offers exactly the three skins and checks the applied one.
    {
        QWidget parent;
        ThemeController controller;
        auto* menu = controller.createMenu(&parent);
        require(menu != nullptr, "createMenu returned nothing");
        require(controller.menuLabels()
                == QStringList{QStringLiteral("&System"),
                    QStringLiteral("&Light"), QStringLiteral("&Dark"),
                    QStringLiteral("&Blue"), QStringLiteral("&Green"),
                    QStringLiteral("&Maroon")},
            "the Skin menu does not offer exactly the six skins");
        require(controller.skin() == Skin::System,
            "the default skin is not System");
        require(controller.checkedMenuLabel() == QLatin1String("&System"),
            "the menu does not check the applied skin");
    }

    // Dark and Light produce the palettes their names promise, and System puts
    // back the one captured at startup.
    {
        QWidget parent;
        ThemeController controller;
        controller.createMenu(&parent);

        int changes = 0;
        QObject::connect(&controller, &ThemeController::skinChanged,
            [&changes] { ++changes; });

        controller.selectSkin(Skin::Dark);
        require(changes == 1, "selecting Dark did not emit skinChanged once");
        require(controller.skin() == Skin::Dark, "Dark was not applied");
        require(controller.checkedMenuLabel() == QLatin1String("&Dark"),
            "the menu check did not follow the applied skin");
        {
            const auto palette = QApplication::palette();
            require(isDark(palette), "the Dark skin's window colour is light");
            require(palette.color(QPalette::WindowText).lightness() > 127,
                "the Dark skin's window text is not light");
            require(palette.color(QPalette::Base).lightness() < 128,
                "the Dark skin's text-entry background is not dark");
            // A disabled group identical to the active one is the classic
            // hand-rolled dark palette bug: every grayed-out control reads as
            // enabled.
            require(palette.color(QPalette::Disabled, QPalette::WindowText)
                    != palette.color(QPalette::Active, QPalette::WindowText),
                "the Dark skin cannot distinguish disabled text");
            require(palette.color(QPalette::Disabled, QPalette::Text)
                    != palette.color(QPalette::Active, QPalette::Text),
                "the Dark skin cannot distinguish disabled item text");
            // The frame shades have to stay ordered, or separators and sunken
            // frames vanish into the background.
            require(palette.color(QPalette::Light).lightness()
                    > palette.color(QPalette::Dark).lightness(),
                "the Dark skin's frame shades are not ordered");
        }

        // Re-selecting the applied skin is a no-op, not a second change.
        controller.selectSkin(Skin::Dark);
        require(changes == 1, "re-selecting the applied skin emitted a change");
        require(controller.checkedMenuLabel() == QLatin1String("&Dark"),
            "re-selecting the applied skin unchecked its menu action");

        controller.selectSkin(Skin::Light);
        require(changes == 2, "selecting Light did not emit skinChanged");
        require(!isDark(QApplication::palette()),
            "the Light skin's window colour is dark");
        require(QApplication::palette().color(QPalette::WindowText).lightness()
                < 128,
            "the Light skin's window text is not dark");

        // The three tinted skins are the neutral Dark set at a hue, so what
        // is worth asserting is that the construction holds for each: dark,
        // legible against both grounds, distinguishable when disabled, and --
        // the one that actually regressed -- structured like the neutral skin,
        // whose lightnesses are what keep Fusion's derived separator and
        // check-box outlines visible. Their exact colours are the palette's
        // business; only the accent Blue quotes from the icon is pinned.
        const auto neutral = [&controller] {
            controller.selectSkin(Skin::Dark);
            return QApplication::palette();
        }();
        int expected = changes;
        for (const auto skin : {Skin::Blue, Skin::Green, Skin::Maroon}) {
            controller.selectSkin(skin);
            ++expected;
            require(changes == expected, "a tinted skin emitted no change");
            const auto palette = QApplication::palette();
            require(isDark(palette), "a tinted skin's window colour is light");
            require(contrast(palette.color(QPalette::WindowText),
                        palette.color(QPalette::Window)) >= 4.5,
                "a tinted skin's text does not contrast with its chrome");
            require(contrast(palette.color(QPalette::Text),
                        palette.color(QPalette::Base)) >= 4.5,
                "a tinted skin's text does not contrast with its entry fields");
            require(contrast(palette.color(QPalette::HighlightedText),
                        palette.color(QPalette::Highlight)) >= 4.5,
                "a tinted skin's selected text does not contrast");
            require(palette.color(QPalette::Disabled, QPalette::Text)
                    != palette.color(QPalette::Active, QPalette::Text),
                "a tinted skin cannot distinguish disabled item text");
            require(palette.color(QPalette::Light).lightness()
                    > palette.color(QPalette::Dark).lightness(),
                "a tinted skin's frame shades are not ordered");
            // Base above Window is what the first Blue got backwards: it put
            // the menu ground, which Fusion derives from Base, far below the
            // chrome, and the separators and check-box frames went with it.
            require(palette.color(QPalette::Base).lightness()
                    < palette.color(QPalette::Window).lightness(),
                "a tinted skin's entry ground is not below its chrome");
            for (const auto role : {QPalette::Window, QPalette::Base,
                     QPalette::AlternateBase}) {
                require(std::abs(palette.color(role).lightness()
                            - neutral.color(role).lightness()) <= 6,
                    "a tinted skin left the neutral skin's lightness band");
            }
        }

        // The one colour in the tinted skins quoted rather than derived.
        controller.selectSkin(Skin::Blue);
        ++expected;
        require(QApplication::palette().color(QPalette::Highlight)
                == QColor(0x4a, 0xc3, 0xe2),
            "the Blue skin's highlight is not the application icon's lens");

        controller.selectSkin(Skin::System);
        require(changes == expected + 1,
            "returning to System did not emit skinChanged");
        require(QApplication::palette() == systemPalette,
            "System did not restore the palette captured at startup");
        require(QApplication::style()->objectName() == systemStyle,
            "System did not restore the style captured at startup");
    }

    // A skin picked in one window re-checks the menus of the others; the
    // applied skin is a property of the application, not of a controller.
    {
        QWidget firstParent;
        QWidget secondParent;
        ThemeController first;
        ThemeController second;
        first.createMenu(&firstParent);
        second.createMenu(&secondParent);

        first.selectSkin(Skin::Dark);
        require(second.skin() == Skin::Dark,
            "a second controller did not see the applied skin");
        require(second.checkedMenuLabel() == QLatin1String("&Dark"),
            "a second controller's menu did not follow the applied skin");

        second.selectSkin(Skin::System);
        require(first.checkedMenuLabel() == QLatin1String("&System"),
            "the first controller's menu did not follow the second's choice");
    }

    // Settings round-trip, and restore() applies without reporting a change.
    {
        QTemporaryDir directory;
        require(directory.isValid(), "could not create a scratch directory");
        const auto path = QDir(directory.path()).filePath("settings.ini");

        {
            QWidget parent;
            ThemeController controller;
            controller.createMenu(&parent);
            controller.selectSkin(Skin::Blue);
            QSettings settings(path, QSettings::IniFormat);
            controller.save(settings);
            settings.sync();
            controller.selectSkin(Skin::System);
        }
        {
            QSettings settings(path, QSettings::IniFormat);
            require(settings.value(QStringLiteral("theme/skin")).toString()
                    == QLatin1String("blue"),
                "the stored skin is not the documented key");
        }
        {
            QWidget parent;
            ThemeController controller;
            controller.createMenu(&parent);
            int changes = 0;
            QObject::connect(&controller, &ThemeController::skinChanged,
                [&changes] { ++changes; });
            const QSettings settings(path, QSettings::IniFormat);
            controller.restore(settings);
            require(controller.skin() == Skin::Blue,
                "restore did not apply the stored skin");
            require(changes == 0,
                "restore emitted skinChanged, which would rewrite settings");
            require(controller.checkedMenuLabel() == QLatin1String("&Blue"),
                "restore did not check the stored skin in the menu");
            controller.selectSkin(Skin::System);
        }
        // A settings file with no skin key, and one with a bad value, both
        // leave the application on System.
        {
            const auto emptyPath = QDir(directory.path()).filePath("empty.ini");
            QWidget parent;
            ThemeController controller;
            controller.createMenu(&parent);
            controller.selectSkin(Skin::Dark);
            const QSettings settings(emptyPath, QSettings::IniFormat);
            controller.restore(settings);
            require(controller.skin() == Skin::System,
                "an absent stored skin did not restore System");
        }
    }

    require(QApplication::palette() == systemPalette,
        "the tests left the application palette changed");

    std::cout << "theme controller tests passed\n";
    return 0;
}
