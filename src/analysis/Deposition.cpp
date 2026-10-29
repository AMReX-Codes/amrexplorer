#include <amrexplorer/analysis/Deposition.hpp>

#include <amrexplorer/analysis/LeafChunks.hpp>
#include <amrexplorer/core/CoordinateSystem.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace amrvis {
namespace {

int floorDivide(int value, int divisor) noexcept
{
    const auto quotient = value / divisor;
    return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}

int floorIndex(double position, double origin, double width) noexcept
{
    return static_cast<int>(std::floor((position - origin) / width));
}

double overlap(double lower, double upper, double boundLower,
    double boundUpper) noexcept
{
    return std::max(0.0, std::min(upper, boundUpper) - std::max(lower, boundLower));
}

} // namespace

void ParticleDeposit::CompensatedSum::add(double value) noexcept
{
    const auto total = sum + value;
    compensation += std::abs(sum) >= std::abs(value) ? (sum - total) + value
                                                     : (value - total) + sum;
    sum = total;
}

ParticleDeposit::ParticleDeposit(
    const DatasetMetadata& metadata, DepositionRequest request)
    : m_request(request)
{
    if (metadata.dimension != 3 || metadata.isFab || !metadata.hasPhysicalGeometry
        || metadata.coordinateSystem != static_cast<int>(CoordinateSystem::Cartesian)) {
        throw AnalysisError("particle deposition needs a 3-D Cartesian plotfile");
    }
    if (request.axis < 0 || request.axis > 2) {
        throw AnalysisError("deposition axis must be 0, 1 or 2");
    }
    m_imageAxes = request.axis == 0 ? std::array<int, 2>{1, 2}
        : request.axis == 1         ? std::array<int, 2>{0, 2}
                                    : std::array<int, 2>{0, 1};
    const auto [nu, nv] = request.resolution;
    if (nu <= 0 || nv <= 0
        || static_cast<std::uint64_t>(nu) * static_cast<std::uint64_t>(nv)
            > (std::uint64_t{1} << 31U)) {
        throw AnalysisError("deposition resolution must be positive and at most "
                            "2^31 pixels");
    }
    const auto ordered = [](double lo, double hi) {
        return std::isfinite(lo) && std::isfinite(hi) && lo < hi;
    };
    if (!ordered(request.rect[0], request.rect[2])
        || !ordered(request.rect[1], request.rect[3])
        || !ordered(request.axisBounds[0], request.axisBounds[1])) {
        throw AnalysisError("deposition bounds must be finite with lower < upper");
    }
    m_image.assign(static_cast<std::size_t>(nu) * static_cast<std::size_t>(nv), 0.0);

    for (std::size_t index = 0; index < metadata.levels.size(); ++index) {
        const auto& source = metadata.levels[index];
        Level level;
        level.domain = source.domain;
        level.cellSize = source.cellSize;
        level.indexOrigin = source.indexOrigin;
        if (index > 0) {
            const auto& coarse = metadata.levels[index - 1];
            for (std::size_t a = 0; a < 3; ++a) {
                const auto fine = source.domain.upper[a] - source.domain.lower[a] + 1;
                const auto cells = coarse.domain.upper[a] - coarse.domain.lower[a] + 1;
                if (cells <= 0 || fine % cells != 0) {
                    throw AnalysisError("level " + std::to_string(index)
                        + " is not an integer refinement of level "
                        + std::to_string(index - 1));
                }
                level.ratio[a] = fine / cells;
            }
        }
        // Bin the grids on a lattice as coarse as the largest grid, so a
        // point lookup touches one bin and few boxes.
        for (const auto& block : source.blocks) {
            level.boxes.push_back(block.box);
            for (std::size_t a = 0; a < 3; ++a) {
                level.bin = std::max(level.bin, block.box.upper[a] - block.box.lower[a] + 1);
            }
        }
        m_levels.push_back(std::move(level));
    }
    for (auto& level : m_levels) {
        for (std::uint32_t box = 0; box < level.boxes.size(); ++box) {
            const auto& b = level.boxes[box];
            Int3 lo;
            Int3 hi;
            for (std::size_t a = 0; a < 3; ++a) {
                lo[a] = floorDivide(b.lower[a], level.bin);
                hi[a] = floorDivide(b.upper[a], level.bin);
            }
            for (int k = lo[2]; k <= hi[2]; ++k) {
                for (int j = lo[1]; j <= hi[1]; ++j) {
                    for (int i = lo[0]; i <= hi[0]; ++i) {
                        level.bins[binKey(Int3{{i, j, k}})].push_back(box);
                    }
                }
            }
        }
    }
}

std::int64_t ParticleDeposit::binKey(const Int3& bin) const noexcept
{
    constexpr std::int64_t span = std::int64_t{1} << 21;
    return ((static_cast<std::int64_t>(bin[2]) + span / 2) * span
               + (static_cast<std::int64_t>(bin[1]) + span / 2))
            * span
        + (static_cast<std::int64_t>(bin[0]) + span / 2);
}

