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

} // namespace amrvis
