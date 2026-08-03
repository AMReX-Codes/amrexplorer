#pragma once

// Compatibility adapter for the Qt Dataset window. The extraction itself is
// owned by the Qt-free data layer so local and remote sessions share the same
// page-bounded contract.

#include <amrexplorer/data/DatasetPage.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

namespace amrvis::qt {

inline constexpr int datasetExtractMaxExtent = datasetPageMaxExtent;
using DatasetLevelExtract = DatasetPage;

[[nodiscard]] inline DatasetLevelExtract extractDatasetLevel(
    PlotfileDataset& dataset, FieldId field, int levelIndex,
    const RealBox& region, int normalAxis, double slicePosition,
    int maxExtent, StopToken cancellation = {})
{
    DatasetPageRequest request;
    request.dataset = dataset.id();
    request.field = field;
    request.level = levelIndex;
    request.region = region;
    request.normalAxis = normalAxis;
    request.slicePosition = slicePosition;
    request.maximumExtent = maxExtent;
    return extractDatasetPage(dataset, request, cancellation);
}

} // namespace amrvis::qt
