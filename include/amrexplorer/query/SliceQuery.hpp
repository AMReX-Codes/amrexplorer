#pragma once

#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <cstdint>

namespace amrvis {

struct SliceQueryMetrics {
    std::uint64_t candidateBlocks = 0;
    std::uint64_t blocksRead = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t payloadBytesRead = 0;
};

struct SliceQueryResult {
    ScalarPlane plane;
    SliceQueryMetrics metrics;
};

class SliceQuery {
public:
    explicit SliceQuery(PlotfileDataset& dataset) : m_dataset(dataset) {}

    [[nodiscard]] SliceQueryResult execute(
        const SliceRequest& request, StopToken cancellation = {});

private:
    PlotfileDataset& m_dataset;
};

} // namespace amrvis
