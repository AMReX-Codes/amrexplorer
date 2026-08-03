#pragma once

#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/query/LineQuery.hpp>
#include <amrexplorer/query/SliceQuery.hpp>

#include <variant>

namespace amrvis {

struct LineViewRequest {
    LineRequest query;
    int outputWidth = 0;
};

using ViewDataRequest = std::variant<SliceRequest, LineViewRequest>;
using ViewDataResult = std::variant<SliceQueryResult, LineQueryResult>;

[[nodiscard]] LineQueryResult boundLineToViewport(
    LineQueryResult result, int outputWidth);

} // namespace amrvis
