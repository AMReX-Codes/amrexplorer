#pragma once

#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace amrvis::qt {

enum class LengthUnit {
    Centimetre,
    Metre,
    Kilometre,
    AstronomicalUnit,
    Parsec,
    Kiloparsec,
    Megaparsec,
};

struct LengthUnitSpec {
    LengthUnit unit;
    double centimetresPerUnit;
    std::string_view id;
};

inline constexpr double astronomicalUnitCm = 1.495978707e13;
inline constexpr double parsecCm = 3.0856775814913673e18;
inline constexpr std::array lengthUnits{
    LengthUnitSpec{LengthUnit::Centimetre, 1.0, "cm"},
    LengthUnitSpec{LengthUnit::Metre, 100.0, "m"},
    LengthUnitSpec{LengthUnit::Kilometre, 1.0e5, "km"},
    LengthUnitSpec{LengthUnit::AstronomicalUnit, astronomicalUnitCm, "AU"},
    LengthUnitSpec{LengthUnit::Parsec, parsecCm, "pc"},
    LengthUnitSpec{LengthUnit::Kiloparsec, 1.0e3 * parsecCm, "kpc"},
    LengthUnitSpec{LengthUnit::Megaparsec, 1.0e6 * parsecCm, "Mpc"},
};

[[nodiscard]] inline std::optional<LengthUnit> lengthUnitFromId(
    std::string_view id)
{
    for (const auto& candidate : lengthUnits) {
        if (candidate.id == id) {
            return candidate.unit;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline const LengthUnitSpec& lengthUnitSpec(LengthUnit unit)
{
    for (const auto& candidate : lengthUnits) {
        if (candidate.unit == unit) {
            return candidate;
        }
    }
    return lengthUnits.front();
}

struct ScaleBarSpec {
    double lengthCodeUnits = 0.0;
    double lengthPixels = 0.0;
    std::string label;
};

// Choose a 1/2/5 x 10^n length no longer than the available screen width.
// Without an explicitly selected plotfile unit, preserve native coordinate
// values and say so in scientific notation. With one, the displayed unit
// follows the whole visible physical scale rather than the shorter bar itself:
// a parsec-wide view should say "0.2 pc", not "40000 AU".
[[nodiscard]] inline std::optional<ScaleBarSpec>
chooseScaleBar(double visibleWidthCodeUnits, double maximumLengthCodeUnits,
    double maximumPixels, std::optional<LengthUnit> codeUnit = std::nullopt) {
    if (!(visibleWidthCodeUnits > 0.0)
        || !std::isfinite(visibleWidthCodeUnits)
        || !(maximumLengthCodeUnits > 0.0)
        || !std::isfinite(maximumLengthCodeUnits) || !(maximumPixels > 0.0) ||
        !std::isfinite(maximumPixels)) {
        return std::nullopt;
    }

    double maximumInUnit = maximumLengthCodeUnits;
    double displayedUnitsPerCodeUnit = 1.0;
    std::string_view displayedUnit;
    if (codeUnit) {
        const double centimetresPerCodeUnit
            = lengthUnitSpec(*codeUnit).centimetresPerUnit;
        const double visibleWidthCm
            = visibleWidthCodeUnits * centimetresPerCodeUnit;
        if (!(visibleWidthCm > 0.0) || !std::isfinite(visibleWidthCm)) {
            return std::nullopt;
        }
        const auto* unit = &lengthUnits.front();
        for (const auto& candidate : lengthUnits) {
            if (visibleWidthCm >= candidate.centimetresPerUnit) {
                unit = &candidate;
            }
        }
        displayedUnitsPerCodeUnit
            = centimetresPerCodeUnit / unit->centimetresPerUnit;
        maximumInUnit
            = maximumLengthCodeUnits * displayedUnitsPerCodeUnit;
        displayedUnit = unit->id;
    }

    if (!(maximumInUnit > 0.0) || !std::isfinite(maximumInUnit)) {
        return std::nullopt;
    }
    const double decade = std::pow(10.0, std::floor(std::log10(maximumInUnit)));
    if (!(decade > 0.0) || !std::isfinite(decade)) {
        return std::nullopt;
    }
    double nice = decade;
    for (const double multiplier : {1.0, 2.0, 5.0}) {
        const double candidate = multiplier * decade;
        if (candidate <= maximumInUnit ||
            std::abs(candidate - maximumInUnit) <=
                std::numeric_limits<double>::epsilon() * maximumInUnit) {
            nice = candidate;
        }
    }

    const double lengthCodeUnits = nice / displayedUnitsPerCodeUnit;
    const double lengthPixels
        = maximumPixels * lengthCodeUnits / maximumLengthCodeUnits;
    if (!(lengthPixels > 0.0) || !std::isfinite(lengthPixels)) {
        return std::nullopt;
    }

    std::ostringstream label;
    label.imbue(std::locale::classic());
    if (codeUnit) {
        label << std::setprecision(3) << std::defaultfloat << nice << ' '
              << displayedUnit;
    } else {
        label << std::scientific << std::setprecision(3) << nice
              << " code units";
    }
    return ScaleBarSpec{lengthCodeUnits, lengthPixels, label.str()};
}

} // namespace amrvis::qt
