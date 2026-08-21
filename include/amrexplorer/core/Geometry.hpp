#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace amrvis {

struct Int3 {
    std::array<int, 3> values{0, 0, 0};

    [[nodiscard]] constexpr int operator[](std::size_t index) const noexcept
    {
        return values[index];
    }

    constexpr int& operator[](std::size_t index) noexcept { return values[index]; }

    friend constexpr bool operator==(const Int3&, const Int3&) = default;
};

struct Real3 {
    std::array<double, 3> values{0.0, 0.0, 0.0};

    [[nodiscard]] constexpr double operator[](std::size_t index) const noexcept
    {
        return values[index];
    }

    constexpr double& operator[](std::size_t index) noexcept { return values[index]; }

    friend constexpr bool operator==(const Real3&, const Real3&) = default;
};

struct IntBox {
    Int3 lower;
    Int3 upper;
    Int3 centering;

    [[nodiscard]] constexpr bool valid(int dimension) const noexcept
    {
        if (dimension < 1 || dimension > 3) {
            return false;
        }
        for (int axis = 0; axis < dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            if (lower[i] > upper[i]) {
                return false;
            }
        }
        return true;
    }

    friend constexpr bool operator==(const IntBox&, const IntBox&) = default;
};

struct RealBox {
    Real3 lower;
    Real3 upper;

    [[nodiscard]] constexpr bool valid(int dimension) const noexcept
    {
        if (dimension < 1 || dimension > 3) {
            return false;
        }
        for (int axis = 0; axis < dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            // Ordered *and* finite. C++ streams parse "inf", so a corrupt
            // header can hand us lower = -inf, upper = +inf, which is ordered
            // and passes an ordering-only test -- and then upper - lower is
            // inf, and any extent derived from it is inf or NaN. Written as a
            // pair of comparisons rather than std::isfinite because that is not
            // constexpr before C++23 and this function is used in constant
            // expressions. NaN fails the ordering test on its own.
            constexpr auto infinity
                = std::numeric_limits<double>::infinity();
            if (!(lower[i] < upper[i]) || !(lower[i] > -infinity)
                || !(upper[i] < infinity)) {
                return false;
            }
        }
        return true;
    }

    // Each bound halved before the sum, rather than half of the sum or the
    // lower bound plus half the span. Both of those overflow for boxes valid()
    // accepts: lower + upper is infinite for a box spanning most of the range
    // (its centre is then NaN downstream), and lower + 0.5 * span is infinite
    // when the span itself overflows. Halving is exact -- the exponent moves,
    // the mantissa does not -- so this form is no less accurate and stays
    // finite for both.
    [[nodiscard]] constexpr Real3 center() const noexcept
    {
        Real3 result;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            result[axis] = 0.5 * lower[axis] + 0.5 * upper[axis];
        }
        return result;
    }

    friend constexpr bool operator==(const RealBox&, const RealBox&) = default;
};

// Snap a physical region outward to cell boundaries along the given axes:
// the lower edge floors and the upper edge ceils in the index space anchored
// at domain.lower with the given cell size, and the result is clamped to
// domain. Regions derived from screen-pixel selections land mid-cell;
// sampling such a region at one pixel per cell lets pixel centers drift off
// the cell centers until flooring assigns two pixels to one cell or skips
// one. With edges on cell boundaries the extent is an exact multiple of the
// cell size, so pixel centers coincide with cell centers and the floor is
// robust. Expanding outward (rather than snapping nearest) never hides data
// the selection included, and the snapped region always spans at least one
// cell.
[[nodiscard]] inline RealBox snapToCellBoundaries(
    const RealBox& region, const RealBox& domain, const Real3& cellSize,
    const std::array<int, 2>& axes)
{
    auto snapped = region;
    for (const auto axis : axes) {
        const auto i = static_cast<std::size_t>(axis);
        const auto origin = domain.lower[i];
        const auto dx = cellSize[i];
        if (!(dx > 0.0)) {
            continue;
        }
        snapped.lower[i] = std::max(origin,
            origin + std::floor((region.lower[i] - origin) / dx) * dx);
        snapped.upper[i] = std::min(domain.upper[i],
            origin + std::ceil((region.upper[i] - origin) / dx) * dx);
    }
    return snapped;
}

// Snap a region to the nearest cell grid along the given axes, preserving
// the span: the lower edge rounds to the nearest cell boundary and the
// extent rounds to an integer cell count, so the snapped window holds
// round(extent/cellSize) whole cells and stays inside domain (the domain
// edges count as grid edges). Panning translates a cell-aligned region by a
// fractional delta; if the result were kept as-is, pixel centers would land
// on cell boundaries whenever the fractional phase approaches half a cell
// (arrow-key steps of 0.05*N cells hit exactly x.5 after a few presses), and
// the sampler's floor would assign them to either side at random —
// duplicated and skipped rows/columns. Use snapToCellBoundaries for
// selections (it expands), this for translations (it preserves the span).
[[nodiscard]] inline RealBox snapToNearestCellGrid(
    const RealBox& region, const RealBox& domain, const Real3& cellSize,
    const std::array<int, 2>& axes)
{
    auto snapped = region;
    for (const auto axis : axes) {
        const auto i = static_cast<std::size_t>(axis);
        const auto origin = domain.lower[i];
        const auto dx = cellSize[i];
        if (!(dx > 0.0)) {
            continue;
        }
        const auto domainCells = std::max(0.0,
            std::round((domain.upper[i] - origin) / dx));
        // Never more cells than the domain holds. Past that the clamp below
        // collapses to [0, 0], which pins first at zero while upper still
        // lands at origin + cells*dx -- outside the domain, translating the
        // view to the corner. The only caller clamps its region into the
        // domain before calling, so this is not reachable today; the sibling
        // snapToCellBoundaries clamps its upper edge for the same reason, and
        // a geometry helper should not depend on its caller for that.
        const auto cells = std::min(
            std::max(1.0, std::round((region.upper[i] - region.lower[i]) / dx)),
            std::max(1.0, domainCells));
        const auto first = std::clamp(
            std::round((region.lower[i] - origin) / dx),
            0.0, std::max(0.0, domainCells - cells));
        snapped.lower[i] = origin + first * dx;
        snapped.upper[i] = origin + (first + cells) * dx;
    }
    return snapped;
}

} // namespace amrvis

