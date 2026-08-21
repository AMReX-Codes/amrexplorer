#include <amrexplorer/core/Volume.hpp>

#include <cmath>
#include <cstddef>
#include <limits>

namespace amrvis {

std::uint64_t volumeVoxelCount(const std::array<int, 3>& dims) noexcept
{
    constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t result = 1;
    for (const auto extent : dims) {
        const auto value = static_cast<std::uint64_t>(extent);
        if (value != 0 && result > limit / value) {
            return limit;
        }
        result *= value;
    }
    return result;
}

std::vector<std::string> validateVolumeTransferFunction(
    const VolumeTransferFunction& transfer)
{
    std::vector<std::string> errors;
    if (transfer.colors.size() != transfer.opacities.size()) {
        errors.emplace_back(
            "transfer function colors and opacities must have the same size");
    }
    if (transfer.colors.size() < 2) {
        errors.emplace_back("transfer function must have at least two entries");
    }
    if (transfer.colors.size() > maxVolumeTransferEntries
        || transfer.opacities.size() > maxVolumeTransferEntries) {
        errors.emplace_back("transfer function exceeds the entry limit");
    }
    for (const auto color : transfer.colors) {
        if (color > 0x00FFFFFFU) {
            errors.emplace_back(
                "transfer function colors must be 0x00RRGGBB");
            break;
        }
    }
    for (const auto opacity : transfer.opacities) {
        if (!(opacity >= 0.0F) || !(opacity <= 1.0F)) {
            errors.emplace_back(
                "transfer function opacities must be finite and within [0, 1]");
            break;
        }
    }
    return errors;
}

VolumeSampleRequest volumeSampleRequestOf(const VolumeRenderRequest& request)
{
    VolumeSampleRequest sample;
    sample.dataset = request.dataset;
    sample.field = request.field;
    sample.component = request.component;
    sample.maximumLevel = request.maximumLevel;
    sample.composition = request.composition;
    sample.region = request.region;
    sample.maximumVoxels = request.maximumVoxels;
    return sample;
}

std::vector<std::string> validateVolumeSampleRequest(
    const VolumeSampleRequest& request, int datasetDimension)
{
    std::vector<std::string> errors;
    if (request.dataset.value == 0) {
        errors.emplace_back("dataset id must be nonzero");
    }
    if (datasetDimension != 3) {
        // Both the sampler and the renderer validate through here, so the
        // message names neither.
        errors.emplace_back("volume operations require a 3-D dataset");
    }
    if (request.component < 0) {
        errors.emplace_back("component must be non-negative");
    }
    if (request.maximumLevel < 0) {
        errors.emplace_back("maximum level must be non-negative");
    }
    if (!request.region.valid(3)) {
        errors.emplace_back("region must have finite positive extent");
    }
    if (request.maximumVoxels < 1
        || request.maximumVoxels > maxVolumeVoxelBudget) {
        errors.emplace_back("voxel budget is outside the supported range");
    }
    return errors;
}

std::vector<std::string> validateVolumeRenderRequest(
    const VolumeRenderRequest& request, int datasetDimension)
{
    auto errors = validateVolumeSampleRequest(
        volumeSampleRequestOf(request), datasetDimension);
    if (!std::isfinite(request.camera.azimuth)
        || !std::isfinite(request.camera.elevation)) {
        errors.emplace_back("camera angles must be finite");
    }
    if (!(request.camera.zoom >= minVolumeZoom)
        || !(request.camera.zoom <= maxVolumeZoom)) {
        errors.emplace_back("camera zoom is outside the supported range");
    }
    for (const auto extent : request.outputSize) {
        if (extent < 1 || extent > maxVolumeOutputDimension) {
            errors.emplace_back(
                "output dimensions must be within [1, 4096]");
            break;
        }
    }
    if (request.range) {
        const auto& range = *request.range;
        if (!std::isfinite(range.minimum) || !std::isfinite(range.maximum)
            || !(range.minimum < range.maximum)
            || !std::isfinite(range.maximum - range.minimum)) {
            errors.emplace_back(
                "range must be finite with minimum < maximum and a finite span");
        } else if (range.logarithmic && !(range.minimum > 0.0)) {
            errors.emplace_back("a logarithmic range must be strictly positive");
        }
    }
    for (const auto& error : validateVolumeTransferFunction(request.transfer)) {
        errors.push_back(error);
    }
    if (request.samplesPerVoxel < 1
        || request.samplesPerVoxel > maxVolumeSamplesPerVoxel) {
        errors.emplace_back("samples per voxel must be within [1, 8]");
    }
    return errors;
}

} // namespace amrvis
