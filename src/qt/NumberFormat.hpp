#pragma once

#include <QString>

namespace amrvis::qt {

// The default readout format for the color bar / probe (View -> Number Format...
// to change it). %g keeps significant digits and switches to exponent form for
// very large/small magnitudes; its widest output is 13 chars (e.g. -1.23456e-308).
QString defaultNumberFormat();

// True when format is a printf-style string carrying exactly one
// floating-point conversion specifier: '%', optional flags out of -+0 #,
// optional decimal width, optional .precision, then one of eEfgG. Literal
// text and %% escapes may surround the specifier; anything else is rejected
// (no %d/%s/%n, no '*' width or precision, no trailing '%', no second
// specifier).
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

} // namespace amrvis::qt
