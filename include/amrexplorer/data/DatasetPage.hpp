#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/StopToken.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace amrvis {

class PlotfileDataset;

inline constexpr int datasetPageMaxExtent = 512;

struct DatasetPageRequest {
    DatasetId dataset;
    FieldId field;
    int level = 0;
    RealBox region;
    int normalAxis = 2;
    double slicePosition = 0.0;
    int maximumExtent = datasetPageMaxExtent;
};

struct DatasetPage {
    std::array<int, 2> lower{0, 0};
    std::array<int, 2> upper{-1, -1};
    int nx = 0;
    int ny = 0;
    int sliceIndex = 0;
    std::vector<float> values;
    std::vector<std::uint8_t> covered;
    double minimum = 0.0;
    double maximum = 0.0;
    bool hasFiniteValues = false;
    bool truncatedX = false;
    bool truncatedY = false;
};

[[nodiscard]] DatasetPage extractDatasetPage(
    PlotfileDataset& dataset, const DatasetPageRequest& request,
    StopToken cancellation = {});

} // namespace amrvis
