#pragma once

// The non-obvious rules in saving a widget's picture to a file, apart from the
// window that owns the menu action: the name the bytes are written under, which
// of the files an export is about to write are already there, and painting the
// widget without the controls parked over it. Each had shipped a bug that only
// a human reading the diff caught, because none was reachable from a test while
// it lived inside a private slot; as free functions they are unit-tested in
// tests/unit/test_widget_image_export.cpp.

#include <QFileInfo>
#include <QImage>
#include <QPoint>
#include <QRegion>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace amrvis::qt {

// The suffix `chosen` already carries from `accepted`, spelled the way it was
// typed, or an empty string if it carries none.
//
// Case-insensitive on the way in and case-preserving on the way out: that is
// the whole point of returning the text rather than a bool. Recognising
// "shot.PNG" and then writing the lowercase spelling back is a different file
// on a case-sensitive filesystem -- one the save dialog never vetted, silently
// replaced, while the file the user picked is left alone.
//
// List `accepted` longest first, so a compound suffix (".tar.gz") is not
// matched by its own tail (".gz").
[[nodiscard]] inline QString carriedExportSuffix(
    const QString& chosen, const QStringList& accepted)
{
    for (const auto& suffix : accepted) {
        if (chosen.endsWith(suffix, Qt::CaseInsensitive)) {
            return chosen.right(suffix.size());
        }
    }
    return {};
}

// The name to write `canonical`-format bytes under, given what the save dialog
// returned and the suffixes that already say that format.
//
// QImage::save(path, "PNG") -- and the FITS writer -- force the format whatever
// the name says, so a name that says none of `accepted` gains `canonical`: a
// typed "shot" would otherwise hold PNG bytes under a name nothing opens. A
// name that already says the format comes back untouched, case included.
//
// One-way only: whether the name changed is the caller's comparison, because
// that is the same question the overwrite prompt asks (the dialog confirmed the
// name it returned, so only an appended suffix is worth asking about).
//
// Not QFileDialog::setDefaultSuffix: it only fills an *empty* suffix, so it
// leaves "shot.jpg" alone, and Qt applies it in selectedFiles() -- after a
// native dialog has already run its own overwrite prompt on the unsuffixed
// name. Both were tried and both were wrong.
[[nodiscard]] inline QString exportPathWithSuffix(const QString& chosen,
    const QStringList& accepted, const QString& canonical)
{
    if (chosen.isEmpty() || !carriedExportSuffix(chosen, accepted).isEmpty()) {
        return chosen;
    }
    return chosen + canonical;
}

// The single-suffix case: PNG.
[[nodiscard]] inline QString pngExportPath(const QString& chosen)
{
    return exportPathWithSuffix(
        chosen, {QStringLiteral(".png")}, QStringLiteral(".png"));
}

// Which of the files an export is about to write are already on disk, so the
// caller can name them before replacing them.
//
// `vetted` -- the name the save dialog returned -- is excluded: that one it
// already confirmed, and asking twice about the same file is what teaches
// someone to click through the prompt that matters. Everything else needs
// asking about, because the dialog never saw it: a name that gained a suffix,
// and the per-panel names a 3-D export derives from the one that was typed.
//
// The comparison is on the text, so on a case-insensitive filesystem a typed
// "shot.PNG" beside an existing "shot.png" is asked about twice. That is the
// harmless direction to be wrong in, and canonicalising here would mean a stat
// per candidate and a different answer per platform.
[[nodiscard]] inline QStringList existingExportTargets(
    const QString& vetted, const QStringList& targets)
{
    QStringList existing;
    for (const auto& target : targets) {
        if (target != vetted && QFileInfo::exists(target)) {
            existing.append(target);
        }
    }
    return existing;
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
    if (image.isNull()) {
        // Refused allocation. Carrying on would build a QPainter on a null
        // paint device, which Qt reports with two warnings of its own before
        // handing back the same empty image.
        return image;
    }
    image.setDevicePixelRatio(devicePixelRatio);
    // Transparent first: render() paints the widget's own background over this,
    // but a widget that does not fill its whole rect would otherwise leave
    // whatever the allocation happened to contain.
    image.fill(Qt::transparent);
    widget.render(&image, QPoint(), QRegion(), QWidget::DrawWindowBackground);
    return image;
}

} // namespace amrvis::qt
