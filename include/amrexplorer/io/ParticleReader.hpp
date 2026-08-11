#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/StopToken.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace amrvis {

enum class ParticleRealPrecision : std::uint8_t {
    Single,
    Double
};

// Component counts a species header may declare. The local parser refuses
// anything outside this, so the wire has to as well: a negative count reaching
// the client would describe a species that cannot exist.
inline constexpr int maximumParticleComponents = 100'000;

struct ParticleSpeciesMetadata {
    std::string name;
    int dimension = 0;
    int realComponentCount = 0;
    int intComponentCount = 0;
    std::uint64_t particleCount = 0;
    ParticleRealPrecision precision = ParticleRealPrecision::Double;

    friend bool operator==(
        const ParticleSpeciesMetadata&, const ParticleSpeciesMetadata&)
        = default;
};

struct ParticlePoint {
    // Complete AMReX idcpu: validity bit, persistent particle ID, and the
    // persistent CPU field. This is the stable sampling identity.
    std::uint64_t id = 0;
    Real3 position{};
};

struct ParticleReadMetrics {
    std::uint64_t integerBytesRead = 0;
    std::uint64_t realBytesRead = 0;
    std::uint64_t levelDirectoriesScanned = 0;
    std::uint64_t dataFilesOpened = 0;
};

struct ParticleSample {
    ParticleSpeciesMetadata species;
    std::vector<ParticlePoint> points;
    ParticleReadMetrics io;
};

class ParticleReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ParticleSampleLimitExceeded : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] std::vector<ParticleSpeciesMetadata> discoverParticleSpecies(
    const std::filesystem::path& plotfile, StopToken cancellation = {});

// Selection is a stable hash of the complete AMReX idcpu. File order, grid,
// level, and current file ownership do not affect it; lower fractions are
// nested subsets of higher fractions for a fixed seed.
[[nodiscard]] ParticleSample readParticleSample(
    const std::filesystem::path& plotfile, const std::string& species,
    double fraction, std::uint64_t seed = 0,
    StopToken cancellation = {},
    std::size_t maximumPoints = std::numeric_limits<std::size_t>::max());

} // namespace amrvis
