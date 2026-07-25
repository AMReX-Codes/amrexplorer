#include <amrexplorer/core/Request.hpp>

namespace amrvis {

std::vector<std::string> validateBlockRequest(const BlockRequest& request)
{
    std::vector<std::string> errors;
    if (request.dataset.value == 0) {
        errors.emplace_back("dataset id must be nonzero");
    }
    if (request.level < 0) {
        errors.emplace_back("level must be non-negative");
    }
    if (request.gridIndex < 0) {
        errors.emplace_back("grid index must be non-negative");
    }
    if (request.firstComponent < 0) {
        errors.emplace_back("first component must be non-negative");
    }
    if (request.componentCount <= 0) {
        errors.emplace_back("component count must be positive");
    }
    if (request.ghostWidth < 0) {
        errors.emplace_back("ghost width must be non-negative");
    }
    return errors;
}

std::vector<std::string> validateSliceRequest(
    const SliceRequest& request, int datasetDimension)
{
    std::vector<std::string> errors;
    if (request.dataset.value == 0) {
        errors.emplace_back("dataset id must be nonzero");
    }
    if (datasetDimension < 2 || datasetDimension > 3) {
        errors.emplace_back("slice requests require a 2-D or 3-D dataset");
    }
    if (request.normalDirection < 0 || request.normalDirection >= datasetDimension) {
        errors.emplace_back("normal direction is outside the dataset dimension");
    }
    if (request.component < 0) {
        errors.emplace_back("component must be non-negative");
    }
    if (request.maximumLevel < 0) {
        errors.emplace_back("maximum level must be non-negative");
    }
    if (request.outputSize[0] <= 0 || request.outputSize[1] <= 0) {
        errors.emplace_back("output dimensions must be positive");
    }
    if (!request.visibleRegion.valid(datasetDimension)) {
        errors.emplace_back("visible region must have positive extent");
    }
    return errors;
}

std::vector<std::string> validateLineRequest(
    const LineRequest& request, int datasetDimension)
{
    std::vector<std::string> errors;
    if (request.dataset.value == 0) {
        errors.emplace_back("dataset id must be nonzero");
    }
    if (datasetDimension < 2 || datasetDimension > 3) {
        errors.emplace_back("line requests require a 2-D or 3-D dataset");
    }
    if (request.axis < 0 || request.axis >= datasetDimension) {
        errors.emplace_back("line axis is outside the dataset dimension");
    }
    if (request.component < 0) {
        errors.emplace_back("component must be non-negative");
    }
    if (request.maximumLevel < 0) {
        errors.emplace_back("maximum level must be non-negative");
    }
    return errors;
}

} // namespace amrvis

