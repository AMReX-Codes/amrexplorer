#include <amrexplorer/pipeline/SliceRangeResolver.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace amrvis {

std::optional<std::pair<double, double>> finiteRange(const ScalarPlane& plane)
{
    auto minimum = std::numeric_limits<double>::infinity();
    auto maximum = -std::numeric_limits<double>::infinity();
    for (std::size_t pixel = 0; pixel < plane.values.size(); ++pixel) {
        if (plane.valid[pixel] == 0) {
            continue;
        }
        const auto value = static_cast<double>(plane.values[pixel]);
        if (!std::isfinite(value)) {
            continue;
        }
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
        return std::nullopt;
    }
    if (minimum == maximum) {
        const auto padding = std::max(std::abs(minimum), 1.0) * 1.0e-6;
        minimum -= padding;
        maximum += padding;
    }
    return std::pair{minimum, maximum};
}

std::optional<ValueRange> selectedMetadataRange(
    const DatasetMetadata& metadata, FieldId field, int maximumLevel,
    CompositionPolicy composition, RangeMode rangeMode)
{
    if (rangeMode == RangeMode::File) {
        return metadataValueRange(metadata, field, std::nullopt);
    }
    if (rangeMode != RangeMode::Level) {
        return std::nullopt;
    }
    if (composition == CompositionPolicy::ExactLevel) {
        return metadataValueRange(metadata, field, maximumLevel);
    }

    auto minimum = std::numeric_limits<double>::infinity();
    auto maximum = -std::numeric_limits<double>::infinity();
    for (int level = 0; level <= maximumLevel; ++level) {
        const auto range = metadataValueRange(metadata, field, level);
        if (!range) {
            return std::nullopt;
        }
        minimum = std::min(minimum, range->minimum);
        maximum = std::max(maximum, range->maximum);
    }
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
        return std::nullopt;
    }
    return ValueRange{minimum, maximum};
}

RangeMode effectiveRangeMode(
    const DatasetMetadata& metadata, FieldId field, int maximumLevel,
    CompositionPolicy composition, RangeMode requested)
{
    if (metadata.isFab && requested == RangeMode::File) {
        return requested;
    }
    if ((requested == RangeMode::File || requested == RangeMode::Level)
        && !selectedMetadataRange(
            metadata, field, maximumLevel, composition, requested)) {
        return RangeMode::Visible;
    }
    return requested;
}

RangeMode effectiveRangeMode(
    const std::shared_ptr<DatasetSession>& dataset, FieldId field,
    int maximumLevel, CompositionPolicy composition, RangeMode requested)
{
    if (requested != RangeMode::File && requested != RangeMode::Level) {
        return requested;
    }
    const auto scope = requested == RangeMode::File
        ? RangeScope::File : RangeScope::Level;
    return dataset->rangeAvailable(
               RangeRequest{field, maximumLevel, composition, scope})
        ? requested : RangeMode::Visible;
}

std::optional<std::pair<double, double>> fabDataRange(
    const std::shared_ptr<DatasetSession>& dataset, FieldId field)
{
    if (!dataset->metadata().isFab) {
        return std::nullopt;
    }
    const auto range = dataset->requestRange(RangeRequest{
        .field = field,
        .maximumLevel = 0,
        .composition = CompositionPolicy::ExactLevel,
        .scope = RangeScope::File});
    if (!range) {
        return std::nullopt;
    }
    return std::pair{range->minimum, range->maximum};
}

std::pair<double, double> resolveRange(
    const std::shared_ptr<DatasetSession>& dataset, FieldId field,
    int maximumLevel, CompositionPolicy composition, RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const ScalarPlane& plane)
{
    auto selectedRange = userRange;
    if (rangeMode == RangeMode::Level || rangeMode == RangeMode::File) {
        if (rangeMode == RangeMode::File) {
            selectedRange = fabDataRange(dataset, field);
        }
        if (!selectedRange) {
            const auto statistics = dataset->requestRange(RangeRequest{
                .field = field,
                .maximumLevel = maximumLevel,
                .composition = composition,
                .scope = rangeMode == RangeMode::File
                    ? RangeScope::File
                    : RangeScope::Level});
            if (statistics) {
                selectedRange
                    = std::pair{statistics->minimum, statistics->maximum};
            }
        }
    }
    auto [minimum, maximum] = selectedRange
        ? *selectedRange
        : finiteRange(plane).value_or(logarithmic
              ? std::pair{1.0, 10.0}
              : std::pair{0.0, 1.0});
    if (minimum == maximum) {
        if (logarithmic && minimum > 0.0) {
            minimum /= 1.0 + 1.0e-6;
            maximum *= 1.0 + 1.0e-6;
        } else {
            const auto padding = std::max(std::abs(minimum), 1.0) * 1.0e-6;
            minimum -= padding;
            maximum += padding;
        }
    }
    if (!(minimum < maximum)) {
        throw std::runtime_error("user scalar range must have positive extent");
    }
    if (logarithmic && !(minimum > 0.0)) {
        throw std::runtime_error("logarithmic scalar range must be positive");
    }
    return {minimum, maximum};
}

ResolvedRange resolveDisplayRange(
    const std::shared_ptr<DatasetSession>& dataset, FieldId field,
    int maximumLevel, CompositionPolicy composition, RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const ScalarPlane& plane)
{
    if (logarithmic) {
        try {
            const auto [minimum, maximum] = resolveRange(dataset, field,
                maximumLevel, composition, rangeMode, userRange, true, plane);
            return {minimum, maximum, true};
        } catch (const std::exception&) {
            // Log is not viable for this range; fall back to linear below.
        }
    }
    const auto [minimum, maximum] = resolveRange(dataset, field, maximumLevel,
        composition, rangeMode, userRange, false, plane);
    return {minimum, maximum, false};
}

} // namespace amrvis
