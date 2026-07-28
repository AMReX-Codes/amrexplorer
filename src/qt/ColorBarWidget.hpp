#pragma once

#include <QPainter>
#include <QRect>
#include <QString>
#include <QWidget>

namespace amrvis {
class Palette;
}

namespace amrvis::qt {

class ColorBarWidget final : public QWidget {
public:
    explicit ColorBarWidget(QWidget* parent = nullptr);

    // Fixed panel width (including labels); kept constant so exports of the
    // same view always have the same size even when tick labels vary.
    static constexpr int panelWidth = 150;

    void setPalette(const amrvis::Palette* palette);
    void setFieldRange(QString fieldName, double minimum, double maximum);
    void setNumberFormat(QString format);
    [[nodiscard]] const QString& numberFormat() const noexcept
    {
        return m_numberFormat;
    }
    void setLogarithmic(bool logarithmic);
    void clearRange();

    // Paints the color bar into an arbitrary rect (e.g. for image export),
    // using this widget's current palette/range/format state.
    void paintBar(QPainter* painter, const QRect& target) const;

    // Width that just fits the bar plus the widest current tick label, so the
    // export panel is as narrow as the number format/range require. Stable for
    // the same format and range.
    [[nodiscard]] int preferredWidth() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    const amrvis::Palette* m_palette = nullptr;
    QString m_fieldName;
    QString m_numberFormat;
    double m_minimum = 0.0;
    double m_maximum = 1.0;
    bool m_logarithmic = false;
    bool m_hasRange = false;
};

} // namespace amrvis::qt
