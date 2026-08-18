#include <amrexplorer/query/VolumeQuery.hpp>
#include <amrexplorer/query/detail/BlockLookup.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace amrvis {
namespace {

using detail::intersects;
using detail::requireBlockPayload;
using detail::valueOffset;

constexpr auto quietNaN = std::numeric_limits<float>::quiet_NaN();

std::uint64_t product(const std::array<int, 3>& dims)
{
    std::uint64_t result = 1;
    for (const auto extent : dims) {
        result *= static_cast<std::uint64_t>(extent);
    }
    return result;
}

// The index box `region` covers at `level`: the cells whose sample positions
// fall inside it, the upper edge nudged inward so a region that ends on a
// cell boundary does not claim the next cell (as the slice planner does).
IntBox regionIndexBox(const RealBox& region, const LevelMetadata& level)
{
    auto result = level.domain;
    for (int axis = 0; axis < 3; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        result.lower[i] = sampleIndex(level, axis, region.lower[i]);
        result.upper[i] = sampleIndex(level, axis,
            std::nextafter(region.upper[i],
                -std::numeric_limits<double>::infinity()));
    }
    return result;
}

} // namespace

std::array<int, 3> volumeGridDims(const DatasetMetadata& metadata,
    const RealBox& region, int maximumLevel, std::uint64_t maximumVoxels)
{
    if (metadata.levels.empty()) {
        return {1, 1, 1};
    }
    const auto& level = metadata.levels[static_cast<std::size_t>(
        std::clamp(maximumLevel, 0, metadata.finestLevel))];
    // Native cell counts, kept far below any product overflow: a single axis
    // never needs more voxels than the whole budget can hold.
    const auto ceiling = static_cast<double>(
        std::max<std::uint64_t>(maximumVoxels, 1));
    std::array<int, 3> dims{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto extent = region.upper[axis] - region.lower[axis];
        const auto cells = std::round(extent / level.cellSize[axis]);
        dims[axis] = static_cast<int>(
            std::clamp(cells, 1.0, std::min(ceiling, 1.0e9)));
    }
    const auto budget = std::max<std::uint64_t>(maximumVoxels, 1);
    if (product(dims) > budget) {
        const auto scale = std::cbrt(static_cast<double>(budget)
            / static_cast<double>(product(dims)));
        for (auto& extent : dims) {
            // The nudge keeps an exact ratio (64 -> 8 is exactly 1/2 per
            // axis) from flooring to the size below when cbrt lands a hair
            // under; the trim below repairs any overshoot it lets through.
            extent = std::max(1, static_cast<int>(
                std::floor(static_cast<double>(extent) * scale + 1.0e-9)));
        }
        // Rounding can leave the product a hair over: trim the largest axis.
        while (product(dims) > budget) {
            auto& largest = *std::max_element(dims.begin(), dims.end());
            if (largest <= 1) {
                break;
            }
            --largest;
        }
    }
    return dims;
}

