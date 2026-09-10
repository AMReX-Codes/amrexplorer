// Unit tests for the Qt-free-of-widgets NumberFormat helpers (they need only
// QString). isValidNumberFormat is a printf-format validator that must accept
// exactly one floating conversion and reject everything else; formatNumber
// applies a valid format and falls back to a general format otherwise. Prior
// coverage was only indirect, through ScientificDoubleSpinBox.
#include "NumberFormat.hpp"

#include <QString>

#include <clocale>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using namespace amrvis::qt;

    require(defaultNumberFormat() == QStringLiteral("%g"),
        "the default number format changed");

    // Accepted: exactly one floating conversion, with any supported
    // flag/width/precision/conversion, possibly embedded in literal text and
    // alongside literal %%.
    require(isValidNumberFormat(QStringLiteral("%g")), "%g was rejected");
    require(isValidNumberFormat(QStringLiteral("%.3e")), "%.3e was rejected");
    require(isValidNumberFormat(QStringLiteral("%+08.2f")), "%+08.2f was rejected");
    require(isValidNumberFormat(QStringLiteral("%E")), "%E was rejected");
    require(isValidNumberFormat(QStringLiteral("%G")), "%G was rejected");
    require(isValidNumberFormat(QStringLiteral("value=%.2e units")),
        "a specifier embedded in literal text was rejected");
    require(isValidNumberFormat(QStringLiteral("100%% of %g")),
        "a literal %% alongside one specifier was rejected");

    // Rejected: no conversion, the wrong conversion, more than one, a trailing
    // percent, and '*' width/precision (which would consume extra varargs).
    require(!isValidNumberFormat(QStringLiteral("plain text")),
        "a format with no conversion was accepted");
    require(!isValidNumberFormat(QStringLiteral("%%")),
        "a lone literal %% (no conversion) was accepted");
    require(!isValidNumberFormat(QStringLiteral("%d")),
        "an integer conversion was accepted");
    require(!isValidNumberFormat(QStringLiteral("%s")),
        "a string conversion was accepted");
    require(!isValidNumberFormat(QStringLiteral("%g %g")),
        "two conversions were accepted");
    require(!isValidNumberFormat(QStringLiteral("%")),
        "a trailing percent was accepted");
    require(!isValidNumberFormat(QStringLiteral("%*g")),
        "a '*' width was accepted");
    require(!isValidNumberFormat(QStringLiteral("%.*f")),
        "a '*' precision was accepted");

    // conversionSpecifier extracts the specifier, or falls back to the default.
    require(conversionSpecifier(QStringLiteral("value=%.2e units"))
            == QStringLiteral("%.2e"),
        "conversionSpecifier did not extract the embedded specifier");
    require(conversionSpecifier(QStringLiteral("no specifier here"))
            == defaultNumberFormat(),
        "conversionSpecifier did not fall back to the default");

    // formatNumber applies a valid format (fixed-point is portable) ...
    require(formatNumber(3.14159, QStringLiteral("%.2f")) == QStringLiteral("3.14"),
        "formatNumber did not honor a valid fixed-point format");
    // ... falls back to a general format for an invalid one ...
    require(formatNumber(3.14159, QStringLiteral("%d"))
            == QString::number(3.14159, 'g', 7),
        "formatNumber did not fall back for an invalid format");
    // ... and falls back again when the formatted result overflows its buffer
    // (128 bytes): a 130-digit precision is a valid but unrenderable format.
    require(formatNumber(1.0, QStringLiteral("%.130f"))
            == QString::number(1.0, 'g', 7),
        "formatNumber did not fall back on a buffer overflow");

    // snprintf honors LC_NUMERIC, and QApplication sets it from the
    // environment on Unix, so under a comma locale the formatted path would
    // render "3,14" while both fallback paths above -- which use
    // QString::number -- always render a point. Every readout has to agree.
    // Skipped, with a note, where the locale is not installed: the fix is
    // wanted on the platforms that have it, and a test that silently passes
    // for the wrong reason is worse than one that says why.
    // Literal text survives in the C locale too, so this half of the contract
    // is checked whether or not a comma locale is installed.
    require(formatNumber(3.14159, QStringLiteral("rho=%.2f, kg/m3"))
            == QStringLiteral("rho=3.14, kg/m3"),
        "formatNumber altered the literal text around the conversion");
    require(formatNumber(2.5, QStringLiteral("%.3f,%%"))
            == QStringLiteral("2.500,%"),
        "formatNumber mishandled a literal percent");

    // Several candidates, not just de_DE: the runners here and on CI have no
    // German locale but do have en_DK, so trying only de_DE meant this branch
    // printed its note and passed green everywhere it mattered. A silent skip
    // is how the far larger sibling bug -- strtod failing on "0.5", so no
    // plotfile opened at all under a comma locale -- stayed hidden.
    const char* commaLocales[] = {"de_DE.UTF-8", "de_DE", "en_DK.UTF-8",
        "en_DK.utf8", "fr_FR.UTF-8", "fr_FR.utf8"};
    bool commaLocaleSet = false;
    for (const auto* candidate : commaLocales) {
        if (std::setlocale(LC_NUMERIC, candidate) != nullptr) {
            commaLocaleSet = true;
            break;
        }
    }
    if (commaLocaleSet) {
        require(formatNumber(3.14159, QStringLiteral("%.2f"))
                == QStringLiteral("3.14"),
            "formatNumber emitted a locale decimal separator");
        require(formatNumber(1234.5, QStringLiteral("%.1f"))
                == QStringLiteral("1234.5"),
            "formatNumber emitted a locale decimal separator");
        // The normalization must reach the conversion's output and nothing
        // else. Under a comma locale the decimal separator *is* the comma the
        // user typed as a literal, so substituting across the whole formatted
        // string rewrote their text too: this returned "rho=3.14. kg/m3".
        require(formatNumber(3.14159, QStringLiteral("rho=%.2f, kg/m3"))
                == QStringLiteral("rho=3.14, kg/m3"),
            "the decimal-point normalization rewrote literal text");
        require(formatNumber(2.5, QStringLiteral("%.3f,%%"))
                == QStringLiteral("2.500,%"),
            "the decimal-point normalization rewrote a literal separator");
        std::setlocale(LC_NUMERIC, "C");
    } else {
        std::cerr << "note: no comma-decimal locale installed (tried de_DE, "
                     "en_DK, fr_FR); the LC_NUMERIC case did not run\n";
    }

    // --- adaptive precision -------------------------------------------------
    // Ranges with nothing to separate fall back to what a bare %g always gave.
    require(displayDigits(0.0, 1.0) == 6, "an ordinary range asked for digits");
    require(displayDigits(-1.0, 1.0) == 6, "a signed range asked for digits");
    require(displayDigits(0.1, 0.1) == 6, "a zero span asked for digits");
    require(displayDigits(0.0, 0.0) == 6, "a zero range asked for digits");
    require(displayDigits(1.0, 0.0) == 6, "an inverted range asked for digits");
    require(displayDigits(std::numeric_limits<double>::quiet_NaN(), 1.0) == 6,
        "a NaN bound asked for digits");
    require(displayDigits(1.0, std::numeric_limits<double>::infinity()) == 6,
        "an infinite bound asked for digits");
    require(displayDigits(-std::numeric_limits<double>::max(),
                std::numeric_limits<double>::max())
            == 6,
        "a span that overflows asked for digits");
    // Magnitude alone means nothing; only the span relative to it counts.
    require(displayDigits(1.0e-30, 1.0e30) == 6,
        "a decade-spanning range asked for digits");
    require(displayDigits(0.0, 1.0e-300) == 6,
        "a tiny-magnitude wide range asked for digits");
    require(displayDigits(-1.000000001, -1.0) == 12,
        "an all-negative narrow range got the wrong digit count");
    require(displayDigits(1.0, std::nextafter(1.0, 2.0)) == 17,
        "adjacent doubles did not clamp at max_digits10");
    require(displayDigits(1.0e300, 1.0e300 * (1.0 + 1.0e-13)) == 17,
        "a large-magnitude narrow range did not clamp");

    // Splicing keeps flags, width and literal text, and leaves alone both a
    // precision the user wrote and a conversion whose precision means
    // something else.
    const double narrowLow = 1.2566370621199999e-06;
    const double narrowHigh = 1.25663706213e-06;
    require(displayDigits(narrowLow, narrowHigh) == 15,
        "the reported range got the wrong digit count");
    const auto resolved
        = resolveNumberFormat(defaultNumberFormat(), narrowLow, narrowHigh);
    require(resolved == QStringLiteral("%.15g"),
        "a bare %g did not resolve to the range's digits");
    require(resolveNumberFormat(QStringLiteral("%G"), narrowLow, narrowHigh)
            == QStringLiteral("%.15G"),
        "%G did not resolve");
    require(resolveNumberFormat(QStringLiteral("%.13g"), narrowLow, narrowHigh)
            == QStringLiteral("%.13g"),
        "an explicit precision was overridden");
    require(resolveNumberFormat(QStringLiteral("%+12g"), narrowLow, narrowHigh)
            == QStringLiteral("%+12.15g"),
        "resolving dropped flags or width");
    require(resolveNumberFormat(
                QStringLiteral("rho=%g kg/m3"), narrowLow, narrowHigh)
            == QStringLiteral("rho=%.15g kg/m3"),
        "resolving dropped literal text");
    require(resolveNumberFormat(QStringLiteral("%e"), narrowLow, narrowHigh)
            == QStringLiteral("%e"),
        "%e was given significant-digit precision");
    require(resolveNumberFormat(QStringLiteral("%.2f"), narrowLow, narrowHigh)
            == QStringLiteral("%.2f"),
        "%f was given significant-digit precision");
    require(resolveNumberFormat(QStringLiteral("%d"), narrowLow, narrowHigh)
            == QStringLiteral("%d"),
        "an invalid format was rewritten");
    // Idempotence is what freezes an animation's digits and what stops a
    // pinned format being re-derived downstream. A different second range so
    // this cannot pass by both sides computing the same thing.
    require(resolveNumberFormat(resolved, 0.0, 1.0) == resolved,
        "resolving an already-resolved format changed it");

    // The headline: at the old default these two collide.
    require(formatNumber(narrowLow, defaultNumberFormat())
            == formatNumber(narrowHigh, defaultNumberFormat()),
        "the test pair no longer collides at the default format");
    require(formatNumber(narrowLow, defaultNumberFormat())
            == QStringLiteral("1.25664e-06"),
        "the default format no longer renders the reported string");
    require(formatNumber(narrowLow, resolved)
            != formatNumber(narrowHigh, resolved),
        "two values differing at the 12th digit still render alike");
    require(formatNumber(narrowLow, resolved)
            == QStringLiteral("1.25663706212e-06"),
        "the low value did not render at full precision");
    require(formatNumber(narrowHigh, resolved)
            == QStringLiteral("1.25663706213e-06"),
        "the high value did not render at full precision");

    // Ordinary data must not grow noise digits.
    const auto ordinary = resolveNumberFormat(defaultNumberFormat(), 0.0, 1.0);
    require(formatNumber(0.1, ordinary) == QStringLiteral("0.1"),
        "an ordinary value grew noise digits");
    require(formatNumber(0.3, ordinary) == QStringLiteral("0.3"),
        "an ordinary value grew noise digits");

    // --- fitNumber ----------------------------------------------------------
    // A plain character count stands in for a font.
    const auto charWidth = [](const QString& text) {
        return static_cast<int>(text.size());
    };
    require(fitNumber(narrowLow, resolved, 15, 6, charWidth, 80)
            == QStringLiteral("1.25663706212e-06"),
        "a label that fits was narrowed anyway");
    // 17 characters at 15 digits; 13 fits only by stepping down. Asserting the
    // intermediate count is what catches a loop that restarts at 6.
    require(fitNumber(narrowLow, resolved, 15, 6, charWidth, 13)
            == QStringLiteral("1.2566371e-06"),
        "fitting did not step down one digit at a time");
    require(fitNumber(narrowLow, resolved, 15, 12, charWidth, 1)
            == QStringLiteral("1.25663706212e-06"),
        "fitting went below its floor");
    require(charWidth(fitNumber(narrowLow, resolved, 15, 6, charWidth, 1)) > 1,
        "fitting past the floor did not return the narrowest rendering");

    for (const auto& format : {QString("%.99999999999g"), QString("%.2000g"),
             QString("%.99999999999f"), QString("%99999999999g")}) {
        require(formatDigits(format) <= maximumDisplayDigits,
            "unbounded precision escaped the display budget");
        require(!formatNumber(1.2345, format).isEmpty(),
            "oversized formatting did not return a bounded fallback");
    }
    require(fitNumber(12345678.9, "%.2f", 6, 1, charWidth, 10) == "1.2346e+07",
        "fixed-point export did not fall back to compact notation");
    require(charWidth(fitNumber(1.23456e200, "%.6e", 6, 1, charWidth, 10)) <= 10,
        "exponential export did not fit its label budget");

    return 0;
}
