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
    struct NumberPresentation {
        QString valueFormat;
        QString tickFormat;
        bool offsetLine = false;
    };

    explicit ColorBarWidget(QWidget* parent = nullptr);

    // Fixed panel width (including labels); kept constant so exports of the
    // same view always have the same size even when tick labels vary.
    // Minimum on-screen width; the widget grows past it when the tick
    // labels need more (see applyPreferredWidth).
    static constexpr int panelWidth = 150;

    void setPalette(const amrvis::Palette* palette);
    void setFieldRange(QString fieldName, double minimum, double maximum);
    void setNumberFormat(QString format);
    void setLogarithmic(bool logarithmic);
    void clearRange();

    // Label value at a fraction of the bar (0 = maximum, 1 = minimum).
    [[nodiscard]] static double tickValue(
        double minimum, double maximum, bool logarithmic, double fraction);

    // Paints the color bar into an arbitrary rect (e.g. for image export),
    // using this widget's current palette/range/format state.
    void paintBar(QPainter* painter, const QRect& target, bool transparentBackground = false,
                  bool boundedLabels = false,
                  const NumberPresentation* presentation = nullptr) const;

    // Capture the first frame's notation and offset-line placement. Its value
    // still follows the range, avoiding cancellation against a stale offset.
    [[nodiscard]] NumberPresentation exportPresentation(
        const QFontMetrics& metrics, const QRect& target) const;

    // Width that just fits the bar plus the widest current tick label, so the
    // export panel is as narrow as the number format/range require. Stable for
    // the same format and range.
    [[nodiscard]] int preferredWidth() const;
    [[nodiscard]] static int exportWidth(const QFontMetrics& metrics, int labelWidth);
    [[nodiscard]] int exportLabelWidth(const QFontMetrics& metrics, int maximumWidth,
                                       int height) const;

    // What the tick labels are actually drawn against: the format resolved
    // for this range, the common leading part factored out of the ticks (0
    // when the range is ordinary enough to print in full -- see tickOffset),
    // and the format the residuals use.
    [[nodiscard]] QString effectiveFormat() const;
    [[nodiscard]] double labelOffset() const;
    [[nodiscard]] QString tickFormat() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct LabelLayout;
    [[nodiscard]] double offsetForFormat(const QString& format) const;
    [[nodiscard]] LabelLayout labelLayout(
        const QFontMetrics& metrics, int height, bool bounded, int width,
        const NumberPresentation* presentation = nullptr) const;
    // Resize to whichever is larger, panelWidth or what the current labels
    // need, after anything that can change their width.
    void applyPreferredWidth();

    const amrvis::Palette* m_palette = nullptr;
    QString m_fieldName;
    // The authored format. It is resolved against the range this widget holds
    // (see effectiveFormat) rather than at the three setFieldRange call
    // sites, which is what keeps the two from drifting apart.
    QString m_numberFormat;
    double m_minimum = 0.0;
    double m_maximum = 1.0;
    bool m_logarithmic = false;
    bool m_hasRange = false;
};

} // namespace amrvis::qt
