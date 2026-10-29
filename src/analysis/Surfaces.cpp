#include <amrexplorer/analysis/Surfaces.hpp>

#include <amrexplorer/analysis/LeafChunks.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace amrvis {
namespace {

double minDistanceSquared(double a0, double a1) noexcept
{
    if (a1 < 0.0) {
        return a1 * a1;
    }
    if (a0 > 0.0) {
        return a0 * a0;
    }
    return 0.0;
}

double maxDistanceSquared(double a0, double a1) noexcept
{
    const auto amax = std::max(std::abs(a0), std::abs(a1));
    return amax * amax;
}

bool sphereMayIntersect(double radius2, double x0, double x1, double y0,
    double y1, double z0, double z1) noexcept
{
    const auto r2Min = minDistanceSquared(x0, x1) + minDistanceSquared(y0, y1)
        + minDistanceSquared(z0, z1);
    const auto r2Max = maxDistanceSquared(x0, x1) + maxDistanceSquared(y0, y1)
        + maxDistanceSquared(z0, z1);
    return radius2 >= r2Min && radius2 <= r2Max;
}

bool cylinderMayIntersect(double radius2, double height, double x0, double x1,
    double y0, double y1, double z0, double z1) noexcept
{
    const auto r2Min = minDistanceSquared(x0, x1) + minDistanceSquared(y0, y1);
    const auto r2Max = maxDistanceSquared(x0, x1) + maxDistanceSquared(y0, y1);
    return radius2 >= r2Min && radius2 <= r2Max && z1 >= -height && z0 <= height;
}

double planeBoxTolerance(double x0, double x1, double y0, double y1, double z0,
    double z1, double d) noexcept
{
    const auto scale = std::abs(x0) + std::abs(x1) + std::abs(y0) + std::abs(y1)
        + std::abs(z0) + std::abs(z1) + std::abs(d) + 1.0;
    return 1.0e-12 * scale;
}

struct Point {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

void addUnique(std::array<Point, 16>& points, int& count, double x, double y,
    double z, double tolerance) noexcept
{
    const auto tolerance2 = tolerance * tolerance;
    for (int i = 0; i < count; ++i) {
        const auto& p = points[static_cast<std::size_t>(i)];
        const auto dx = p.x - x;
        const auto dy = p.y - y;
        const auto dz = p.z - z;
        if (dx * dx + dy * dy + dz * dz <= tolerance2) {
            return;
        }
    }
    if (count < static_cast<int>(points.size())) {
        points[static_cast<std::size_t>(count++)] = Point{x, y, z};
    }
}

// The area of the polygon in which the plane n.x = d cuts the box: its
// vertices are where the plane crosses the box's edges, ordered by angle about
// their centroid in the plane.
double planeBoxSectionArea(double x0, double x1, double y0, double y1,
    double z0, double z1, double nx, double ny, double nz, double d)
{
    const auto tolerance = planeBoxTolerance(x0, x1, y0, y1, z0, z1, d);
    const std::array<Point, 8> vertices{Point{x0, y0, z0}, Point{x1, y0, z0},
        Point{x0, y1, z0}, Point{x1, y1, z0}, Point{x0, y0, z1},
        Point{x1, y0, z1}, Point{x0, y1, z1}, Point{x1, y1, z1}};
    constexpr std::array<std::array<int, 2>, 12> edges{{{0, 1}, {2, 3}, {4, 5},
        {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6},
        {3, 7}}};
    std::array<Point, 16> points{};
    int count = 0;
    for (const auto& edge : edges) {
        const auto& p0 = vertices[static_cast<std::size_t>(edge[0])];
        const auto& p1 = vertices[static_cast<std::size_t>(edge[1])];
        auto f0 = nx * p0.x + ny * p0.y + nz * p0.z - d;
        auto f1 = nx * p1.x + ny * p1.y + nz * p1.z - d;
        if (std::abs(f0) <= tolerance) {
            f0 = 0.0;
        }
        if (std::abs(f1) <= tolerance) {
            f1 = 0.0;
        }
        if (f0 == 0.0 && f1 == 0.0) {
            addUnique(points, count, p0.x, p0.y, p0.z, tolerance);
            addUnique(points, count, p1.x, p1.y, p1.z, tolerance);
            continue;
        }
        if (f0 == 0.0) {
            addUnique(points, count, p0.x, p0.y, p0.z, tolerance);
            continue;
        }
        if (f1 == 0.0) {
            addUnique(points, count, p1.x, p1.y, p1.z, tolerance);
            continue;
        }
        if ((f0 < 0.0 && f1 > 0.0) || (f0 > 0.0 && f1 < 0.0)) {
            const auto t = f0 / (f0 - f1);
            addUnique(points, count, p0.x + t * (p1.x - p0.x),
                p0.y + t * (p1.y - p0.y), p0.z + t * (p1.z - p0.z), tolerance);
        }
    }
    if (count < 3) {
        return 0.0;
    }
    double cx = 0.0;
    double cy = 0.0;
    double cz = 0.0;
    for (int i = 0; i < count; ++i) {
        const auto& p = points[static_cast<std::size_t>(i)];
        cx += p.x;
        cy += p.y;
        cz += p.z;
    }
    cx /= static_cast<double>(count);
    cy /= static_cast<double>(count);
    cz /= static_cast<double>(count);

    // An orthonormal basis (e1, e2) of the plane.
    double ax = 1.0;
    double ay = 0.0;
    double az = 0.0;
    if (std::abs(nx) > 0.9) {
        ax = 0.0;
        ay = 1.0;
    }
    auto e1x = ny * az - nz * ay;
    auto e1y = nz * ax - nx * az;
    auto e1z = nx * ay - ny * ax;
    const auto e1norm = std::sqrt(e1x * e1x + e1y * e1y + e1z * e1z);
    if (e1norm <= 0.0) {
        return 0.0;
    }
    e1x /= e1norm;
    e1y /= e1norm;
    e1z /= e1norm;
    const auto e2x = ny * e1z - nz * e1y;
    const auto e2y = nz * e1x - nx * e1z;
    const auto e2z = nx * e1y - ny * e1x;

    std::array<double, 16> u{};
    std::array<double, 16> v{};
    std::array<double, 16> angle{};
    for (int i = 0; i < count; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const auto rx = points[index].x - cx;
        const auto ry = points[index].y - cy;
        const auto rz = points[index].z - cz;
        u[index] = rx * e1x + ry * e1y + rz * e1z;
        v[index] = rx * e2x + ry * e2y + rz * e2z;
        angle[index] = std::atan2(v[index], u[index]);
    }
    // Insertion sort by angle: at most 16 points, and stable, as Kangaroo's.
    for (int i = 1; i < count; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const auto keyAngle = angle[index];
        const auto keyU = u[index];
        const auto keyV = v[index];
        int j = i - 1;
        while (j >= 0 && angle[static_cast<std::size_t>(j)] > keyAngle) {
            const auto to = static_cast<std::size_t>(j + 1);
            const auto from = static_cast<std::size_t>(j);
            angle[to] = angle[from];
            u[to] = u[from];
            v[to] = v[from];
            --j;
        }
        const auto to = static_cast<std::size_t>(j + 1);
        angle[to] = keyAngle;
        u[to] = keyU;
        v[to] = keyV;
    }
    double area2 = 0.0;
    for (int i = 0; i < count; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const auto next = static_cast<std::size_t>(i + 1 < count ? i + 1 : 0);
        area2 += u[index] * v[next] - v[index] * u[next];
    }
    return 0.5 * std::abs(area2);
}

double sphereSectionArea(double radius, double x0, double x1, double y0,
    double y1, double z0, double z1)
{
    if ((x1 - x0) * (y1 - y0) * (z1 - z0) <= 0.0) {
        return 0.0;
    }
    const auto xc = 0.5 * (x0 + x1);
    const auto yc = 0.5 * (y0 + y1);
    const auto zc = 0.5 * (z0 + z1);
    const auto rc = std::sqrt(xc * xc + yc * yc + zc * zc);
    if (rc <= 0.0) {
        return 0.0;
    }
    return planeBoxSectionArea(
        x0, x1, y0, y1, z0, z1, xc / rc, yc / rc, zc / rc, radius);
}

double cylinderWallArea(double radius, double height, double x0, double x1,
    double y0, double y1, double z0, double z1)
{
    const auto zlo = std::max(z0, -height);
    const auto zhi = std::min(z1, height);
    if (zhi <= zlo) {
        return 0.0;
    }
    const auto xc = 0.5 * (x0 + x1);
    const auto yc = 0.5 * (y0 + y1);
    const auto rc = std::sqrt(xc * xc + yc * yc);
    if (rc <= 0.0) {
        return 0.0;
    }
    const auto nx = xc / rc;
    const auto ny = yc / rc;
    const auto tolerance = planeBoxTolerance(x0, x1, y0, y1, zlo, zhi, radius);
    const auto nearest = nx * (nx >= 0.0 ? x0 : x1) + ny * (ny >= 0.0 ? y0 : y1)
        - radius;
    // A tangent plane on a shared face belongs to the interior-side cell: it
    // needs a vertex strictly inside, or an exterior cell touching the plane
    // would count the face again.
    if (nearest >= -tolerance) {
        return 0.0;
    }
    return planeBoxSectionArea(x0, x1, y0, y1, zlo, zhi, nx, ny, 0.0, radius);
}

// A rectangle in the positive quadrant of the unit disk, split where the
// circle first cuts the rectangle's upper edge: left of the split it is a
// rectangle, right of it the upper boundary is sqrt(1 - x^2).
DiskSectionMoments quadrantDiskMoments(double a, double b, double c, double d)
{
    a = std::clamp(a, 0.0, 1.0);
    b = std::clamp(b, 0.0, 1.0);
    c = std::clamp(c, 0.0, 1.0);
    d = std::clamp(d, 0.0, 1.0);
    DiskSectionMoments result;
    if (b <= a || d <= c) {
        return result;
    }
    b = std::min(b, std::sqrt(std::max(0.0, 1.0 - c * c)));
    if (b <= a) {
        return result;
    }
    const auto split = std::sqrt(std::max(0.0, 1.0 - d * d));
    const auto fullHigh = std::min(b, split);
    if (fullHigh > a) {
        const auto width = fullHigh - a;
        result.area = width * (d - c);
        result.x = 0.5 * width * (fullHigh + a) * (d - c);
        result.y = 0.5 * width * (d - c) * (d + c);
    }
    const auto low = std::max(a, split);
    if (b > low) {
        const auto primitive = [](double x) {
            return 0.5 * (x * std::sqrt(std::max(0.0, 1.0 - x * x)) + std::asin(x));
        };
        const auto width = b - low;
        const auto area = primitive(b) - primitive(low) - c * width;
        const auto momentX = (std::pow(std::max(0.0, 1.0 - low * low), 1.5)
                                 - std::pow(std::max(0.0, 1.0 - b * b), 1.5))
                / 3.0
            - 0.5 * c * width * (b + low);
        const auto momentY
            = 0.5 * width * (1.0 - c * c - (b * b + b * low + low * low) / 3.0);
        // Roundoff can leave a vanishing sliver slightly negative.
        result.area += std::max(0.0, area);
        result.x += std::max(0.0, momentX);
        result.y += std::max(0.0, momentY);
    }
    return result;
}

void requireShape(const LeafChunk& chunk, double radius, double halfHeight)
{
    if (chunk.dimension() != 3) {
        throw AnalysisError("surface areas need a 3-D plotfile");
    }
    if (!std::isfinite(radius) || radius <= 0.0 || !std::isfinite(halfHeight)
        || halfHeight <= 0.0) {
        throw AnalysisError("surface radius and half height must be finite "
                            "and positive");
    }
}

} // namespace

DiskSectionMoments diskRectangleMoments(
    double radius, double x0, double x1, double y0, double y1)
{
    DiskSectionMoments result;
    if (radius <= 0.0 || x1 <= x0 || y1 <= y0) {
        return result;
    }
    // In units of the radius, so large cgs lengths do not cancel.
    for (const int sx : {-1, 1}) {
        const auto a = std::max(0.0, (sx > 0 ? x0 : -x1) / radius);
        const auto b = std::max(0.0, (sx > 0 ? x1 : -x0) / radius);
        for (const int sy : {-1, 1}) {
            const auto c = std::max(0.0, (sy > 0 ? y0 : -y1) / radius);
            const auto d = std::max(0.0, (sy > 0 ? y1 : -y0) / radius);
            const auto part = quadrantDiskMoments(a, b, c, d);
            result.area += part.area;
            result.x += sx * part.x;
            result.y += sy * part.y;
        }
    }
    const auto radius2 = radius * radius;
    result.area *= radius2;
    result.x *= radius2 * radius;
    result.y *= radius2 * radius;
    return result;
}

double surfaceRegionPadding(const Real3& coarseCellSize) noexcept
{
    return 0.5 * std::sqrt(coarseCellSize[0] * coarseCellSize[0]
                     + coarseCellSize[1] * coarseCellSize[1]
                     + coarseCellSize[2] * coarseCellSize[2]);
}

SphereSection sphereAreaInCells(
    const LeafChunk& chunk, const Real3& center, double radius)
{
    requireShape(chunk, radius, radius);
    const auto& origin = chunk.indexOrigin();
    const auto& dx = chunk.cellSize();
    const auto& i = chunk.index(0);
    const auto& j = chunk.index(1);
    const auto& k = chunk.index(2);
    const auto n = chunk.size();
    SphereSection section{std::vector<double>(n, 0.0),
        std::vector<double>(n, 0.0), std::vector<double>(n, 0.0),
        std::vector<double>(n, 0.0)};
    const auto radius2 = radius * radius;
    for (std::size_t cell = 0; cell < n; ++cell) {
        const auto x0 = origin[0] + static_cast<double>(i[cell]) * dx[0] - center[0];
        const auto y0 = origin[1] + static_cast<double>(j[cell]) * dx[1] - center[1];
        const auto z0 = origin[2] + static_cast<double>(k[cell]) * dx[2] - center[2];
        const auto x1 = x0 + dx[0];
        const auto y1 = y0 + dx[1];
        const auto z1 = z0 + dx[2];
        if (!sphereMayIntersect(radius2, x0, x1, y0, y1, z0, z1)) {
            continue;
        }
        const auto area = sphereSectionArea(radius, x0, x1, y0, y1, z0, z1);
        if (area <= 0.0) {
            continue;
        }
        const auto x = 0.5 * (x0 + x1);
        const auto y = 0.5 * (y0 + y1);
        const auto z = 0.5 * (z0 + z1);
        const auto r = std::sqrt(x * x + y * y + z * z);
        section.area[cell] = area;
        section.normalX[cell] = x / r;
        section.normalY[cell] = y / r;
        section.normalZ[cell] = z / r;
    }
    return section;
}

CylinderSection cylinderAreaInCells(const LeafChunk& chunk,
    const Real3& center, double radius, double halfHeight)
{
    requireShape(chunk, radius, halfHeight);
    const auto& origin = chunk.indexOrigin();
    const auto& dx = chunk.cellSize();
    const auto& i = chunk.index(0);
    const auto& j = chunk.index(1);
    const auto& k = chunk.index(2);
    const auto n = chunk.size();
    CylinderSection section;
    for (auto* column : {&section.wallArea, &section.wallNormalX,
             &section.wallNormalY, &section.upperCapArea, &section.lowerCapArea,
             &section.capMomentX, &section.capMomentY}) {
        column->assign(n, 0.0);
    }
    const auto radius2 = radius * radius;
    for (std::size_t cell = 0; cell < n; ++cell) {
        const auto x0 = origin[0] + static_cast<double>(i[cell]) * dx[0] - center[0];
        const auto y0 = origin[1] + static_cast<double>(j[cell]) * dx[1] - center[1];
        const auto x1 = x0 + dx[0];
        const auto y1 = y0 + dx[1];
        // Both z faces from the index, as Kangaroo takes them for cylinders.
        const auto z0 = origin[2] + static_cast<double>(k[cell]) * dx[2] - center[2];
        const auto z1
            = origin[2] + static_cast<double>(k[cell] + 1) * dx[2] - center[2];
        const auto minXY = minDistanceSquared(x0, x1) + minDistanceSquared(y0, y1);
        if (minXY > radius2) {
            continue;
        }
        const auto maxXY = maxDistanceSquared(x0, x1) + maxDistanceSquared(y0, y1);
        if (maxXY >= radius2
            && cylinderMayIntersect(radius2, halfHeight, x0, x1, y0, y1, z0, z1)) {
            const auto x = 0.5 * (x0 + x1);
            const auto y = 0.5 * (y0 + y1);
            const auto rxy = std::sqrt(x * x + y * y);
            if (rxy > 0.0) {
                const auto area
                    = cylinderWallArea(radius, halfHeight, x0, x1, y0, y1, z0, z1);
                if (area > 0.0) {
                    section.wallArea[cell] = area;
                    section.wallNormalX[cell] = x / rxy;
                    section.wallNormalY[cell] = y / rxy;
                }
            }
        }
        // A cap on a z face belongs to the cell on the cylinder's interior
        // side, so it is counted once.
        const bool upper = z0 < halfHeight && halfHeight <= z1;
        const bool lower = z0 <= -halfHeight && -halfHeight < z1;
        if (upper || lower) {
            const auto cap = diskRectangleMoments(radius, x0, x1, y0, y1);
            if (cap.area > 0.0) {
                section.upperCapArea[cell] = upper ? cap.area : 0.0;
                section.lowerCapArea[cell] = lower ? cap.area : 0.0;
                section.capMomentX[cell] = cap.x;
                section.capMomentY[cell] = cap.y;
            }
        }
    }
    return section;
}

} // namespace amrvis
