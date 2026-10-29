#include <amrexplorer/analysis/Projection.hpp>

#include <amrexplorer/analysis/LeafChunks.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace amrvis {
namespace {

std::array<int, 2> imageAxes(int axis) noexcept
{
    if (axis == 0) {
        return {1, 2};
    }
    if (axis == 1) {
        return {0, 2};
    }
    return {0, 1};
}

double overlap(double lower, double upper, double boundLower,
    double boundUpper) noexcept
{
    return std::max(0.0, std::min(upper, boundUpper) - std::max(lower, boundLower));
}

void requireOrdered(double lower, double upper, const char* what)
{
    if (!std::isfinite(lower) || !std::isfinite(upper) || !(lower < upper)) {
        throw AnalysisError(std::string("projection ") + what
            + " must be finite with lower < upper");
    }
}

} // namespace

ProjectionResult projectLeafCells(PlotfileDataset& dataset,
    const ProjectionRequest& request, StopToken cancellation)
{
    const auto& metadata = dataset.metadata();
    if (metadata.dimension != 3) {
        throw AnalysisError("projection needs a 3-D plotfile");
    }
    if (request.axis < 0 || request.axis > 2) {
        throw AnalysisError("projection axis must be 0, 1 or 2");
    }
    const auto [uAxis, vAxis] = imageAxes(request.axis);
    const auto [nu, nv] = request.resolution;
    if (nu <= 0 || nv <= 0
        || static_cast<std::uint64_t>(nu) * static_cast<std::uint64_t>(nv)
            > (std::uint64_t{1} << 31U)) {
        throw AnalysisError("projection resolution must be positive and at "
                            "most 2^31 pixels");
    }
    const auto umin = request.rect[0];
    const auto vmin = request.rect[1];
    const auto umax = request.rect[2];
    const auto vmax = request.rect[3];
    requireOrdered(umin, umax, "image bounds");
    requireOrdered(vmin, vmax, "image bounds");
    const auto amin = request.axisBounds[0];
    const auto amax = request.axisBounds[1];
    requireOrdered(amin, amax, "line-of-sight bounds");
    const auto du = (umax - umin) / static_cast<double>(nu);
    const auto dv = (vmax - vmin) / static_cast<double>(nv);

    // Every cell that meets the slab has its center within half a cell of
    // it, and no leaf cell is wider than a level-0 cell, so planning by
    // center over the slab grown by half a level-0 cell finds them all.
    // Cells the grown region adds that do not meet the slab overlap it by
    // zero and contribute nothing.
    const auto& coarse = metadata.levels.front();
    LeafChunkRequest leaf;
    leaf.fields.push_back(request.field);
    if (request.weight && *request.weight != request.field) {
        leaf.fields.push_back(*request.weight);
    }
    leaf.dtype = AnalysisValueType::Float64;
    RealBox region;
    const std::array<std::array<double, 2>, 3> bounds = [&] {
        std::array<std::array<double, 2>, 3> b{};
        b[static_cast<std::size_t>(uAxis)] = {umin, umax};
        b[static_cast<std::size_t>(vAxis)] = {vmin, vmax};
        b[static_cast<std::size_t>(request.axis)] = {amin, amax};
        return b;
    }();
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto half = 0.5 * coarse.cellSize[axis];
        region.lower[axis] = bounds[axis][0] - half;
        region.upper[axis] = std::nextafter(
            bounds[axis][1] + half, std::numeric_limits<double>::infinity());
    }
    leaf.region = region;
    const auto plan = planLeafChunks(dataset, leaf);

    ProjectionResult result;
    result.width = nu;
    result.height = nv;
    const auto pixels = static_cast<std::size_t>(nu) * static_cast<std::size_t>(nv);
    std::vector<double> sum(pixels, 0.0);
    std::vector<double> weightSum(request.weight ? pixels : 0, 0.0);
    std::vector<double> volumeSum(pixels, 0.0);
    const bool sameWeight = request.weight && *request.weight == request.field;

    for (const auto& item : plan.items) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto chunk = readLeafChunk(dataset, item, cancellation);
        const auto& values = std::get<std::vector<double>>(chunk->field(0));
        const std::vector<double>* weights = nullptr;
        if (request.weight) {
            weights = sameWeight
                ? &values
                : &std::get<std::vector<double>>(chunk->field(1));
        }
        const auto& origin = chunk->indexOrigin();
        const auto& dx = chunk->cellSize();
        const auto& iu = chunk->index(uAxis);
        const auto& iv = chunk->index(vAxis);
        const auto& ia = chunk->index(request.axis);
        const auto ua = static_cast<std::size_t>(uAxis);
        const auto va = static_cast<std::size_t>(vAxis);
        const auto aa = static_cast<std::size_t>(request.axis);
        for (std::size_t cell = 0; cell < chunk->size(); ++cell) {
            // Cell faces as Kangaroo's projection computes them, so the two
            // agree on which pixels a face falls between.
            const auto aLow = origin[aa] + static_cast<double>(ia[cell]) * dx[aa];
            const auto oa = overlap(aLow, aLow + dx[aa], amin, amax);
            if (oa <= 0.0) {
                continue;
            }
            const auto uLow = origin[ua] + static_cast<double>(iu[cell]) * dx[ua];
            const auto uHigh = uLow + dx[ua];
            const auto vLow = origin[va] + static_cast<double>(iv[cell]) * dx[va];
            const auto vHigh = vLow + dx[va];
            auto i0 = static_cast<long long>(std::floor((uLow - umin) / du));
            auto i1 = static_cast<long long>(std::floor((uHigh - umin) / du));
            auto j0 = static_cast<long long>(std::floor((vLow - vmin) / dv));
            auto j1 = static_cast<long long>(std::floor((vHigh - vmin) / dv));
            if (i1 < 0 || j1 < 0 || i0 >= nu || j0 >= nv) {
                continue;
            }
            i0 = std::max<long long>(i0, 0);
            j0 = std::max<long long>(j0, 0);
            i1 = std::min<long long>(i1, nu - 1);
            j1 = std::min<long long>(j1, nv - 1);
            const auto value = values[cell];
            const auto weight = weights ? (*weights)[cell] : 1.0;
            bool counted = false;
            for (auto j = j0; j <= j1; ++j) {
                const auto pv0 = vmin + static_cast<double>(j) * dv;
                const auto ov = overlap(vLow, vHigh, pv0, pv0 + dv);
                if (ov <= 0.0) {
                    continue;
                }
                for (auto i = i0; i <= i1; ++i) {
                    const auto pu0 = umin + static_cast<double>(i) * du;
                    const auto ou = overlap(uLow, uHigh, pu0, pu0 + du);
                    if (ou <= 0.0) {
                        continue;
                    }
                    const auto volume = ou * ov * oa;
                    const auto pixel = static_cast<std::size_t>(j)
                            * static_cast<std::size_t>(nu)
                        + static_cast<std::size_t>(i);
                    if (weights) {
                        sum[pixel] += value * weight * volume;
                        weightSum[pixel] += weight * volume;
                        result.directIntegral += value * weight * volume;
                    } else {
                        sum[pixel] += value * volume;
                        result.directIntegral += value * volume;
                    }
                    volumeSum[pixel] += volume;
                    counted = true;
                }
            }
            result.cells += counted ? 1U : 0U;
        }
    }

    result.pixelArea = du * dv;
    result.pathLength = amax - amin;
    result.image.resize(pixels);
    result.coverage.resize(pixels);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        if (request.weight) {
            result.image[pixel] = sum[pixel] / weightSum[pixel];
        } else {
            result.image[pixel] = sum[pixel] / result.pixelArea;
        }
        result.coverage[pixel]
            = volumeSum[pixel] / (result.pixelArea * result.pathLength);
    }
    if (request.weight) {
        result.weightImage.resize(pixels);
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            result.weightImage[pixel] = weightSum[pixel] / result.pixelArea;
        }
    }
    return result;
}

