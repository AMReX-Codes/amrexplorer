#pragma once

// openPMD series the openPMD tests write for themselves with openPMD-api's
// JSON backend, which every openPMD-api build has, and the functions their
// values follow. Every field is linear in position, so a staggered component
// interpolated to the cell centres reproduces the field there exactly (to
// rounding).

#include <openPMD/openPMD.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace amrvis::test::openpmd {

namespace io = ::openPMD;

inline double rhoAt(double x, double y, double z)
{
    return 1.0 + 2.0 * x + 3.0 * y + 5.0 * z;
}

inline double eAt(int component, double x, double y, double z)
{
    return 7.0 * (component + 1) - x + 4.0 * y + 0.5 * z;
}

// The 3-D series: level 0 is 8^3 cells of size 1 from (-4, 0, 10); level 1
// (WarpX's _lvl1 meshes) is the same domain at half the spacing, written only
// over cells [4, 11]^3. rho is cell-centred and stored at half its value with
// unitSI 2; E's components each sit on the lower face along their own axis.
// Arrays are (z, y, x), x fastest, as WarpX writes them.
constexpr std::array<double, 3> origin{-4.0, 0.0, 10.0};

inline void writeMesh(io::Iteration& iteration, const std::string& name,
    double spacing, int cells, int lower, int upper, bool vector)
{
    auto mesh = iteration.meshes[name];
    mesh.setGeometry(io::Mesh::Geometry::cartesian);
    mesh.setDataOrder(io::Mesh::DataOrder::C);
    mesh.setAxisLabels({"z", "y", "x"});
    mesh.setGridSpacing(std::vector<double>{spacing, spacing, spacing});
    mesh.setGridGlobalOffset({origin[2], origin[1], origin[0]});
    mesh.setGridUnitSI(1.0);
    const std::vector<std::string> keys = vector
        ? std::vector<std::string>{"x", "y", "z"}
        : std::vector<std::string>{io::RecordComponent::SCALAR};
    for (std::size_t c = 0; c < keys.size(); ++c) {
        auto rc = mesh[keys[c]];
        // By array dimension (z, y, x): which axis this component is
        // staggered along, if any.
        std::array<double, 3> position{0.5, 0.5, 0.5};
        std::array<std::uint64_t, 3> extent{static_cast<std::uint64_t>(cells),
            static_cast<std::uint64_t>(cells), static_cast<std::uint64_t>(cells)};
        std::array<int, 3> last{upper, upper, upper};
        if (vector) {
            const auto dim = 2 - c;
            position[dim] = 0.0;
            extent[dim] += 1;
            last[dim] += 1;
        }
        rc.setPosition(std::vector<double>(position.begin(), position.end()));
        rc.setUnitSI(vector ? 1.0 : 2.0);
        rc.resetDataset({io::Datatype::DOUBLE, {extent[0], extent[1], extent[2]}});
        std::vector<double> values;
        for (int k = lower; k <= last[0]; ++k) {
            for (int j = lower; j <= last[1]; ++j) {
                for (int i = lower; i <= last[2]; ++i) {
                    const double x = origin[0] + (i + position[2]) * spacing;
                    const double y = origin[1] + (j + position[1]) * spacing;
                    const double z = origin[2] + (k + position[0]) * spacing;
                    values.push_back(vector ? eAt(static_cast<int>(c), x, y, z)
                                            : 0.5 * rhoAt(x, y, z));
                }
            }
        }
        const auto n = [&](std::size_t dim) {
            return static_cast<std::uint64_t>(last[dim] - lower + 1);
        };
        rc.storeChunk(values,
            {static_cast<std::uint64_t>(lower), static_cast<std::uint64_t>(lower),
                static_cast<std::uint64_t>(lower)},
            {n(0), n(1), n(2)});
        // storeChunk keeps only a view of `values` until the next flush.
        iteration.seriesFlush();
    }
}

inline void writeAmr(const std::filesystem::path& file)
{
    io::Series series(file.string(), io::Access::CREATE);
    auto iteration = series.iterations[100];
    iteration.setTime(2.0);
    iteration.setTimeUnitSI(0.5);
    writeMesh(iteration, "rho", 1.0, 8, 0, 7, false);
    writeMesh(iteration, "E", 1.0, 8, 0, 7, true);
    writeMesh(iteration, "rho_lvl1", 0.5, 16, 4, 11, false);
    writeMesh(iteration, "E_lvl1", 0.5, 16, 4, 11, true);
    series.flush();
}

// 2-D, single precision, labelled (x, z) with x the *slowest* array
// dimension -- the opposite of AMReX's layout, so the reader must transpose.
// x spacing 1 from 0, z spacing 2 from 100.
inline double phiAt(double x, double z)
{
    return x + 1000.0 * z;
}

inline void writeTransposed(const std::filesystem::path& file)
{
    io::Series series(file.string(), io::Access::CREATE);
    auto iteration = series.iterations[0];
    auto mesh = iteration.meshes["phi"];
    mesh.setGeometry(io::Mesh::Geometry::cartesian);
    mesh.setAxisLabels({"x", "z"});
    mesh.setGridSpacing(std::vector<double>{1.0, 2.0});
    mesh.setGridGlobalOffset({0.0, 100.0});
    auto rc = mesh[io::RecordComponent::SCALAR];
    rc.setPosition(std::vector<double>{0.5, 0.5});
    rc.resetDataset({io::Datatype::FLOAT, {4, 6}});
    std::vector<float> values;
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 6; ++k) {
            values.push_back(static_cast<float>(phiAt(i + 0.5, 100.0 + 2.0 * (k + 0.5))));
        }
    }
    rc.storeChunk(values, {0, 0}, {4, 6});
    series.flush();
}

