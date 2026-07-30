#pragma once

#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <cstdint>
#include <vector>

namespace amrvis {

struct SliceQueryMetrics {
    std::uint64_t candidateBlocks = 0;
    std::uint64_t blocksRead = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t payloadBytesRead = 0;
};

struct SliceGridBox {
    int level = 0;
    RealBox physicalRegion;
};

struct SliceQueryResult {
    ScalarPlane plane;
    SliceQueryMetrics metrics;
    bool gridBoxesIncluded = false;
    bool gridBoxesTruncated = false;
    std::vector<SliceGridBox> gridBoxes;
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
