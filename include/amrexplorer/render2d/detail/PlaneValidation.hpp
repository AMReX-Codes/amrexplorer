#pragma once

// The one definition of "this ScalarPlane's storage matches its dimensions",
// previously reimplemented with drifting strictness by the renderer, the
// glyph generator, and the contour extractor. Extent policy is explicit:
// rendering requires a positive extent, while contour extraction legitimately
// accepts empty planes (and returns no segments).

#include <amrexplorer/core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace amrvis::detail {

enum class PlaneExtent : std::uint8_t {
    RequirePositive,
    AllowEmpty
};

inline void validatePlaneStorage(const ScalarPlane& plane, PlaneExtent extent)
{
    if (extent == PlaneExtent::RequirePositive) {
        if (plane.width <= 0 || plane.height <= 0) {
            throw std::invalid_argument(
                "scalar plane dimensions must be positive");
        }
    } else if (plane.width < 0 || plane.height < 0) {
        throw std::invalid_argument(
            "scalar plane dimensions must not be negative");
    }
    const auto pixelCount = static_cast<std::uint64_t>(plane.width)
        * static_cast<std::uint64_t>(plane.height);
    if (pixelCount > std::numeric_limits<std::size_t>::max()
        || plane.values.size() != static_cast<std::size_t>(pixelCount)
        || plane.valid.size() != static_cast<std::size_t>(pixelCount)) {
        throw std::invalid_argument(
            "scalar plane storage does not match its dimensions");
    }
}

} // namespace amrvis::detail
