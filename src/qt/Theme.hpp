#pragma once

#include <QColor>
#include <QGuiApplication>
#include <QPalette>

namespace amrvis::qt {

// Background shared by the image viewports and the color scale: a dark gray
// that reads as a distinct panel without the harshness of pure black. Adjust
// here to retune every viewport at once.
inline QColor viewportBackground()
{
    return {0x88, 0x88, 0x88};
}

// Foreground for text and rules drawn straight onto viewportBackground().
// Mid-gray on mid-gray is invisible, which is what the Isometric view's
// placeholder used to be; this is dark enough to read against 0x888888 while
// staying quieter than the data.
inline QColor viewportForeground()
{
    return {0x20, 0x20, 0x20};
}

// Text colours for the dialogs' inline error and warning labels. These are
// stylesheet colours rather than palette roles -- there is no palette role for
// "this input is wrong" -- so they have to pick their own contrast, which the
// window colour's lightness decides. Read when the label's text is set, so a
// dialog opened after a skin change gets the right pair.
namespace detail {

inline bool onLightBackground()
{
    return QGuiApplication::palette().color(QPalette::Window).lightness() > 127;
}

} // namespace detail

// Refusal: the input was rejected.
inline QColor errorTextColor()
{
    return detail::onLightBackground() ? QColor(0xc0, 0x00, 0x00)
                                       : QColor(0xff, 0x6b, 0x6b);
}

// Caution: the input is accepted but questionable. Deliberately not the error
// colour, so the two cannot be confused.
inline QColor warningTextColor()
{
    return detail::onLightBackground() ? QColor(0xb8, 0x86, 0x0b)
                                       : QColor(0xe0, 0xb0, 0x40);
}

} // namespace amrvis::qt
