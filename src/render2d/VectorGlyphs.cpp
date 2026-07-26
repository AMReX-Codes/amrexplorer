#include <amrexplorer/render2d/VectorGlyphs.hpp>
#include <amrexplorer/render2d/detail/PlaneValidation.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace amrvis {

namespace {

constexpr double minimumMaxSpeed = 1.0e-8;
constexpr double minimumArrowLength = 1.0e-6;
constexpr double headBack = 0.25;
constexpr double headSide = 0.125;

void validatePlane(const ScalarPlane& plane)
{
    // AllowEmpty keeps the caller's own positive-extent check authoritative;
    // the shared validator adds the negative-dimension rejection this copy
    // previously lacked.
    detail::validatePlaneStorage(plane, detail::PlaneExtent::AllowEmpty);
}

} // namespace

std::vector<VectorSegment> generateVectorGlyphs(
    const ScalarPlane& uComponent, const ScalarPlane& vComponent, int count)
{
    if (count < 1) {
        throw std::invalid_argument("vector glyph count must be positive");
    }
    if (uComponent.width <= 0 || uComponent.height <= 0) {
        throw std::invalid_argument("vector plane dimensions must be positive");
    }
    if (uComponent.width != vComponent.width || uComponent.height != vComponent.height) {
        throw std::invalid_argument("vector component dimensions must match");
    }
    validatePlane(uComponent);
    validatePlane(vComponent);

    const auto pixelCount = static_cast<std::size_t>(uComponent.width)
        * static_cast<std::size_t>(uComponent.height);

    double maxSpeed = 0.0;
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
        if (uComponent.valid[pixel] == 0 || vComponent.valid[pixel] == 0) {
            continue;
        }
        const double u = uComponent.values[pixel];
        const double v = vComponent.values[pixel];
        if (!std::isfinite(u) || !std::isfinite(v)) {
            continue;
        }
        maxSpeed = std::max(maxSpeed, std::hypot(u, v));
    }

    std::vector<VectorSegment> segments;
    if (!(maxSpeed >= minimumMaxSpeed)) {
        return segments;
    }

    // Partition the longest side, then truncate to the stride. Floating-point
    // division keeps sight (and thus arrowMax) nonzero when count exceeds the
    // longest side, so small planes still draw glyphs instead of vanishing.
    const int longestSide = std::max(uComponent.width, uComponent.height);
    const double sight = static_cast<double>(longestSide) / count;
    const int stride = std::max(1, static_cast<int>(sight));
    const double arrowMax = 1.25 * sight;

    for (int j = 0; j < uComponent.height; j += stride) {
        for (int i = 0; i < uComponent.width; i += stride) {
            const auto pixel = static_cast<std::size_t>(i)
                + static_cast<std::size_t>(j) * static_cast<std::size_t>(uComponent.width);
            if (uComponent.valid[pixel] == 0 || vComponent.valid[pixel] == 0) {
                continue;
            }
            const double u = uComponent.values[pixel];
            const double v = vComponent.values[pixel];
            if (!std::isfinite(u) || !std::isfinite(v)) {
                continue;
            }
            const double a = arrowMax * (u / maxSpeed);
            const double b = arrowMax * (v / maxSpeed);
            if (std::hypot(a, b) < minimumArrowLength) {
                continue;
            }
            const auto baseX = static_cast<float>(i) + 0.5F;
            const auto baseY = static_cast<float>(j) + 0.5F;
            const auto tipX = static_cast<float>(baseX + a);
            const auto tipY = static_cast<float>(baseY + b);
            segments.push_back(VectorSegment{baseX, baseY, tipX, tipY});
            const auto backX = static_cast<float>(headBack * a);
            const auto backY = static_cast<float>(headBack * b);
            const auto sideX = static_cast<float>(headSide * b);
            const auto sideY = static_cast<float>(headSide * a);
            segments.push_back(
                VectorSegment{tipX, tipY, tipX - backX - sideX, tipY - backY + sideY});
            segments.push_back(
                VectorSegment{tipX, tipY, tipX - backX + sideX, tipY - backY - sideY});
        }
    }
    return segments;
}

} // namespace amrvis
