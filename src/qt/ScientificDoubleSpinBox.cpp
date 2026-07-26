#include "ScientificDoubleSpinBox.hpp"
#include "NumberFormat.hpp"

#include <QDoubleValidator>
#include <QLineEdit>
#include <QLocale>
#include <QSignalBlocker>

#include <algorithm>
#include <limits>

namespace amrvis::qt {
namespace {

QLocale cNumberLocale()
{
    auto result = QLocale::c();
    result.setNumberOptions(QLocale::RejectGroupSeparator);
    return result;
}

QValidator::State validateNumber(const QString& text, int position,
    double minimum, double maximum, const QLocale& locale)
{
    QDoubleValidator validator(minimum, maximum,
        std::numeric_limits<double>::max_exponent10
            + std::numeric_limits<double>::digits10);
    validator.setLocale(locale);
    validator.setNotation(QDoubleValidator::ScientificNotation);
    auto candidate = text;
    return validator.validate(candidate, position);
}

} // namespace

ScientificDoubleSpinBox::ScientificDoubleSpinBox(QWidget* parent)
    : QDoubleSpinBox(parent)
    , m_numberFormat(defaultNumberFormat())
{
    // This is the largest decimal precision QDoubleSpinBox supports. It keeps
    // the stored value precise while textFromValue controls the visible digits.
    setDecimals(std::numeric_limits<double>::max_exponent10
        + std::numeric_limits<double>::digits10);
    setKeyboardTracking(false);
}

void ScientificDoubleSpinBox::setNumberFormat(const QString& format)
{
    if (!isValidNumberFormat(format)) {
        return;
    }
    const auto numberFormat = conversionSpecifier(format);
    if (m_numberFormat == numberFormat) {
        return;
    }
    m_numberFormat = numberFormat;
    const QSignalBlocker blocker(this);
    lineEdit()->setText(prefix() + textFromValue(value()) + suffix());
    lineEdit()->setModified(false);
}

QString ScientificDoubleSpinBox::textFromValue(double value) const
{
    return formatNumber(value, m_numberFormat);
}

double ScientificDoubleSpinBox::valueFromText(const QString& text) const
{
    const auto number = numberText(text);
    if (!lineEdit()->isModified()
        && number == formatNumber(value(), m_numberFormat).trimmed()) {
        return value();
    }
    bool ok = false;
    const auto cValue = cNumberLocale().toDouble(number, &ok);
    if (ok) {
        return cValue;
    }
    const auto localizedValue = locale().toDouble(number, &ok);
    return ok ? localizedValue : QDoubleSpinBox::valueFromText(text);
}

QValidator::State ScientificDoubleSpinBox::validate(
    QString& input, int& position) const
{
    const auto number = numberText(input);
    const auto prefixLength = input.startsWith(prefix())
        ? static_cast<int>(prefix().size()) : 0;
    const auto numberPosition = std::clamp(position - prefixLength,
        0, static_cast<int>(number.size()));

    const auto cState = validateNumber(
        number, numberPosition, minimum(), maximum(), cNumberLocale());
    if (cState != QValidator::Invalid) {
        return cState;
    }
    return validateNumber(
        number, numberPosition, minimum(), maximum(), locale());
}

QString ScientificDoubleSpinBox::numberText(const QString& text) const
{
    auto result = text;
    if (!prefix().isEmpty() && result.startsWith(prefix())) {
        result.remove(0, prefix().size());
    }
    if (!suffix().isEmpty() && result.endsWith(suffix())) {
        result.chop(suffix().size());
    }
    return result.trimmed();
}

} // namespace amrvis::qt
