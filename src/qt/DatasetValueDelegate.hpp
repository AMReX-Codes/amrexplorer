#pragma once

// The Dataset window's item delegate. It exists for one reason: a selected
// cell has to keep the colors the model gives it.
//
// The default QStyledItemDelegate paints a selected item with the theme's
// Highlight brush and its text in HighlightedText, which overrides both the
// per-value color the table draws its numbers in and the no-data shade behind
// the samples no grid covers. Selecting is the window's ordinary interaction
// -- a click marks a sample in the image, a drag marks a region -- and a
// selection is meant to stay up while the user looks at the image, so under
// the stock delegate the block you are looking at is exactly the block whose
// colors have gone.

#include <QColor>
#include <QModelIndex>
#include <QPalette>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>

namespace amrvis::qt {

// The fill for a selected cell: its own background mixed a third of the way
// towards the theme's selection color. Enough of a shift to read as selected,
// while an uncovered cell stays recognisably dark and every value keeps its
// own color. A background rather than a wash over the top, so the text is
// drawn on it at full strength; the one thing that still tints a value is a
// style's focus rect over the *current* cell, which Fusion draws as a
// translucent wash -- but it does that to an unselected current cell too, so
// it is the style's doing and not this delegate's.
[[nodiscard]] inline QColor datasetSelectionBackground(
    const QColor& ground, const QColor& highlight)
{
    constexpr float weight = 0.35F;
    const auto mix = [](float from, float to) {
        return from * (1.0F - weight) + to * weight;
    };
    return QColor::fromRgbF(mix(ground.redF(), highlight.redF()),
        mix(ground.greenF(), highlight.greenF()),
        mix(ground.blueF(), highlight.blueF()));
}

class DatasetValueDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(
        QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        // The base fills in the model's roles: ForegroundRole becomes
        // QPalette::Text and BackgroundRole becomes backgroundBrush.
        QStyledItemDelegate::initStyleOption(option, index);
        if (!(option->state & QStyle::State_Selected)) {
            return;
        }
        const auto ground = option->backgroundBrush.style() != Qt::NoBrush
            ? option->backgroundBrush.color()
            : option->palette.color(QPalette::Base);
        // Painted as an unselected cell carrying a different background: with
        // the flag cleared the style fills with backgroundBrush and draws the
        // text in QPalette::Text, so both of the model's colors survive. The
        // alternative -- leaving the flag and pointing HighlightedText at the
        // value's color -- keeps the theme's saturated fill, on which a value
        // near the palette's own blue end would be unreadable.
        option->state &= ~QStyle::State_Selected;
        option->backgroundBrush = datasetSelectionBackground(
            ground, option->palette.color(QPalette::Highlight));
    }
};

} // namespace amrvis::qt
