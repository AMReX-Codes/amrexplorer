#include "NumberFormat.hpp"

#include <clocale>
#include <cstdio>
#include <cstring>

namespace amrvis::qt {

QString defaultNumberFormat()
{
    return QStringLiteral("%g");
}

bool isValidNumberFormat(const QString& format)
{
    // Validate raw bytes: UTF-8 continuation bytes are >= 0x80, so they can
    // never alias an ASCII '%', flag, digit, or conversion character.
    const auto bytes = format.toUtf8();
    const auto size = bytes.size();
    auto specifiers = 0;
    for (qsizetype index = 0; index < size; ++index) {
        if (bytes[index] != '%') {
            continue;
        }
        ++index;
        if (index >= size) {
            return false;  // trailing '%'
        }
        if (bytes[index] == '%') {
            continue;  // literal %%
        }
        while (index < size && (bytes[index] == '-' || bytes[index] == '+'
                || bytes[index] == '0' || bytes[index] == ' '
                || bytes[index] == '#')) {
            ++index;
        }
        // '*' width/precision would consume extra varargs; reject outright.
        if (index < size && bytes[index] == '*') {
            return false;
        }
        while (index < size && bytes[index] >= '0' && bytes[index] <= '9') {
            ++index;
        }
        if (index < size && bytes[index] == '.') {
            ++index;
            if (index < size && bytes[index] == '*') {
                return false;
            }
            while (index < size && bytes[index] >= '0' && bytes[index] <= '9') {
                ++index;
            }
        }
        if (index >= size || (bytes[index] != 'e' && bytes[index] != 'E'
                && bytes[index] != 'f' && bytes[index] != 'g'
                && bytes[index] != 'G')) {
            return false;
        }
        ++specifiers;
    }
    return specifiers == 1;
}

namespace {

// Byte range [start, end) of the single floating conversion in `bytes`, or a
// start of -1 if there is none. Both the specifier accessor and the formatter
// need it, and the formatter needs the *position*, not just the text: it splices
// the rendered number back between the surrounding literals.
struct ConversionSpan {
    qsizetype start = -1;
    qsizetype end = -1;
};

ConversionSpan conversionSpan(const QByteArray& bytes)
{
    const auto size = bytes.size();
    for (qsizetype index = 0; index < size; ++index) {
        if (bytes[index] != '%') {
            continue;
        }
        const auto start = index++;
        if (index >= size || bytes[index] == '%') {
            continue;
        }
        while (index < size && (bytes[index] == '-' || bytes[index] == '+'
                || bytes[index] == '0' || bytes[index] == ' '
                || bytes[index] == '#')) {
            ++index;
        }
        while (index < size && bytes[index] >= '0' && bytes[index] <= '9') {
            ++index;
        }
        if (index < size && bytes[index] == '.') {
            ++index;
            while (index < size && bytes[index] >= '0' && bytes[index] <= '9') {
                ++index;
            }
        }
        if (index < size && (bytes[index] == 'e' || bytes[index] == 'E'
                || bytes[index] == 'f' || bytes[index] == 'g'
                || bytes[index] == 'G')) {
            return {start, index + 1};
        }
    }
    return {};
}

// Literal text around the conversion. snprintf is no longer run over it, so the
// "%%" it would have collapsed to "%" is collapsed here instead.
QString literalText(const char* data, qsizetype size)
{
    QByteArray literal;
    literal.reserve(size);
    for (qsizetype index = 0; index < size; ++index) {
        literal.append(data[index]);
        if (data[index] == '%' && index + 1 < size && data[index + 1] == '%') {
            ++index;
        }
    }
    return QString::fromUtf8(literal);
}

} // namespace

QString conversionSpecifier(const QString& format)
{
    const auto bytes = format.toUtf8();
    const auto span = conversionSpan(bytes);
    if (span.start >= 0) {
        return QString::fromLatin1(
            bytes.constData() + span.start, span.end - span.start);
    }
    return defaultNumberFormat();
}

QString formatNumber(double value, const QString& format)
{
    if (!isValidNumberFormat(format)) {
        return QString::number(value, 'g', 7);
    }
    const auto bytes = format.toUtf8();
    const auto span = conversionSpan(bytes);
    if (span.start < 0) {
        return QString::number(value, 'g', 7);
    }
    // Only the conversion is rendered through snprintf, and only its output is
    // normalized below. Formatting the whole format string and substituting
    // across the result corrupts the user's literal text: under a comma locale
    // the decimal point *is* a comma, so "rho=%.2f, kg/m3" -- which the
    // validator accepts -- came back as "rho=3.14. kg/m3", with the literal
    // separator rewritten too. The old comment reasoned that the decimal point
    // was the only locale-dependent character that could appear, which is true
    // of the conversion's output and says nothing about the literals around it.
    const QByteArray specifier(bytes.constData() + span.start,
        span.end - span.start);
    char buffer[128];
    // The validator guarantees exactly one floating conversion and no other
    // arguments, so a single double is the whole vararg list.
    const auto written = std::snprintf(buffer, sizeof(buffer),
        specifier.constData(), value);
    if (written < 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
        return QString::number(value, 'g', 7);
    }
    auto number = QString::fromUtf8(buffer, written);
    // Belt-and-braces since main() pins LC_NUMERIC to "C" after constructing
    // QApplication: with that pin in place snprintf already renders "1.5", and
    // this substitution finds nothing to do. It is kept because this is a
    // library function -- nothing stops it being called from a process that
    // has not pinned the locale, and the readouts have to agree with the
    // QString::number fallbacks above, which are always C-locale, whichever
    // locale is in force. Substituting the decimal point rather than reaching
    // for snprintf_l keeps it free of platform conditionals, and with only the
    // conversion's own output in hand it cannot reach anything the user typed.
    if (const auto* conventions = std::localeconv(); conventions != nullptr) {
        const auto* point = conventions->decimal_point;
        if (point != nullptr && *point != '\0'
            && std::strcmp(point, ".") != 0) {
            number.replace(QString::fromUtf8(point), QStringLiteral("."));
        }
    }
    return literalText(bytes.constData(), span.start) + number
        + literalText(bytes.constData() + span.end, bytes.size() - span.end);
}

} // namespace amrvis::qt
