#pragma once

#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/query/LineQuery.hpp>
#include <amrexplorer/query/SliceQuery.hpp>

#include <cstdint>
#include <variant>

namespace amrvis {

inline constexpr int maxViewOutputDimension = 4096;
inline constexpr std::uint64_t sliceResponseOverheadBytes = 512;
// Derived from the plane's own members so widening one cannot leave this
// behind. A client reserves against this unconditionally, even talking to a
// server old enough to answer in floats: over-reserving there only asks for a
// smaller raster than it could have had, while under-reserving against a
// current server is a hard ResourceLimitExceeded.
inline constexpr std::uint64_t sliceResponseBytesPerCell
    = sizeof(decltype(ScalarPlane::values)::value_type)
    + sizeof(decltype(ScalarPlane::valid)::value_type)
    + sizeof(decltype(ScalarPlane::sourceLevel)::value_type);

struct LineViewRequest {
    LineRequest query;
    int outputWidth = 0;
};

using ViewDataRequest = std::variant<SliceRequest, LineViewRequest>;
using ViewDataResult = std::variant<SliceQueryResult, LineQueryResult>;

[[nodiscard]] LineQueryResult boundLineToViewport(
    LineQueryResult result, int outputWidth);

} // namespace amrvis