CellAverageSliceResult sliceCellAverage(PlotfileDataset& dataset,
    const CellAverageSliceRequest& request, StopToken cancellation)
{
    const auto& metadata = dataset.metadata();
    if (metadata.dimension != 3) {
        throw AnalysisError("a cell-average slice needs a 3-D plotfile");
    }
    if (request.axis < 0 || request.axis > 2) {
        throw AnalysisError("slice axis must be 0, 1 or 2");
    }
    if (!std::isfinite(request.coordinate)) {
        throw AnalysisError("slice coordinate must be finite");
    }
    const auto [uAxis, vAxis] = imageAxes(request.axis);
    const auto [nu, nv] = request.resolution;
    if (nu <= 0 || nv <= 0
        || static_cast<std::uint64_t>(nu) * static_cast<std::uint64_t>(nv)
            > (std::uint64_t{1} << 31U)) {
        throw AnalysisError("slice resolution must be positive and at most "
                            "2^31 pixels");
    }
    const auto umin = request.rect[0];
    const auto vmin = request.rect[1];
    const auto umax = request.rect[2];
    const auto vmax = request.rect[3];
    requireOrdered(umin, umax, "image bounds");
    requireOrdered(vmin, vmax, "image bounds");
    const auto du = (umax - umin) / static_cast<double>(nu);
    const auto dv = (vmax - vmin) / static_cast<double>(nv);
    const auto aa = static_cast<std::size_t>(request.axis);
    const auto ua = static_cast<std::size_t>(uAxis);
    const auto va = static_cast<std::size_t>(vAxis);

    // As for a projection: plan by center over the image grown by half a
    // level-0 cell, and along the axis over the half cell either side of the
    // plane, then keep the cells the plane actually passes through.
    const auto& coarse = metadata.levels.front();
    LeafChunkRequest leaf;
    leaf.fields.push_back(request.field);
    leaf.dtype = AnalysisValueType::Float64;
    RealBox region;
    const auto infinity = std::numeric_limits<double>::infinity();
    const std::array<std::array<double, 2>, 3> bounds = [&] {
        std::array<std::array<double, 2>, 3> b{};
        b[ua] = {umin, umax};
        b[va] = {vmin, vmax};
        b[aa] = {request.coordinate, request.coordinate};
        return b;
    }();
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto half = 0.5 * coarse.cellSize[axis];
        region.lower[axis] = bounds[axis][0] - half;
        region.upper[axis] = std::nextafter(bounds[axis][1] + half, infinity);
    }
    leaf.region = region;
    const auto plan = planLeafChunks(dataset, leaf);

    CellAverageSliceResult result;
    result.width = nu;
    result.height = nv;
    const auto pixels = static_cast<std::size_t>(nu) * static_cast<std::size_t>(nv);
    std::vector<double> sum(pixels, 0.0);
    std::vector<double> area(pixels, 0.0);
    result.finestLevel.assign(pixels, -1);
    for (const auto& item : plan.items) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto& level = metadata.levels[static_cast<std::size_t>(item.level)];
        // The plane's cell index on this level, as Kangaroo computes it.
        const auto planeIndex = static_cast<std::int32_t>(std::floor(
            (request.coordinate - metadata.physicalDomain.lower[aa])
            / level.cellSize[aa])) + level.domain.lower[aa];
        if (planeIndex < item.selectedBox.lower[aa]
            || planeIndex > item.selectedBox.upper[aa]) {
            continue;
        }
        const auto chunk = readLeafChunk(dataset, item, cancellation);
        const auto& values = std::get<std::vector<double>>(chunk->field(0));
        const auto& origin = chunk->indexOrigin();
        const auto& dx = chunk->cellSize();
        const auto& iu = chunk->index(uAxis);
        const auto& iv = chunk->index(vAxis);
        const auto& ia = chunk->index(request.axis);
        for (std::size_t cell = 0; cell < chunk->size(); ++cell) {
            if (ia[cell] != planeIndex) {
                continue;
            }
            const auto uLow = origin[ua] + static_cast<double>(iu[cell]) * dx[ua];
            const auto uHigh = uLow + dx[ua];
            const auto vLow = origin[va] + static_cast<double>(iv[cell]) * dx[va];
            const auto vHigh = vLow + dx[va];
            auto i0 = static_cast<long long>(std::floor((uLow - umin) / du));
            auto i1 = static_cast<long long>(std::floor((uHigh - umin) / du));
            auto j0 = static_cast<long long>(std::floor((vLow - vmin) / dv));
            auto j1 = static_cast<long long>(std::floor((vHigh - vmin) / dv));
            if (i1 < 0 || j1 < 0 || i0 >= nu || j0 >= nv) {
                continue;
            }
            i0 = std::max<long long>(i0, 0);
            j0 = std::max<long long>(j0, 0);
            i1 = std::min<long long>(i1, nu - 1);
            j1 = std::min<long long>(j1, nv - 1);
            for (auto j = j0; j <= j1; ++j) {
                const auto pv0 = vmin + static_cast<double>(j) * dv;
                const auto ov = overlap(vLow, vHigh, pv0, pv0 + dv);
                if (ov <= 0.0) {
                    continue;
                }
                for (auto i = i0; i <= i1; ++i) {
                    const auto pu0 = umin + static_cast<double>(i) * du;
                    const auto ou = overlap(uLow, uHigh, pu0, pu0 + du);
                    if (ou <= 0.0) {
                        continue;
                    }
                    const auto pixel = static_cast<std::size_t>(j)
                            * static_cast<std::size_t>(nu)
                        + static_cast<std::size_t>(i);
                    sum[pixel] += values[cell] * (ou * ov);
                    area[pixel] += ou * ov;
                    result.finestLevel[pixel] = std::max<std::int16_t>(
                        result.finestLevel[pixel],
                        static_cast<std::int16_t>(item.level));
                }
            }
        }
    }
    result.pixelArea = du * dv;
    result.image.resize(pixels);
    result.coverage.resize(pixels);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        result.image[pixel] = area[pixel] == 0.0
            ? std::numeric_limits<double>::quiet_NaN()
            : sum[pixel] / result.pixelArea;
        result.coverage[pixel] = area[pixel] / result.pixelArea;
    }
    return result;
}

} // namespace amrvis
