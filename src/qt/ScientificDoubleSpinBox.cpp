#include "ScientificDoubleSpinBox.hpp"
#include "NumberFormat.hpp"

#include <QDoubleValidator>
#include <QLineEdit>
#include <QLocale>
#include <QRegularExpression>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
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

// A whole decimal number in either locale's notation. QLocale::toDouble
// rejects one only when it underflows.
bool isCompleteNumber(const QString& text, const QLocale& locale)
{
    const QRegularExpression pattern(QStringLiteral(
        "^[+-]?(\\d+(%1\\d*)?|%1\\d+)([eE][+-]?\\d+)?$")
            .arg(QRegularExpression::escape(locale.decimalPoint())));
    return pattern.match(text).hasMatch();
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
    connect(this, &QAbstractSpinBox::editingFinished, this, [this] {
        // Qt can leave identical retyped text marked as modified on focus
        // loss. Finish the edit and apply any format deferred while typing.
        const QSignalBlocker blocker(this);
        const auto text = prefix() + textFromValue(value()) + suffix();
        if (lineEdit()->text() != text) {
            lineEdit()->setText(text);
        }
        lineEdit()->setModified(false);
    });
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
    // Not while the user is typing. The format used to change only when
    // someone visited the Number Format dialog, but it now tracks the
    // displayed range, so it can change under a half-entered bound -- and
    // rewriting the editor there discards what they typed and moves the
    // cursor. The pending text keeps its own digits until it is committed;
    // editingFinished applies the new format and clears the pending edit.
    if (lineEdit()->isModified()) {
        return;
    }
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

void ScientificDoubleSpinBox::fixup(QString& input) const
{
    // A number past the range is clamped to it. Left alone, Qt puts the old
    // value back without a word. An overflow parses as an infinity, and a
    // complete number that still fails to parse has underflowed to zero.
    const auto number = numberText(input);
    const auto parse = [&number](const QLocale& locale, double& value) {
        bool ok = false;
        value = locale.toDouble(number, &ok);
        if (!ok && !std::isinf(value) && isCompleteNumber(number, locale)) {
            value = 0.0;
            ok = true;
        }
        return ok || std::isinf(value);
    };
    double value = 0.0;
    if ((!parse(cNumberLocale(), value) && !parse(locale(), value)) || std::isnan(value)) {
        QDoubleSpinBox::fixup(input);
        return;
    }
    // Full precision, not the display format: the text is parsed again, and a
    // short format can round the bound past itself (DBL_MAX to 1.8e+308). The
    // committed value is then shown in the display format.
    input = prefix()
        + cNumberLocale().toString(std::clamp(value, minimum(), maximum()), 'g',
            std::numeric_limits<double>::max_digits10)
        + suffix();
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
