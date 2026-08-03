#pragma once

#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/query/LineQuery.hpp>
#include <amrexplorer/query/SliceQuery.hpp>

#include <cstdint>
#include <variant>

namespace amrvis {

inline constexpr int maxViewOutputDimension = 4096;
inline constexpr std::uint64_t sliceResponseOverheadBytes = 512;
inline constexpr std::uint64_t sliceResponseBytesPerCell
    = sizeof(float) + sizeof(std::uint8_t) + sizeof(std::int16_t);

struct LineViewRequest {
    LineRequest query;
    int outputWidth = 0;
};

using ViewDataRequest = std::variant<SliceRequest, LineViewRequest>;
using ViewDataResult = std::variant<SliceQueryResult, LineQueryResult>;

[[nodiscard]] LineQueryResult boundLineToViewport(
    LineQueryResult result, int outputWidth);

} // namespace amrvis
