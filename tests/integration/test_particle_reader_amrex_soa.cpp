// Reads particles AMReX itself wrote from a pure-SoA container, so the reader's
// layout is checked against the real writer rather than a fixture of our own.
//
// tests/data/plotfile_3d_soa_particles is the output of AMReX
// Tests/Particles/CheckpointRestartSOA (AMReX 26.09-185-g19d64c697f, with the
// pure-SoA InitRandom int loop fixed to start at 0, AMReX-Codes/amrex#5698),
// built with DIM=3 and run with
//   ncells=4 max_grid_size=2 nppc=1 ncomp=2
// ParticleContainerPureSoA<12, 4>: 64 particles over 8 grids, positions
// uniform in [0, 1), the 9 other reals set to 4, 6, 7, ..., 13 and the 4 ints
// to 5, 14, 15, 16.

#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/io/ParticleReader.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <stdexcept>
#include <unordered_set>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        require(argc == 2, "plotfile fixture path is required");
        const std::filesystem::path plotfile(argv[1]);

        const auto species = amrvis::discoverParticleSpecies(plotfile);
        require(species.size() == 1 && species.front().name == "particle0",
            "the AMReX particle species was not discovered");
        const auto& metadata = species.front();
        require(metadata.dimension == 3, "wrong particle dimension");
        // The Header lists the reals after the positions: a pure-SoA
        // container's first three reals are the positions, not attributes.
        require(metadata.realComponentCount == 9,
            "the positions were counted as real components");
        require(metadata.intComponentCount == 4, "wrong int component count");
        require(metadata.particleCount == 64, "wrong particle count");
        require(metadata.precision == amrvis::ParticleRealPrecision::Double,
            "wrong particle precision");

        const auto sample = amrvis::readParticleSample(plotfile, "particle0", 1.0);
        require(sample.points.size() == 64, "the full sample omitted particles");
        std::unordered_set<std::uint64_t> ids;
        for (const auto& point : sample.points) {
            // A misread record lands on the attributes (4 and up) or the ids.
            for (const auto coordinate : point.position.values) {
                require(coordinate >= 0.0 && coordinate < 1.0,
                    "a particle position was read from the wrong offset");
            }
            require(ids.insert(point.id).second, "two particles share an idcpu");
        }

        require(metadata.realComponentNames.size() == 9
                && metadata.realComponentNames.front() == "particle_real_component_0"
                && metadata.realComponentNames.back() == "particle_real_component_8",
            "the real component names were not kept");
        require(metadata.intComponentNames.size() == 4
                && metadata.intComponentNames.back() == "particle_int_component_3",
            "the int component names were not kept");

        // Each attribute is constant, so every point must carry exactly it,
        // at the same position the plain read gave.
        using Kind = amrvis::ParticleAttribute::Kind;
        const auto requireAttribute = [&](Kind kind, int index, double expected) {
            const auto withValues = amrvis::readParticleSample(
                plotfile, "particle0", 1.0, 0, {},
                std::numeric_limits<std::size_t>::max(),
                amrvis::ParticleAttribute{kind, index});
            require(withValues.attribute == amrvis::ParticleAttribute{kind, index},
                "the sample does not name the attribute it read");
            require(withValues.points.size() == sample.points.size(),
                "reading an attribute changed the sample");
            for (std::size_t i = 0; i < withValues.points.size(); ++i) {
                require(withValues.points[i].id == sample.points[i].id
                        && withValues.points[i].position == sample.points[i].position,
                    "reading an attribute moved a particle");
                require(withValues.points[i].value == expected,
                    "a particle attribute was read from the wrong offset");
            }
        };
        requireAttribute(Kind::Real, 0, 4.0);
        requireAttribute(Kind::Real, 8, 13.0);
        requireAttribute(Kind::Int, 0, 5.0);
        requireAttribute(Kind::Int, 1, 14.0);
        requireAttribute(Kind::Int, 3, 16.0);

        for (const auto& outside : {amrvis::ParticleAttribute{Kind::Real, 9},
                 amrvis::ParticleAttribute{Kind::Int, 4},
                 amrvis::ParticleAttribute{Kind::Real, -1}}) {
            bool refused = false;
            try {
                static_cast<void>(amrvis::readParticleSample(plotfile,
                    "particle0", 1.0, 0, {},
                    std::numeric_limits<std::size_t>::max(), outside));
            } catch (const std::invalid_argument&) {
                refused = true;
            }
            require(refused, "an attribute outside the components was read");
        }

        // By name: reals before ints, nothing for a name the species lacks.
        require(amrvis::findParticleAttribute(metadata, "particle_real_component_2")
                    == amrvis::ParticleAttribute{Kind::Real, 2}
                && amrvis::findParticleAttribute(metadata, "particle_int_component_1")
                    == amrvis::ParticleAttribute{Kind::Int, 1}
                && !amrvis::findParticleAttribute(metadata, "mass"),
            "an attribute name resolved to the wrong component");

        // Through a session, as the overlay loads them.
        amrvis::LocalDatasetSession session(plotfile, amrvis::DatasetId{1}, 1U << 20U);
        const std::vector<std::string> selected{"particle0"};
        const auto requireLoaded = [&](const std::string& name,
                                       std::optional<amrvis::ParticleAttribute> expected,
                                       double value) {
            const auto samples = amrvis::loadParticleSamples(
                session, selected, 1.0, 0, {}, name);
            require(samples.size() == 1 && samples.front().attribute == expected
                    && samples.front().points.size() == 64,
                "a loaded sample does not carry the named attribute");
            for (const auto& point : samples.front().points) {
                require(point.value == value, "a loaded particle has the wrong value");
            }
        };
        requireLoaded("particle_real_component_2",
            amrvis::ParticleAttribute{Kind::Real, 2}, 7.0);
        requireLoaded("particle_int_component_1",
            amrvis::ParticleAttribute{Kind::Int, 1}, 14.0);
        requireLoaded("mass", std::nullopt, 0.0);
        requireLoaded("", std::nullopt, 0.0);

        bool sessionRefused = false;
        try {
            static_cast<void>(session.requestParticleSample(
                "particle0", 1.0, 0, {}, amrvis::ParticleAttribute{Kind::Int, 4}));
        } catch (const std::invalid_argument&) {
            sessionRefused = true;
        }
        require(sessionRefused, "the session read an attribute outside the components");
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "AMReX pure-SoA particle reader test passed\n";
    return 0;
}
