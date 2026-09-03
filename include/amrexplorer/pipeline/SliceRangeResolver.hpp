#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/core/Statistics.hpp>
#include <amrexplorer/core/ValueMapping.hpp>
#include <amrexplorer/data/DatasetSession.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace amrvis {

// How a slice's color-mapping range is chosen. Values are persisted (combo data
// and QSettings), so their order must stay stable.
enum class RangeMode {
    Visible,
    Level,
    File,
    User
};

// A resolved display range plus whether logarithmic mapping actually applied
// (resolveDisplayRange falls back to linear when a log range is not viable).
struct ResolvedRange {
    double minimum;
    double maximum;
    bool logarithmic;
    ColorScaleConfig scale;
};

// paddedIfDegenerate, once declared here, now lives in core/Statistics.hpp
// (included above) so the data layer can pad a volume's visible range too.

// Finite extrema of a plane, padded so minimum < maximum; nullopt when the
// plane has no finite valid samples.
[[nodiscard]] std::optional<std::pair<double, double>> finiteRange(
    const ScalarPlane& plane, bool logarithmic = false);

// The metadata (File/Level) range for the selection, or nullopt when the
// statistics needed for that mode are unavailable.
[[nodiscard]] std::optional<ValueRange> selectedMetadataRange(
    const DatasetMetadata& metadata, FieldId field, int maximumLevel,
    CompositionPolicy composition, RangeMode rangeMode);

// The requested range mode, downgraded to Visible when File/Level statistics
// are unavailable for the selection.
[[nodiscard]] RangeMode effectiveRangeMode(
    const DatasetMetadata& metadata, FieldId field, int maximumLevel,
    CompositionPolicy composition, RangeMode requested);
[[nodiscard]] RangeMode effectiveRangeMode(
    const std::shared_ptr<DatasetSession>& dataset, FieldId field,
    int maximumLevel, CompositionPolicy composition, RangeMode requested);

// The finite extrema of a standalone FAB's payload, or nullopt for non-FAB
// datasets / all-non-finite data.
[[nodiscard]] std::optional<std::pair<double, double>> fabDataRange(
    const std::shared_ptr<DatasetSession>& dataset, FieldId field,
    StopToken cancellation = {});

// The display range for a slice: the user's explicit range, the level/file
// metadata range, or the finite extrema of the plane itself, padded so
// minimum < maximum always holds. A logarithmic request whose range is not
// strictly positive throws, so the caller can fall back to linear. Shared by
// executeSlice and the re-render-from-cache path, which must agree exactly.
[[nodiscard]] std::pair<double, double> resolveRange(
    const std::shared_ptr<DatasetSession>& dataset, FieldId field,
    int maximumLevel, CompositionPolicy composition, RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const ScalarPlane& plane, StopToken cancellation = {});
[[nodiscard]] ResolvedRange resolveDisplayRange(
    const std::shared_ptr<DatasetSession>& dataset, FieldId field,
    int maximumLevel, CompositionPolicy composition, RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    ColorScaleConfig scale, const ScalarPlane& plane,
    StopToken cancellation = {});

// Like resolveRange, but if a logarithmic scale is requested and the range is
// not strictly positive it falls back to a linear range and reports
// logarithmic=false, so the caller renders linearly instead of failing the
// whole slice. A slice with no finite values uses a neutral positive range and
// can therefore remain logarithmic.
[[nodiscard]] ResolvedRange resolveDisplayRange(
    const std::shared_ptr<DatasetSession>& dataset, FieldId field,
    int maximumLevel, CompositionPolicy composition, RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const ScalarPlane& plane, StopToken cancellation = {});

} // namespace amrvis
