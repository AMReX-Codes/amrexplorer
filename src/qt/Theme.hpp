#pragma once

#include <QColor>

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

} // namespace amrvis::qt
