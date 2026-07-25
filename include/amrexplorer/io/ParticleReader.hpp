#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/StopToken.hpp>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace amrvis {

enum class ParticleRealPrecision : std::uint8_t {
    Single,
    Double
};

struct ParticleSpeciesMetadata {
    std::string name;
    int dimension = 0;
    int realComponentCount = 0;
    int intComponentCount = 0;
    std::uint64_t particleCount = 0;
    ParticleRealPrecision precision = ParticleRealPrecision::Double;
};

struct ParticlePoint {
    // Complete AMReX idcpu: validity bit, persistent particle ID, and the
    // persistent CPU field. This is the stable sampling identity.
    std::uint64_t id = 0;
    Real3 position{};
};

struct ParticleSample {
    ParticleSpeciesMetadata species;
    std::vector<ParticlePoint> points;
};

class ParticleReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] std::vector<ParticleSpeciesMetadata> discoverParticleSpecies(
    const std::filesystem::path& plotfile);

// Selection is a stable hash of the complete AMReX idcpu. File order, grid,
// level, and current file ownership do not affect it; lower fractions are
// nested subsets of higher fractions for a fixed seed.
[[nodiscard]] ParticleSample readParticleSample(
    const std::filesystem::path& plotfile, const std::string& species,
    double fraction, std::uint64_t seed = 0,
    StopToken cancellation = {});

} // namespace amrvis
