#pragma once

#include <QDoubleSpinBox>
#include <QValidator>

namespace amrvis::qt {

// A double spin box that accepts decimal scientific notation. Keyboard
// tracking is disabled so partially entered values are not committed.
class ScientificDoubleSpinBox : public QDoubleSpinBox {
public:
    explicit ScientificDoubleSpinBox(QWidget* parent = nullptr);

    // Uses the format's floating conversion; surrounding literal text is
    // omitted so the displayed value remains directly editable.
    void setNumberFormat(const QString& format);
    [[nodiscard]] const QString& numberFormat() const noexcept
    {
        return m_numberFormat;
    }

protected:
    [[nodiscard]] QString textFromValue(double value) const override;
    [[nodiscard]] double valueFromText(const QString& text) const override;
    QValidator::State validate(QString& input, int& position) const override;

private:
    [[nodiscard]] QString numberText(const QString& text) const;

    QString m_numberFormat;
};

} // namespace amrvis::qt
