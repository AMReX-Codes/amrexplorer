#pragma once

#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/AmrDataset.hpp>
#include <amrexplorer/query/SliceQuery.hpp>

#include <cstdint>

namespace amrvis {

struct LineQueryResult {
    LineResult line;
    SliceQueryMetrics metrics;
};

class LineQuery {
public:
    explicit LineQuery(AmrDataset& dataset) : m_dataset(dataset) {}

    [[nodiscard]] LineQueryResult execute(
        const LineRequest& request, StopToken cancellation = {});

private:
    AmrDataset& m_dataset;
};

} // namespace amrvis
