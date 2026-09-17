#include <amrexplorer/render2d/VectorGlyphs.hpp>
#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/render2d/MappedGridWarp.hpp>
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

struct SpeedNormalization {
    double scale = 1.0;
    double speed = 0.0;

    [[nodiscard]] double normalize(double value) const
    {
        return (value / scale) / speed;
    }
};

SpeedNormalization maximumSpeed(
    const ScalarPlane& uComponent, const ScalarPlane& vComponent)
{
    // Keep the squared-speed fast path for ordinary fields. Larger values
    // stay in scaled units: even hypot can overflow on finite components,
    // so never reconstruct their unscaled magnitude.
    constexpr double componentScale = 1.0e150;
    double maxSpeedSquared = 0.0;
    double maxScaledSpeed = 0.0;
    for (std::size_t pixel = 0; pixel < uComponent.values.size(); ++pixel) {
        if (uComponent.valid[pixel] == 0 || vComponent.valid[pixel] == 0) {
            continue;
        }
        const double u = uComponent.values[pixel];
        const double v = vComponent.values[pixel];
        if (!std::isfinite(u) || !std::isfinite(v)) {
            continue;
        }
        if (std::abs(u) > componentScale || std::abs(v) > componentScale) {
            maxScaledSpeed = std::max(maxScaledSpeed,
                std::hypot(u / componentScale, v / componentScale));
        } else {
            maxSpeedSquared = std::max(maxSpeedSquared, u * u + v * v);
        }
    }
    if (maxScaledSpeed > 0.0) {
        return {componentScale,
            std::max(maxScaledSpeed, std::sqrt(maxSpeedSquared) / componentScale)};
    }
    return {1.0, std::sqrt(maxSpeedSquared)};
}

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

    const auto normalization = maximumSpeed(uComponent, vComponent);

    std::vector<VectorSegment> segments;
    if (!(normalization.speed >= minimumMaxSpeed / normalization.scale)) {
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
            const double a = arrowMax * normalization.normalize(u);
            const double b = arrowMax * normalization.normalize(v);
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

std::vector<VectorSegment> generateSphericalRZVectorGlyphs(
    const ScalarPlane& uComponent, const ScalarPlane& vComponent, int count,
    const RealBox& displayRegion)
{
    if (count < 1) {
        throw std::invalid_argument("vector glyph count must be positive");
    }
    if (uComponent.width <= 0 || uComponent.height <= 0) {
        throw std::invalid_argument("vector plane dimensions must be positive");
    }
    if (uComponent.width != vComponent.width
        || uComponent.height != vComponent.height) {
        throw std::invalid_argument("vector component dimensions must match");
    }
    validatePlane(uComponent);
    validatePlane(vComponent);

    // Logical (r, theta) geometry of the sampled grid and physical (R, Z)
    // extent of the display; degenerate spans mean there is nothing to draw.
    const auto& logical = uComponent.physicalRegion;
    const double rLo = logical.lower[0];
    const double rSpan = logical.upper[0] - rLo;
    const double thetaLo = logical.lower[1];
    const double thetaSpan = logical.upper[1] - thetaLo;
    const double displaySpan = std::max(
        displayRegion.upper[0] - displayRegion.lower[0],
        displayRegion.upper[1] - displayRegion.lower[1]);
    std::vector<VectorSegment> segments;
    if (!(rSpan > 0.0) || !(thetaSpan > 0.0) || !(displaySpan > 0.0)) {
        return segments;
    }
    const double dr = rSpan / uComponent.width;
    const double dtheta = thetaSpan / uComponent.height;
    const auto radiusAt = [&](int i) { return rLo + (i + 0.5) * dr; };
    const auto thetaAt = [&](int j) { return thetaLo + (j + 0.5) * dtheta; };

    // Maximum physical speed. Both stored components are physical velocities
    // (v_r and the meridional v_theta), so the speed is their plain norm.
    const auto width = static_cast<std::size_t>(uComponent.width);
    const auto normalization = maximumSpeed(uComponent, vComponent);
    if (!(normalization.speed >= minimumMaxSpeed / normalization.scale)) {
        return segments;
    }

    // Decimation runs over the logical grid exactly like the Cartesian
    // generator; the arrow length scale is physical, derived from the display
    // extent so the fastest arrow spans the same on-screen fraction as in the
    // logical layouts. The near-zero cutoff scales with it (the Cartesian
    // constant is in pixel units, meaningless for physical coordinates).
    const int longestSide = std::max(uComponent.width, uComponent.height);
    const int stride = std::max(1,
        static_cast<int>(static_cast<double>(longestSide) / count));
    const double arrowMax = 1.25 * displaySpan / count;
    const double minimumLength = minimumArrowLength * arrowMax;

    for (int j = 0; j < uComponent.height; j += stride) {
        for (int i = 0; i < uComponent.width; i += stride) {
            const auto pixel = static_cast<std::size_t>(i)
                + static_cast<std::size_t>(j) * width;
            if (uComponent.valid[pixel] == 0 || vComponent.valid[pixel] == 0) {
                continue;
            }
            const double u = uComponent.values[pixel];
            const double v = vComponent.values[pixel];
            if (!std::isfinite(u) || !std::isfinite(v)) {
                continue;
            }
            const double r = radiusAt(i);
            const double theta = thetaAt(j);
            const double sinTheta = std::sin(theta);
            const double cosTheta = std::cos(theta);
            // Rotate the physical (v_r, v_theta) pair into display components
            // along the local unit vectors e_r = (sin, cos),
            // e_theta = (cos, -sin).
            // Normalize before rotating: the sum can overflow even when
            // both original components are finite.
            const double normalizedU = normalization.normalize(u);
            const double normalizedV = normalization.normalize(v);
            const double displayR = normalizedU * sinTheta + normalizedV * cosTheta;
            const double displayZ = normalizedU * cosTheta - normalizedV * sinTheta;
            const double a = arrowMax * displayR;
            const double b = arrowMax * displayZ;
            if (std::hypot(a, b) < minimumLength) {
                continue;
            }
            const auto anchor = sphericalToDisplay(r, theta);
            const auto baseX = static_cast<float>(anchor[0]);
            const auto baseY = static_cast<float>(anchor[1]);
            const auto tipX = static_cast<float>(anchor[0] + a);
            const auto tipY = static_cast<float>(anchor[1] + b);
            segments.push_back(VectorSegment{baseX, baseY, tipX, tipY});
            // Same barb construction as the Cartesian generator; the display
            // plane is isotropic (square physical pitch), so the perpendicular
            // offsets stay perpendicular.
            const auto backX = static_cast<float>(headBack * a);
            const auto backY = static_cast<float>(headBack * b);
            const auto sideX = static_cast<float>(headSide * b);
            const auto sideY = static_cast<float>(headSide * a);
            segments.push_back(VectorSegment{
                tipX, tipY, tipX - backX - sideX, tipY - backY + sideY});
            segments.push_back(VectorSegment{
                tipX, tipY, tipX - backX + sideX, tipY - backY - sideY});
        }
    }
    return segments;
}

std::vector<DisplaySegment> mappedVectorGlyphs(const MappedGridPlane& nodes,
    const std::vector<VectorSegment>& segments, double lengthPerPixel)
{
    std::vector<DisplaySegment> arrows;
    arrows.reserve(segments.size() - segments.size() % 3);
    for (std::size_t arrow = 0; arrow + 2 < segments.size(); arrow += 3) {
        const auto& shaft = segments[arrow];
        const auto base = mappedDisplayPosition(nodes, shaft.x0, shaft.y0);
        if (!std::isfinite(base[0]) || !std::isfinite(base[1])) {
            continue;
        }
        // A point of this arrow, from its offset to the base in plane pixels.
        const auto place = [&](float x, float y) {
            return std::array<double, 2>{
                base[0] + (static_cast<double>(x) - shaft.x0) * lengthPerPixel,
                base[1] + (static_cast<double>(y) - shaft.y0) * lengthPerPixel};
        };
        const auto tip = place(shaft.x1, shaft.y1);
        arrows.push_back({base[0], base[1], tip[0], tip[1]});
        for (std::size_t barb = 1; barb <= 2; ++barb) {
            const auto end = place(segments[arrow + barb].x1, segments[arrow + barb].y1);
            arrows.push_back({tip[0], tip[1], end[0], end[1]});
        }
    }
    return arrows;
}

} // namespace amrvis