bool ParticleDeposit::inGrid(int levelIndex, const Int3& cell) const
{
    const auto& level = m_levels[static_cast<std::size_t>(levelIndex)];
    Int3 bin;
    for (std::size_t a = 0; a < 3; ++a) {
        bin[a] = floorDivide(cell[a], level.bin);
    }
    const auto found = level.bins.find(binKey(bin));
    if (found == level.bins.end()) {
        return false;
    }
    for (const auto box : found->second) {
        const auto& b = level.boxes[box];
        if (cell[0] >= b.lower[0] && cell[0] <= b.upper[0] && cell[1] >= b.lower[1]
            && cell[1] <= b.upper[1] && cell[2] >= b.lower[2]
            && cell[2] <= b.upper[2]) {
            return true;
        }
    }
    return false;
}

bool ParticleDeposit::anyGridMeets(int levelIndex, const IntBox& query) const
{
    const auto& level = m_levels[static_cast<std::size_t>(levelIndex)];
    Int3 lo;
    Int3 hi;
    for (std::size_t a = 0; a < 3; ++a) {
        lo[a] = floorDivide(query.lower[a], level.bin);
        hi[a] = floorDivide(query.upper[a], level.bin);
    }
    for (int k = lo[2]; k <= hi[2]; ++k) {
        for (int j = lo[1]; j <= hi[1]; ++j) {
            for (int i = lo[0]; i <= hi[0]; ++i) {
                const auto found = level.bins.find(binKey(Int3{{i, j, k}}));
                if (found == level.bins.end()) {
                    continue;
                }
                for (const auto box : found->second) {
                    const auto& b = level.boxes[box];
                    bool meets = true;
                    for (std::size_t a = 0; a < 3; ++a) {
                        meets = meets && b.lower[a] <= query.upper[a]
                            && b.upper[a] >= query.lower[a];
                    }
                    if (meets) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void ParticleDeposit::fold(int levelIndex, Int3& cell) const noexcept
{
    const auto& domain = m_levels[static_cast<std::size_t>(levelIndex)].domain;
    for (std::size_t a = 0; a < 3; ++a) {
        const auto lo = domain.lower[a];
        const auto hi = domain.upper[a];
        if (m_request.periodic[a]) {
            const auto period = hi - lo + 1;
            cell[a] = lo + ((cell[a] - lo) % period + period) % period;
        } else {
            cell[a] = std::clamp(cell[a], lo, hi);
        }
    }
}

bool ParticleDeposit::resolve(int levelIndex, Int3 cell, double weight,
    double axisPosition, Direction direction)
{
    fold(levelIndex, cell);
    if (!inGrid(levelIndex, cell)) {
        if (direction == Direction::NoCoarsen) {
            return false;
        }
        if (levelIndex == 0) {
            // Level 0 covers the domain in a valid plotfile; keep the weight
            // regardless rather than lose it.
            deposit(0, cell, weight);
            return true;
        }
        const auto& ratio = m_levels[static_cast<std::size_t>(levelIndex)].ratio;
        Int3 parent;
        for (std::size_t a = 0; a < 3; ++a) {
            parent[a] = floorDivide(cell[a], ratio[a]);
        }
        return resolve(levelIndex - 1, parent, weight, axisPosition,
            Direction::NoRefine);
    }
    const auto finer = levelIndex + 1;
    if (direction != Direction::NoRefine
        && static_cast<std::size_t>(finer) < m_levels.size()) {
        const auto& fine = m_levels[static_cast<std::size_t>(finer)];
        IntBox children;
        for (std::size_t a = 0; a < 3; ++a) {
            children.lower[a] = cell[a] * fine.ratio[a];
            children.upper[a] = children.lower[a] + fine.ratio[a] - 1;
        }
        if (anyGridMeets(finer, children)) {
            // Shared by image-plane area among the covering columns, in the
            // fine layer along the line of sight that holds the particle.
            const auto axis = static_cast<std::size_t>(m_request.axis);
            const auto u = static_cast<std::size_t>(m_imageAxes[0]);
            const auto v = static_cast<std::size_t>(m_imageAxes[1]);
            auto layer = floorIndex(axisPosition, fine.indexOrigin[axis],
                fine.cellSize[axis]);
            layer = std::clamp(layer, children.lower[axis], children.upper[axis]);
            const auto share = weight
                / (static_cast<double>(fine.ratio[u])
                    * static_cast<double>(fine.ratio[v]));
            double kept = 0.0;
            for (int b = children.lower[v]; b <= children.upper[v]; ++b) {
                for (int a = children.lower[u]; a <= children.upper[u]; ++a) {
                    Int3 child;
                    child[u] = a;
                    child[v] = b;
                    child[axis] = layer;
                    if (!resolve(finer, child, share, axisPosition,
                            Direction::NoCoarsen)) {
                        kept += share;
                    }
                }
            }
            if (kept > 0.0) {
                deposit(levelIndex, cell, kept);
            }
            return true;
        }
    }
    deposit(levelIndex, cell, weight);
    return true;
}

void ParticleDeposit::deposit(int levelIndex, const Int3& cell, double weight)
{
    const auto& level = m_levels[static_cast<std::size_t>(levelIndex)];
    const auto u = static_cast<std::size_t>(m_imageAxes[0]);
    const auto v = static_cast<std::size_t>(m_imageAxes[1]);
    const auto uLow = level.indexOrigin[u] + static_cast<double>(cell[u]) * level.cellSize[u];
    const auto uHigh = uLow + level.cellSize[u];
    const auto vLow = level.indexOrigin[v] + static_cast<double>(cell[v]) * level.cellSize[v];
    const auto vHigh = vLow + level.cellSize[v];
    const auto [nu, nv] = m_request.resolution;
    const auto umin = m_request.rect[0];
    const auto vmin = m_request.rect[1];
    const auto du = (m_request.rect[2] - umin) / static_cast<double>(nu);
    const auto dv = (m_request.rect[3] - vmin) / static_cast<double>(nv);
    const auto area = level.cellSize[u] * level.cellSize[v];
    double placed = 0.0;
    auto i0 = static_cast<long long>(std::floor((uLow - umin) / du));
    auto i1 = static_cast<long long>(std::floor((uHigh - umin) / du));
    auto j0 = static_cast<long long>(std::floor((vLow - vmin) / dv));
    auto j1 = static_cast<long long>(std::floor((vHigh - vmin) / dv));
    if (!(i1 < 0 || j1 < 0 || i0 >= nu || j0 >= nv)) {
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
                const auto part = weight * (ou * ov / area);
                m_image[static_cast<std::size_t>(j) * static_cast<std::size_t>(nu)
                    + static_cast<std::size_t>(i)] += part;
                placed += part;
            }
        }
    }
    m_outside.add(weight - placed);
}

void ParticleDeposit::add(int levelIndex, std::span<const double> x,
    std::span<const double> y, std::span<const double> z, const double* weights,
    const std::uint8_t* mask)
{
    if (levelIndex < 0 || static_cast<std::size_t>(levelIndex) >= m_levels.size()) {
        throw AnalysisError("particles are stored on a level the plotfile lacks");
    }
    if (x.size() != y.size() || x.size() != z.size()) {
        throw AnalysisError("particle position arrays differ in length");
    }
    const std::array<std::span<const double>, 3> position{x, y, z};
    const auto& level = m_levels[static_cast<std::size_t>(levelIndex)];
    const auto axis = static_cast<std::size_t>(m_request.axis);
    const auto u = static_cast<std::size_t>(m_imageAxes[0]);
    const auto v = static_cast<std::size_t>(m_imageAxes[1]);
    for (std::size_t p = 0; p < x.size(); ++p) {
        if (mask && mask[p] == 0) {
            continue;
        }
        const auto a = position[axis][p];
        const auto weight = weights ? weights[p] : 1.0;
        if (!std::isfinite(position[0][p]) || !std::isfinite(position[1][p])
            || !std::isfinite(a) || !std::isfinite(weight)) {
            throw AnalysisError("a selected particle has a non-finite position "
                                "or weight");
        }
        if (a < m_request.axisBounds[0] || a >= m_request.axisBounds[1]) {
            continue;
        }
        ++m_particles;
        m_selected.add(weight);
        // The lower-left of the four nearest cell centers, and the CIC
        // fractions toward the upper neighbors.
        const auto su = (position[u][p] - level.indexOrigin[u]) / level.cellSize[u] - 0.5;
        const auto sv = (position[v][p] - level.indexOrigin[v]) / level.cellSize[v] - 0.5;
        const auto iu = static_cast<int>(std::floor(su));
        const auto iv = static_cast<int>(std::floor(sv));
        const auto fu = su - iu;
        const auto fv = sv - iv;
        Int3 cell;
        cell[axis] = floorIndex(a, level.indexOrigin[axis], level.cellSize[axis]);
        const std::array<std::array<double, 3>, 4> corners{{
            {0.0, 0.0, (1.0 - fu) * (1.0 - fv)}, {1.0, 0.0, fu * (1.0 - fv)},
            {0.0, 1.0, (1.0 - fu) * fv}, {1.0, 1.0, fu * fv}}};
        for (const auto& corner : corners) {
            if (corner[2] == 0.0) {
                continue;
            }
            cell[u] = iu + static_cast<int>(corner[0]);
            cell[v] = iv + static_cast<int>(corner[1]);
            resolve(levelIndex, cell, weight * corner[2], a, Direction::Any);
        }
    }
}

double ParticleDeposit::pixelArea() const noexcept
{
    return (m_request.rect[2] - m_request.rect[0])
        / static_cast<double>(m_request.resolution[0])
        * (m_request.rect[3] - m_request.rect[1])
        / static_cast<double>(m_request.resolution[1]);
}

} // namespace amrvis
