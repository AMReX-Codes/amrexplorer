#pragma once

#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace amrvis::qt {

struct ScaleBarSpec {
    double lengthCm = 0.0;
    double lengthPixels = 0.0;
    std::string label;
};

// Choose a 1/2/5 x 10^n length no longer than the available screen width.
// The unit follows the whole visible physical scale, rather than the shorter
// bar itself: a parsec-wide view should say "0.2 pc", not "40000 AU".
[[nodiscard]] inline std::optional<ScaleBarSpec>
chooseScaleBar(double visibleWidthCm, double maximumLengthCm, double maximumPixels) {
    if (!(visibleWidthCm > 0.0) || !std::isfinite(visibleWidthCm) || !(maximumLengthCm > 0.0) ||
        !std::isfinite(maximumLengthCm) || !(maximumPixels > 0.0) ||
        !std::isfinite(maximumPixels)) {
        return std::nullopt;
    }

    constexpr double astronomicalUnitCm = 1.495978707e13;
    constexpr double parsecCm = 3.0856775814913673e18;
    constexpr std::array units{
        std::pair{1.0, "cm"},
        std::pair{astronomicalUnitCm, "AU"},
        std::pair{parsecCm, "pc"},
        std::pair{1000.0 * parsecCm, "kpc"},
    };

    const auto* unit = &units.front();
    for (const auto& candidate : units) {
        if (visibleWidthCm >= candidate.first) {
            unit = &candidate;
        }
    }

    const double maximumInUnit = maximumLengthCm / unit->first;
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

    const double lengthCm = nice * unit->first;
    const double lengthPixels = maximumPixels * lengthCm / maximumLengthCm;
    if (!(lengthPixels > 0.0) || !std::isfinite(lengthPixels)) {
        return std::nullopt;
    }

    std::ostringstream label;
    label.imbue(std::locale::classic());
    label << std::setprecision(3) << std::defaultfloat << nice << ' ' << unit->second;
    return ScaleBarSpec{lengthCm, lengthPixels, label.str()};
}

} // namespace amrvis::qt