VolumeQueryResult VolumeQuery::execute(
    const VolumeSampleRequest& request, StopToken cancellation)
{
    const auto& metadata = m_dataset.metadata();
    if (metadata.dimension != 3) {
        throw std::invalid_argument("volume sampling requires a 3-D dataset");
    }
    if (request.dataset != m_dataset.id()) {
        throw std::invalid_argument("volume request targets a different dataset");
    }
    if (request.field.value >= metadata.fields.size()) {
        throw std::invalid_argument("volume field is unavailable");
    }
    if (request.component != 0) {
        throw std::invalid_argument("the initial plotfile fields are scalar");
    }
    if (request.maximumLevel < 0) {
        throw std::invalid_argument("maximum level must be non-negative");
    }
    if (!request.region.valid(3)) {
        throw std::invalid_argument("volume region must have positive extent");
    }
    if (request.maximumVoxels < 1 || request.maximumVoxels > maxVolumeVoxelBudget) {
        throw std::invalid_argument("voxel budget is outside the supported range");
    }
    // The region must lie within the domain (a hair of tolerance for a
    // region computed from the domain itself).
    const auto domain = datasetSampleBounds(metadata);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto tolerance = 1.0e-9
            * (domain.upper[axis] - domain.lower[axis]);
        if (request.region.lower[axis] < domain.lower[axis] - tolerance
            || request.region.upper[axis] > domain.upper[axis] + tolerance) {
            throw std::invalid_argument("volume region lies outside the dataset");
        }
    }

    const auto maximumLevel = std::min(request.maximumLevel, metadata.finestLevel);
    const auto minimumLevel = request.composition == CompositionPolicy::ExactLevel
        ? maximumLevel : 0;
    const auto dims = volumeGridDims(
        metadata, request.region, maximumLevel, request.maximumVoxels);

    VolumeQueryResult result;
    auto& grid = result.grid;
    grid.dims = dims;
    grid.region = request.region;
    grid.maximumLevel = maximumLevel;
    grid.values.assign(static_cast<std::size_t>(product(dims)), quietNaN);

    std::array<double, 3> pitch{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        pitch[axis] = (request.region.upper[axis] - request.region.lower[axis])
            / static_cast<double>(dims[axis]);
    }
    const auto voxelCentre = [&](std::size_t axis, int index) {
        return request.region.lower[axis]
            + (static_cast<double>(index) + 0.5) * pitch[axis];
    };

    // Coarse to fine, so a finer level's cells overwrite the coarser ones
    // beneath them; within a level the grids run backwards, so on a
    // (malformed) overlap the lowest grid index wins, as the slice lookup's
    // first match does.
    for (int levelIndex = minimumLevel; levelIndex <= maximumLevel; ++levelIndex) {
        const auto& level = metadata.levels[static_cast<std::size_t>(levelIndex)];
        const auto queryBox = regionIndexBox(request.region, level);
        for (std::size_t reverse = level.blocks.size(); reverse > 0; --reverse) {
            if (cancellation.stop_requested()) {
                throw ReadCancelled();
            }
            const auto gridIndex = reverse - 1;
            const auto& block = level.blocks[gridIndex];
            if (!intersects(block.box, queryBox, 3)) {
                continue;
            }
            ++result.metrics.candidateBlocks;
            BlockRequest blockRequest;
            blockRequest.dataset = request.dataset;
            blockRequest.level = levelIndex;
            blockRequest.gridIndex = static_cast<int>(gridIndex);
            blockRequest.field = request.field;
            auto access = m_dataset.requestBlock(blockRequest, cancellation);
            if (access.cacheHit) {
                ++result.metrics.cacheHits;
            } else {
                ++result.metrics.blocksRead;
                result.metrics.payloadBytesRead += access.io.bytesRead;
            }
            const auto& fab = *access.handle;
            requireBlockPayload(fab, block.box, 3);

            // The voxels whose centres fall inside the block's cells: per
            // axis, the index range of centres within the block's physical
            // bounds, and each centre's cell index (or -1 when the centre
            // lands in a nodal box's half-cell halo outside the valid box).
            const auto bounds = sampleBounds(level, block.box, 3);
            std::array<int, 3> first{};
            std::array<int, 3> last{};
            std::array<std::vector<int>, 3> cellIndex;
            bool empty = false;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto lower = (bounds.lower[axis] - request.region.lower[axis])
                    / pitch[axis] - 0.5;
                const auto upper = (std::nextafter(bounds.upper[axis],
                        -std::numeric_limits<double>::infinity())
                    - request.region.lower[axis]) / pitch[axis] - 0.5;
                first[axis] = static_cast<int>(
                    std::clamp(std::ceil(lower), 0.0,
                        static_cast<double>(dims[axis])));
                last[axis] = static_cast<int>(
                    std::clamp(std::floor(upper), -1.0,
                        static_cast<double>(dims[axis] - 1)));
                if (last[axis] < first[axis]) {
                    empty = true;
                    break;
                }
                auto& indices = cellIndex[axis];
                indices.resize(static_cast<std::size_t>(last[axis] - first[axis] + 1));
                for (int voxel = first[axis]; voxel <= last[axis]; ++voxel) {
                    const auto cell = sampleIndex(level, static_cast<int>(axis),
                        voxelCentre(axis, voxel));
                    indices[static_cast<std::size_t>(voxel - first[axis])]
                        = cell >= block.box.lower[axis] && cell <= block.box.upper[axis]
                        ? cell : -1;
                }
            }
            if (empty) {
                continue;
            }
            const auto rowStride = static_cast<std::size_t>(dims[0]);
            const auto slabStride = rowStride * static_cast<std::size_t>(dims[1]);
            for (int k = first[2]; k <= last[2]; ++k) {
                if ((k - first[2]) % 32 == 31 && cancellation.stop_requested()) {
                    throw ReadCancelled();
                }
                const auto cellK = cellIndex[2][static_cast<std::size_t>(k - first[2])];
                if (cellK < 0) {
                    continue;
                }
                for (int j = first[1]; j <= last[1]; ++j) {
                    const auto cellJ = cellIndex[1][static_cast<std::size_t>(j - first[1])];
                    if (cellJ < 0) {
                        continue;
                    }
                    auto* row = grid.values.data() + slabStride * static_cast<std::size_t>(k)
                        + rowStride * static_cast<std::size_t>(j);
                    for (int i = first[0]; i <= last[0]; ++i) {
                        const auto cellI = cellIndex[0][static_cast<std::size_t>(i - first[0])];
                        if (cellI < 0) {
                            continue;
                        }
                        Int3 cell;
                        cell[0] = cellI;
                        cell[1] = cellJ;
                        cell[2] = cellK;
                        const auto value = fab.values[valueOffset(fab.box, cell, 3)];
                        row[static_cast<std::size_t>(i)] = std::isfinite(value)
                            ? static_cast<float>(value) : quietNaN;
                    }
                }
            }
        }
    }

    grid.coveredVoxels = static_cast<std::uint64_t>(std::count_if(
        grid.values.begin(), grid.values.end(),
        [](float value) { return !std::isnan(value); }));
    return result;
}

} // namespace amrvis
