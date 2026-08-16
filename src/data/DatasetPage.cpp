#include <amrexplorer/data/DatasetPage.hpp>

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/query/detail/BlockLookup.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace amrvis {

DatasetPage extractDatasetPage(PlotfileDataset& dataset,
    const DatasetPageRequest& request, StopToken cancellation)
{
    const auto& metadata = dataset.metadata();
    if (request.dataset != dataset.id()) {
        throw std::invalid_argument("dataset page uses the wrong dataset");
    }
    if (metadata.dimension < 2 || metadata.dimension > 3) {
        throw std::invalid_argument(
            "dataset page requires a 2-D or 3-D dataset");
    }
    if (request.level < 0
        || request.level >= static_cast<int>(metadata.levels.size())) {
        throw std::invalid_argument("dataset page level is unavailable");
    }
    if (request.field.value >= metadata.fields.size()) {
        throw std::invalid_argument("dataset page field is unavailable");
    }
    if (metadata.dimension == 3
        && (request.normalAxis < 0 || request.normalAxis > 2)) {
        throw std::invalid_argument("dataset page normal axis is invalid");
    }
    if (request.maximumExtent < 1
        || request.maximumExtent > datasetPageMaxExtent) {
        throw std::invalid_argument("dataset page extent is outside its limit");
    }

    const auto& level
        = metadata.levels[static_cast<std::size_t>(request.level)];
    const auto axes = planeAxes(metadata.dimension, request.normalAxis);
    for (const auto axis : axes) {
        const auto entry = static_cast<std::size_t>(axis);
        const auto lower = request.region.lower[entry];
        const auto upper = request.region.upper[entry];
        if (!std::isfinite(lower) || !std::isfinite(upper)
            || !(lower < upper)) {
            throw std::invalid_argument(
                "dataset page region must have positive finite in-plane "
                "extent");
        }
    }

    DatasetPage page;
    std::array<std::int64_t, 2> lower{};
    std::array<std::int64_t, 2> upper{};
    for (std::size_t entry = 0; entry < 2; ++entry) {
        const auto axis = static_cast<std::size_t>(axes[entry]);
        const auto domainLower
            = static_cast<std::int64_t>(level.domain.lower[axis]);
        const auto domainUpper
            = static_cast<std::int64_t>(level.domain.upper[axis]);
        const auto rawLower = static_cast<std::int64_t>(
            sampleIndex(level, axes[entry], request.region.lower[axis]));
        const auto rawUpper = static_cast<std::int64_t>(
            sampleIndex(level, axes[entry],
                std::nextafter(request.region.upper[axis],
                    -std::numeric_limits<double>::infinity())));
        if (rawUpper < domainLower || rawLower > domainUpper) {
            return page;
        }
        lower[entry] = std::max(rawLower, domainLower);
        upper[entry] = std::min(rawUpper, domainUpper);
    }

    for (std::size_t entry = 0; entry < 2; ++entry) {
        if (upper[entry] - lower[entry] + 1
            > static_cast<std::int64_t>(request.maximumExtent)) {
            upper[entry] = lower[entry] + request.maximumExtent - 1;
            if (entry == 0) {
                page.truncatedX = true;
            } else {
                page.truncatedY = true;
            }
        }
        page.lower[entry] = static_cast<int>(lower[entry]);
        page.upper[entry] = static_cast<int>(upper[entry]);
    }
    page.nx = static_cast<int>(upper[0] - lower[0] + 1);
    page.ny = static_cast<int>(upper[1] - lower[1] + 1);

    if (metadata.dimension == 3) {
        const auto normal = static_cast<std::size_t>(request.normalAxis);
        const auto domainLower
            = static_cast<std::int64_t>(level.domain.lower[normal]);
        const auto domainUpper
            = static_cast<std::int64_t>(level.domain.upper[normal]);
        const auto raw = static_cast<std::int64_t>(
            sampleIndex(level, request.normalAxis, request.slicePosition));
        page.sliceIndex
            = static_cast<int>(std::clamp(raw, domainLower, domainUpper));
    }

    auto query = level.domain;
    for (std::size_t entry = 0; entry < 2; ++entry) {
        const auto axis = static_cast<std::size_t>(axes[entry]);
        query.lower[axis] = page.lower[entry];
        query.upper[axis] = page.upper[entry];
    }
    if (metadata.dimension == 3) {
        const auto normal = static_cast<std::size_t>(request.normalAxis);
        query.lower[normal] = page.sliceIndex;
        query.upper[normal] = page.sliceIndex;
    }

    const auto cellCount = static_cast<std::size_t>(page.nx)
        * static_cast<std::size_t>(page.ny);
    page.values.assign(cellCount, 0.0F);
    page.covered.assign(cellCount, std::uint8_t{0});

    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    auto minimum = std::numeric_limits<double>::infinity();
    auto maximum = -std::numeric_limits<double>::infinity();
    for (std::size_t grid = 0; grid < level.blocks.size(); ++grid) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto validBox = level.blocks[grid].box;
        if (!detail::intersects(validBox, query, metadata.dimension)) {
            continue;
        }
        BlockRequest blockRequest;
        blockRequest.dataset = dataset.id();
        blockRequest.level = request.level;
        blockRequest.gridIndex = static_cast<int>(grid);
        blockRequest.field = request.field;
        blockRequest.firstComponent = 0;
        blockRequest.componentCount = 1;
        const auto access = dataset.requestBlock(blockRequest, cancellation);
        const auto& fab = *access.handle;

        const auto iLower = std::max(validBox.lower[xAxis], page.lower[0]);
        const auto iUpper = std::min(validBox.upper[xAxis], page.upper[0]);
        const auto jLower = std::max(validBox.lower[yAxis], page.lower[1]);
        const auto jUpper = std::min(validBox.upper[yAxis], page.upper[1]);
        // The cells read below come from the catalog's box; the offsets are
        // computed in the FAB's own header box. Nothing upstream cross-checks
        // the two (a v1 VisMF FAB header can disagree with the Header's grid
        // box), and a disagreement aliases into the wrong cell without ever
        // leaving the payload -- the payload is sized to the FAB box exactly.
        // Require the whole cell range to lie in the FAB box, once per block.
        {
            Int3 first{};
            Int3 last{};
            first[xAxis] = iLower;
            last[xAxis] = iUpper;
            first[yAxis] = jLower;
            last[yAxis] = jUpper;
            if (metadata.dimension == 3) {
                first[static_cast<std::size_t>(request.normalAxis)]
                    = page.sliceIndex;
                last[static_cast<std::size_t>(request.normalAxis)]
                    = page.sliceIndex;
            }
            if (iLower <= iUpper && jLower <= jUpper
                && (!detail::contains(fab.box, first, metadata.dimension)
                    || !detail::contains(fab.box, last, metadata.dimension))) {
                throw std::runtime_error(
                    "dataset page block does not cover its catalog box");
            }
        }
        for (auto j = jLower; j <= jUpper; ++j) {
            if (cancellation.stop_requested()) {
                throw ReadCancelled();
            }
            const auto valueY = static_cast<std::size_t>(
                static_cast<std::int64_t>(j) - page.lower[1]);
            for (auto i = iLower; i <= iUpper; ++i) {
                Int3 cell{};
                cell[xAxis] = i;
                cell[yAxis] = j;
                if (metadata.dimension == 3) {
                    cell[static_cast<std::size_t>(request.normalAxis)]
                        = page.sliceIndex;
                }
                const auto value = fab.values[detail::valueOffset(
                    fab.box, cell, metadata.dimension)];
                const auto valueX = static_cast<std::size_t>(
                    static_cast<std::int64_t>(i) - page.lower[0]);
                const auto offset = valueX
                    + static_cast<std::size_t>(page.nx) * valueY;
                page.values[offset] = static_cast<float>(value);
                page.covered[offset] = std::uint8_t{1};
                if (std::isfinite(value)) {
                    minimum = std::min(minimum, value);
                    maximum = std::max(maximum, value);
                }
            }
        }
    }
    if (std::isfinite(minimum) && std::isfinite(maximum)) {
        page.minimum = minimum;
        page.maximum = maximum;
        page.hasFiniteValues = true;
    }
    return page;
}

} // namespace amrvis
