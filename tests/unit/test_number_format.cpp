// Unit tests for the Qt-free-of-widgets NumberFormat helpers (they need only
// QString). isValidNumberFormat is a printf-format validator that must accept
// exactly one floating conversion and reject everything else; formatNumber
// applies a valid format and falls back to a general format otherwise. Prior
// coverage was only indirect, through ScientificDoubleSpinBox.
#include "NumberFormat.hpp"

#include <QString>

#include <clocale>
#include <cstdlib>
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

    return 0;
}
