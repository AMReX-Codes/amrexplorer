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

QString conversionSpecifier(const QString& format)
{
    const auto bytes = format.toUtf8();
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
            return QString::fromLatin1(
                bytes.constData() + start, index - start + 1);
        }
    }
    return defaultNumberFormat();
}

QString formatNumber(double value, const QString& format)
{
    if (!isValidNumberFormat(format)) {
        return QString::number(value, 'g', 7);
    }
    const auto bytes = format.toUtf8();
    char buffer[128];
    // The validator guarantees exactly one floating conversion and no other
    // arguments, so a single double is the whole vararg list.
    const auto written = std::snprintf(buffer, sizeof(buffer),
        bytes.constData(), value);
    if (written < 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
        return QString::number(value, 'g', 7);
    }
    auto text = QString::fromUtf8(buffer, written);
    // snprintf honors LC_NUMERIC, and QApplication calls setlocale(LC_ALL, "")
    // on Unix, so under a comma locale this path renders "1,5" while both
    // fallbacks above use QString::number, which is always C-locale. The
    // readouts have to agree with each other whichever locale the user runs
    // in, and the C locale is what the rest of the application writes and
    // parses, so normalize to it. Substituting the decimal point rather than
    // reaching for snprintf_l keeps this free of platform conditionals; the
    // validator admits exactly one conversion and no grouping flag, so the
    // decimal point is the only locale-dependent character that can appear.
    if (const auto* conventions = std::localeconv(); conventions != nullptr) {
        const auto* point = conventions->decimal_point;
        if (point != nullptr && *point != '\0'
            && std::strcmp(point, ".") != 0) {
            text.replace(QString::fromUtf8(point), QStringLiteral("."));
        }
    }
    return text;
}

} // namespace amrvis::qt