// A series of three iterations (10, 20, 30) of one cell-centred mesh "rho"
// on 4^3 cells of size 1 from the origin, whose values follow
// seriesRhoAt(iteration, ...), and a species "electrons" of seriesParticles
// particles: position + positionOffset (an integer cell) for the location,
// a non-contiguous uint64 id, a "momentum" vector stored at twice its value
// with unitSI 0.5, a "weighting" scalar and an int32 "level".
constexpr std::uint64_t seriesParticles = 200;
constexpr std::array<std::uint64_t, 3> seriesIterations{10, 20, 30};

inline double seriesRhoAt(std::uint64_t iteration, double x, double y, double z)
{
    return static_cast<double>(iteration) + x + 10.0 * y + 100.0 * z;
}

inline std::uint64_t seriesParticleId(std::uint64_t p)
{
    return 1000 + 7 * p;
}

// Particle p's physical position along `axis` at `iteration`.
inline double seriesParticlePosition(
    std::uint64_t iteration, std::uint64_t p, int axis)
{
    const auto a = static_cast<std::uint64_t>(axis);
    const auto cell = (p + a) % 4;
    const auto fraction = static_cast<double>((p * 37 + iteration + a * 11) % 100)
        / 100.0;
    return static_cast<double>(cell) + fraction;
}

inline double seriesMomentum(std::uint64_t p, int component)
{
    return static_cast<double>(p) * (component + 1);
}

inline double seriesWeighting(std::uint64_t p)
{
    return static_cast<double>(p) + 0.25;
}

inline std::int32_t seriesLevel(std::uint64_t p)
{
    return static_cast<std::int32_t>(p % 3);
}

template <typename T>
void storeParticles(io::Iteration& iteration, io::RecordComponent rc,
    std::vector<T> values, double unitSI = 1.0)
{
    rc.resetDataset({io::determineDatatype<T>(), {values.size()}});
    rc.setUnitSI(unitSI);
    rc.storeChunk(values, {0}, {values.size()});
    // storeChunk keeps only a view of `values` until the next flush.
    iteration.seriesFlush();
}

inline void writeSeries(const std::filesystem::path& file)
{
    io::Series series(file.string(), io::Access::CREATE);
    for (const auto index : seriesIterations) {
        auto iteration = series.iterations[index];
        iteration.setTime(0.1 * static_cast<double>(index));
        auto mesh = iteration.meshes["rho"];
        mesh.setAxisLabels({"z", "y", "x"});
        mesh.setGridSpacing(std::vector<double>{1.0, 1.0, 1.0});
        mesh.setGridGlobalOffset({0.0, 0.0, 0.0});
        auto rc = mesh[io::RecordComponent::SCALAR];
        rc.setPosition(std::vector<double>{0.5, 0.5, 0.5});
        rc.resetDataset({io::Datatype::DOUBLE, {4, 4, 4}});
        std::vector<double> values;
        for (int k = 0; k < 4; ++k) {
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 4; ++i) {
                    values.push_back(seriesRhoAt(index, i + 0.5, j + 0.5, k + 0.5));
                }
            }
        }
        rc.storeChunk(values, {0, 0, 0}, {4, 4, 4});
        iteration.seriesFlush();

        auto electrons = iteration.particles["electrons"];
        const std::array<std::string, 3> axes{"x", "y", "z"};
        for (int axis = 0; axis < 3; ++axis) {
            std::vector<double> fraction;
            std::vector<std::int32_t> cell;
            for (std::uint64_t p = 0; p < seriesParticles; ++p) {
                const auto at = seriesParticlePosition(index, p, axis);
                cell.push_back(static_cast<std::int32_t>(at));
                fraction.push_back(at - static_cast<double>(cell.back()));
            }
            const auto key = axes[static_cast<std::size_t>(axis)];
            storeParticles(iteration, electrons["position"][key], fraction);
            storeParticles(iteration, electrons["positionOffset"][key], cell);
            std::vector<double> momentum;
            for (std::uint64_t p = 0; p < seriesParticles; ++p) {
                momentum.push_back(2.0 * seriesMomentum(p, axis));
            }
            storeParticles(iteration, electrons["momentum"][key], momentum, 0.5);
        }
        std::vector<std::uint64_t> ids;
        std::vector<double> weighting;
        std::vector<std::int32_t> level;
        for (std::uint64_t p = 0; p < seriesParticles; ++p) {
            ids.push_back(seriesParticleId(p));
            weighting.push_back(seriesWeighting(p));
            level.push_back(seriesLevel(p));
        }
        storeParticles(iteration, electrons["id"][io::RecordComponent::SCALAR], ids);
        storeParticles(iteration,
            electrons["weighting"][io::RecordComponent::SCALAR], weighting);
        storeParticles(iteration, electrons["level"][io::RecordComponent::SCALAR], level);
    }
    series.flush();
}

} // namespace amrvis::test::openpmd
