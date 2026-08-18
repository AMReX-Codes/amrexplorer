// The volume-render request's structural validator: every bounded field
// rejects out-of-range and non-finite input, and a well-formed request passes.

#include <amrexplorer/core/Volume.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

amrvis::VolumeRenderRequest validRequest()
{
    amrvis::VolumeRenderRequest request;
    request.dataset.value = 7;
    request.field.value = 1;
    request.maximumLevel = 1;
    request.region.lower = {{0.0, 0.0, 0.0}};
    request.region.upper = {{1.0, 2.0, 3.0}};
    request.camera = {0.4, -0.3, 1.5};
    request.outputSize = {320, 240};
    request.transfer.colors = {0x0000FFU, 0x00FF00U, 0xFF0000U};
    request.transfer.opacities = {0.0F, 0.5F, 1.0F};
    return request;
}

bool rejected(const amrvis::VolumeRenderRequest& request, int dimension = 3)
{
    return !amrvis::validateVolumeRenderRequest(request, dimension).empty();
}

} // namespace

int main()
{
    constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
    constexpr auto infinity = std::numeric_limits<double>::infinity();

    require(!rejected(validRequest()), "a well-formed request was rejected");
    {
        auto request = validRequest();
        request.range = amrvis::VolumeRange{0.5, 2.0, false};
        require(!rejected(request), "an explicit range was rejected");
        request.range = amrvis::VolumeRange{0.5, 2.0, true};
        require(!rejected(request), "a positive logarithmic range was rejected");
    }

    require(rejected(validRequest(), 2), "a 2-D dataset was accepted");
    {
        auto request = validRequest();
        request.dataset.value = 0;
        require(rejected(request), "a zero dataset id was accepted");
    }
    {
        auto request = validRequest();
        request.component = -1;
        require(rejected(request), "a negative component was accepted");
        request = validRequest();
        request.maximumLevel = -1;
        require(rejected(request), "a negative maximum level was accepted");
    }
    {
        auto request = validRequest();
        request.region.upper[1] = request.region.lower[1];
        require(rejected(request), "a degenerate region was accepted");
        request = validRequest();
        request.region.upper[0] = infinity;
        require(rejected(request), "an infinite region was accepted");
    }
    {
        auto request = validRequest();
        request.camera.azimuth = nan;
        require(rejected(request), "a NaN azimuth was accepted");
        request = validRequest();
        request.camera.elevation = infinity;
        require(rejected(request), "an infinite elevation was accepted");
        request = validRequest();
        request.camera.zoom = 0.0;
        require(rejected(request), "a zero zoom was accepted");
        request.camera.zoom = nan;
        require(rejected(request), "a NaN zoom was accepted");
        request.camera.zoom = amrvis::maxVolumeZoom * 2.0;
        require(rejected(request), "an oversized zoom was accepted");
    }
    {
        auto request = validRequest();
        request.outputSize = {0, 240};
        require(rejected(request), "a zero-width output was accepted");
        request.outputSize = {320, amrvis::maxVolumeOutputDimension + 1};
        require(rejected(request), "an over-limit output height was accepted");
    }
    {
        auto request = validRequest();
        request.range = amrvis::VolumeRange{2.0, 2.0, false};
        require(rejected(request), "an empty range was accepted");
        request.range = amrvis::VolumeRange{nan, 2.0, false};
        require(rejected(request), "a NaN range was accepted");
        request.range = amrvis::VolumeRange{-1.0, 2.0, true};
        require(rejected(request), "a non-positive logarithmic range was accepted");
    }
    {
        auto request = validRequest();
        request.transfer.opacities.pop_back();
        require(rejected(request), "mismatched transfer sizes were accepted");
        request = validRequest();
        request.transfer.colors = {0x0U};
        request.transfer.opacities = {1.0F};
        require(rejected(request), "a one-entry transfer function was accepted");
        request = validRequest();
        request.transfer.opacities[1] = 1.5F;
        require(rejected(request), "an opacity above one was accepted");
        request.transfer.opacities[1] = std::numeric_limits<float>::quiet_NaN();
        require(rejected(request), "a NaN opacity was accepted");
        request = validRequest();
        request.transfer.colors.assign(amrvis::maxVolumeTransferEntries + 1, 0U);
        request.transfer.opacities.assign(
            amrvis::maxVolumeTransferEntries + 1, 0.5F);
        require(rejected(request), "an over-limit transfer function was accepted");
    }
    {
        auto request = validRequest();
        request.samplesPerVoxel = 0;
        require(rejected(request), "zero samples per voxel was accepted");
        request.samplesPerVoxel = amrvis::maxVolumeSamplesPerVoxel + 1;
        require(rejected(request), "too many samples per voxel was accepted");
    }
    {
        auto request = validRequest();
        request.maximumVoxels = 0;
        require(rejected(request), "a zero voxel budget was accepted");
        request.maximumVoxels = amrvis::maxVolumeVoxelBudget + 1;
        require(rejected(request), "an over-cap voxel budget was accepted");
    }
    return 0;
}
