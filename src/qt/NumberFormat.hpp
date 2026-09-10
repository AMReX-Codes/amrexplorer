#pragma once

#include <QString>

#include <algorithm>

namespace amrvis::qt {

// The default readout format for the color bar / probe (View -> Number Format...
// to change it). %g keeps significant digits and switches to exponent form for
// very large/small magnitudes. A bare %g shows 6 digits, but resolveNumberFormat
// raises that to suit the range, so the widest output is 24 chars at 17 digits
// (e.g. -1.2345678901234567e-308).
QString defaultNumberFormat();

// What a bare %g has always shown, the most a double can round-trip, and the
// slack displayDigits adds past the digit that separates the bounds.
inline constexpr int minimumDisplayDigits = 6;
inline constexpr int maximumDisplayDigits = 17;
inline constexpr int displayGuardDigits = 3;

// Significant digits at which values a hundredth of [minimum, maximum] apart
// still render as different strings.
//
// Rendering v with d digits resolves to at most scale/10^(d-1), where scale is
// the larger bound's magnitude; asking that to be <= span/100 gives
// d >= log10(scale/span) + 3. Eight color-bar ticks are 14% of the range
// apart, so adjacent ticks always differ, and the probe resolves finer than
// the 253-slot palette can shade. A range with nothing to separate -- equal
// bounds, a zero magnitude, a non-finite end -- gets the 6-digit minimum.
[[nodiscard]] int displayDigits(double minimum, double maximum);

// `format` with `digits` spliced in as its precision, keeping flags, width and
// surrounding literal text. Only %g and %G are touched, and only when the
// author left the precision out: for %f the field means decimals rather than
// significant digits, and an explicit precision is a number the user meant.
// Anything else comes back unchanged, which makes this idempotent -- resolving
// an already-resolved format is a no-op, so a pinned format survives every
// layer that would otherwise re-derive it.
[[nodiscard]] QString withPrecision(const QString& format, int digits);

// As withPrecision, but replaces a precision that is already there. This is
// for fitting a label to a width, where the digit count is the thing being
// searched rather than the thing being honored.
[[nodiscard]] QString withForcedPrecision(const QString& format, int digits);

// The explicit precision, capped at maximumDisplayDigits, or the default 6.
// This is decimal places for %f/%e, not a bound on their output width.
[[nodiscard]] int formatDigits(const QString& format);

// `format` resolved against the range it will render, ready to store wherever
// the authored format was stored.
[[nodiscard]] QString resolveNumberFormat(
    const QString& format, double minimum, double maximum);

// True when format is a printf-style string carrying exactly one
// floating-point conversion specifier: '%', optional flags out of -+0 #,
// optional decimal width, optional .precision, then one of eEfgG. Literal
// text and %% escapes may surround the specifier; anything else is rejected
// (no %d/%s/%n, no '*' width or precision, no trailing '%', no second
// specifier). A format the user pins here keeps its precision through
// resolveNumberFormat.
bool isValidNumberFormat(const QString& format);

// The '%...X' conversion-specifier substring of a format that passes
// isValidNumberFormat (flags, width, precision, and one eEfgG conversion).
// Unlike the scanner this consolidates (ScientificDoubleSpinBox's), it also
// checks the conversion character, so an unvalidated input cannot yield a
// non-float specifier: anything invalid returns defaultNumberFormat().
QString conversionSpecifier(const QString& format);

// snprintf()s value through the validated format into a 128-byte buffer
// (C locale; Qt never installs another one). An invalid format or a result
// that does not fit falls back to 'g' with 7 significant digits.
QString formatNumber(double value, const QString& format);

// `value` rendered as narrow as it must be to fit `width`, stepping the
// precision down one digit at a time from `digits` and never past
// `floorDigits`, with compact general notation as a fallback for wide formats.
// Returns the narrowest rendering even when that still
// overflows, so a caller that must not clip can test the width itself.
//
// Templated on the measurement so this stays Qt Core only and a test can
// drive it with a plain character count instead of a font.
template <typename Measure>
[[nodiscard]] QString fitNumber(double value, const QString& format, int digits,
    int floorDigits, Measure&& widthOf, int width)
{
    digits = std::clamp(digits, 1, maximumDisplayDigits);
    floorDigits = std::clamp(floorDigits, 1, digits);
    auto text = formatNumber(value, format);
    if (widthOf(text) <= width) {
        return text;
    }
    for (int precision = digits; precision >= floorDigits; --precision) {
        text = formatNumber(value, withForcedPrecision(format, precision));
        if (widthOf(text) <= width) {
            return text;
        }
    }
    // Fixed/exponential notation and literal text may remain too wide.
    // Retain the requested significant digits when compact notation fits,
    // then reduce them only as far as the caller allows.
    for (int precision = digits; precision >= floorDigits; --precision) {
        text = QString::number(value, 'g', precision);
        if (widthOf(text) <= width) {
            return text;
        }
    }
    return text;
}

} // namespace amrvis::qt
