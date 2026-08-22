#pragma once

// The two non-obvious rules in saving a widget's picture to a file, apart from
// the window that owns the menu action. Both had shipped bugs that only a human
// reading the diff caught, because neither was reachable from a test while it
// lived inside a private slot; as free functions they are unit-tested against a
// synthetic widget in tests/unit/test_widget_image_export.cpp.

#include <QImage>
#include <QPoint>
#include <QRegion>
#include <QString>
#include <QWidget>

namespace amrvis::qt {

// The name to save PNG bytes under, given what the save dialog returned.
//
// QImage::save(path, "PNG") forces the format whatever the name says, so a name
// that does not already say png gets it appended -- a typed "shot" would
// otherwise hold PNG bytes under a name nothing opens. A name that does say it
// comes back untouched, case included: chopping and re-appending turned
// "shot.PNG" into "shot.png", which on a case-sensitive filesystem writes past
// the file the dialog vetted and leaves the chosen one alone.
//
// One-way only: whether the name changed is the caller's comparison, because
// that is the same question the overwrite prompt asks (the dialog confirmed the
// name it returned, so only an appended suffix is worth asking about).
//
// Not QFileDialog::setDefaultSuffix: it only fills an *empty* suffix, so it
// leaves "shot.jpg" alone, and Qt applies it in selectedFiles() -- after a
// native dialog has already run its own overwrite prompt on the unsuffixed
// name. Both were tried and both were wrong.
[[nodiscard]] inline QString pngExportPath(const QString& chosen)
{
    if (chosen.isEmpty()
        || chosen.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        return chosen;
    }
    return chosen + QStringLiteral(".png");
}

// `widget` and whatever it paints, at `devicePixelRatio`, WITHOUT its child
// widgets.
//
// Not QWidget::grab(): that renders with DrawWindowBackground | DrawChildren,
// so a widget with controls parked over its content bakes them into the
// picture -- the volume view's XY/XZ/YZ preset buttons ended up in every
// exported frame that way. Omitting DrawChildren is what excludes them.
//
// The ratio is applied to the buffer rather than left at 1, or the file holds
// a fraction of the pixels the viewer is looking at on a hi-DPI display.
[[nodiscard]] inline QImage renderWidgetWithoutChildren(
    QWidget& widget, qreal devicePixelRatio)
{
    // A parameter rather than widget.devicePixelRatioF(): it is what makes the
    // scaling testable without a scaled platform, and a sequence export would
    // want one ratio frozen across a whole run. No guard on it -- every caller
    // passes a real ratio, and a guard here would be a branch no test can
    // reach honestly.
    QImage image(widget.size() * devicePixelRatio,
        QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    // Transparent first: render() paints the widget's own background over this,
    // but a widget that does not fill its whole rect would otherwise leave
    // whatever the allocation happened to contain.
    image.fill(Qt::transparent);
    widget.render(&image, QPoint(), QRegion(), QWidget::DrawWindowBackground);
    return image;
}

} // namespace amrvis::qt
