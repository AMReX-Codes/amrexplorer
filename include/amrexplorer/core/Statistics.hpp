#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>

#include <optional>
#include <utility>

namespace amrvis {

struct ValueRange {
    double minimum = 0.0;
    double maximum = 0.0;
};

// Returns no value unless every selected block carries usable statistics.
[[nodiscard]] std::optional<ValueRange> metadataValueRange(
    const DatasetMetadata& metadata, FieldId field,
    std::optional<int> level = std::nullopt);

// Widens a degenerate (minimum == maximum) range just enough to have positive
// extent, leaving anything else alone. The padding is *relative* to the value:
// a uniform plane of 1e-7 must not be padded to a range straddling zero, which
// is what an absolute floor did, and which silently disqualified the plane from
// logarithmic display. A strictly positive constant under a logarithmic range
// is widened multiplicatively so it stays positive.
//
// One definition, because there were three: two in SliceRangeResolver and one
// in DisplayCoordinator, and only two of them knew about logarithmic ranges.
// It lives in core because the data layer resolves a volume's visible range
// with it too.
[[nodiscard]] std::pair<double, double> paddedIfDegenerate(
    double minimum, double maximum, bool logarithmic) noexcept;

} // namespace amrvis
