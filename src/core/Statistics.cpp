#include <amrexplorer/core/Statistics.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace amrvis {

std::optional<ValueRange> metadataValueRange(
    const DatasetMetadata& metadata, FieldId field, std::optional<int> level)
{
    if (field.value >= metadata.fields.size()) {
        return std::nullopt;
    }
    if (level && (*level < 0
        || static_cast<std::size_t>(*level) >= metadata.levels.size())) {
        return std::nullopt;
    }

    auto minimum = std::numeric_limits<double>::infinity();
    auto maximum = -std::numeric_limits<double>::infinity();
    const auto firstLevel = level ? static_cast<std::size_t>(*level) : std::size_t{0};
    const auto lastLevel = level ? firstLevel + 1 : metadata.levels.size();
    for (auto levelIndex = firstLevel; levelIndex < lastLevel; ++levelIndex) {
        const auto& current = metadata.levels[levelIndex];
        const auto component = static_cast<std::size_t>(field.value);
        for (const auto& block : current.blocks) {
            if (!block.statistics
                || component >= block.statistics->minimum.size()
                || component >= block.statistics->maximum.size()) {
                return std::nullopt;
            }
            const auto blockMinimum = block.statistics->minimum[component];
            const auto blockMaximum = block.statistics->maximum[component];
            if (!std::isfinite(blockMinimum) || !std::isfinite(blockMaximum)
                || blockMinimum > blockMaximum) {
                return std::nullopt;
            }
            minimum = std::min(minimum, blockMinimum);
            maximum = std::max(maximum, blockMaximum);
        }
    }
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
        return std::nullopt;
    }
    return ValueRange{minimum, maximum};
}

std::pair<double, double> paddedIfDegenerate(
    double minimum, double maximum, bool logarithmic) noexcept
{
    if (minimum != maximum) {
        return {minimum, maximum};
    }
    if (logarithmic && minimum > 0.0) {
        return {minimum / (1.0 + 1.0e-6), maximum * (1.0 + 1.0e-6)};
    }
    // Relative to the value, not an absolute floor. The old
    // max(abs(minimum), 1.0) * 1e-6 padded a uniform plane of 1e-7 by 1e-6 --
    // ten times the value itself, straddling zero, which then disqualified the
    // plane from logarithmic display. A constant of 5.0 meanwhile stayed
    // logarithmic, so the same control behaved differently on data that
    // differed only in scale.
    //
    // The floor is the smallest padding that can still separate the two
    // endpoints in double precision near this magnitude: one ulp is far too
    // small to survive later arithmetic, so a relative 1e-6 is used with the
    // smallest normal as a backstop.
    //
    // Zero has no magnitude to be relative to, and the backstop is the wrong
    // answer there: a uniform plane of zeros -- an ordinary case, not a
    // pathological one -- would get a range of +/-2.2e-308, which is what the
    // color bar and the seeded User-range spin boxes then display. Fall back to
    // the relative constant as an absolute, which is what this returned before
    // the padding became relative.
    constexpr auto relative = 1.0e-6;
    const auto magnitude = std::abs(minimum);
    const auto padding = magnitude > 0.0
        ? std::max(magnitude * relative, std::numeric_limits<double>::min())
        : relative;
    return {minimum - padding, maximum + padding};
}

} // namespace amrvis
