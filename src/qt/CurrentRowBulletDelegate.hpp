#pragma once

#include <QComboBox>
#include <QModelIndex>
#include <QPainter>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QWidget>

namespace amrvis::qt {

// Marks the active row in a selector dropdown with a bullet. The bullet lives
// in a reserved left column that every row's sizeHint accounts for, so names
// align and the indented text is never clipped. Installed only on the combo's
// popup view, so the closed combo still shows the clean palette name.
class CurrentRowBulletDelegate : public QStyledItemDelegate {
public:
    explicit CurrentRowBulletDelegate(QComboBox* combo, QObject* parent)
        : QStyledItemDelegate(parent), m_combo(combo) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        if (isSeparator(index)) {
            // The default combo delegate draws separators as a thin rule; this
            // custom delegate replaces it, so render the rule ourselves rather
            // than leaving a tall blank row. Paint the same item-view panel the
            // other rows use so the background matches, then draw the line.
            QStyleOptionViewItem sepOpt = option;
            initStyleOption(&sepOpt, index);
            auto* const sepStyle =
                sepOpt.widget != nullptr ? sepOpt.widget->style() : nullptr;
            if (sepStyle != nullptr) {
                sepStyle->drawPrimitive(
                    QStyle::PE_PanelItemViewItem, &sepOpt, painter, sepOpt.widget);
            }
            painter->save();
            painter->setPen(option.palette.color(QPalette::Mid));
            const int y = option.rect.center().y();
            painter->drawLine(option.rect.left() + kSeparatorMargin, y,
                option.rect.right() - kSeparatorMargin, y);
            painter->restore();
            return;
        }
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        auto* const style = opt.widget != nullptr ? opt.widget->style() : nullptr;

        // Full-width selection background, then the name indented past the
        // marker column so all rows line up at the same x.
        if (style != nullptr) {
            style->drawPrimitive(
                QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);
        }
        opt.rect.adjust(kMarkerColumn, 0, 0, 0);
        if (style != nullptr) {
            style->drawControl(
                QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        }

        if (m_combo != nullptr && index.row() == m_combo->currentIndex()) {
            const QPalette::ColorRole role =
                (opt.state & QStyle::State_Selected) != 0
                    ? QPalette::HighlightedText
                    : QPalette::WindowText;
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(opt.palette.brush(role));
            const QPointF center(option.rect.left() + kMarkerColumn / 2.0,
                option.rect.center().y());
            painter->drawEllipse(center, 2.5, 2.5);
            painter->restore();
        }
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        if (isSeparator(index)) {
            // A short row for the rule; the default sizeHint would give it a
            // full text-row height and read as a large gap.
            return QSize(0, kSeparatorHeight);
        }
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        // Reserve the marker column horizontally and add vertical padding so
        // the names have breathing room; keeps the closed combo unaffected.
        size.setWidth(size.width() + kMarkerColumn);
        size.setHeight(size.height() + kRowVerticalPadding);
        return size;
    }

private:
    static bool isSeparator(const QModelIndex& index)
    {
        return index.data(Qt::AccessibleDescriptionRole).toString()
            == QLatin1String("separator");
    }

    static constexpr int kMarkerColumn = 16;
    static constexpr int kRowVerticalPadding = 6;
    static constexpr int kSeparatorHeight = 9;
    static constexpr int kSeparatorMargin = 4;
    QPointer<QComboBox> m_combo;
};

} // namespace amrvis::qt
