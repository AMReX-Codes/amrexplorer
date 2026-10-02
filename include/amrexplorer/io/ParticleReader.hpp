#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/StopToken.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
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
    // As the Header lists them: the positions are not among the reals.
    std::vector<std::string> realComponentNames;
    std::vector<std::string> intComponentNames;

    friend bool operator==(
        const ParticleSpeciesMetadata&, const ParticleSpeciesMetadata&)
        = default;
};

// One real or int component to read with the positions, by its index in the
// Header's list for that kind.
struct ParticleAttribute {
    enum class Kind : std::uint8_t { Real, Int };
    Kind kind = Kind::Real;
    int index = 0;

    friend bool operator==(const ParticleAttribute&, const ParticleAttribute&)
        = default;
};

struct ParticlePoint {
    // Complete AMReX idcpu: validity bit, persistent particle ID, and the
    // persistent CPU field. This is the stable sampling identity.
    std::uint64_t id = 0;
    Real3 position{};
    // The sample's attribute for this particle; zero when none was read.
    double value = 0.0;
};

struct ParticleReadMetrics {
    std::uint64_t integerBytesRead = 0;
    std::uint64_t realBytesRead = 0;
    std::uint64_t levelDirectoriesScanned = 0;
    std::uint64_t dataFilesOpened = 0;
};

struct ParticleSample {
    ParticleSpeciesMetadata species;
    // What each point's value holds, if anything.
    std::optional<ParticleAttribute> attribute;
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

// The component a species calls `name`, reals before ints; none when the
// species has no component of that name.
[[nodiscard]] std::optional<ParticleAttribute> findParticleAttribute(
    const ParticleSpeciesMetadata& species, const std::string& name);

[[nodiscard]] std::vector<ParticleSpeciesMetadata> discoverParticleSpecies(
    const std::filesystem::path& plotfile, StopToken cancellation = {});

// Selection is a stable hash of the complete AMReX idcpu. File order, grid,
// level, and current file ownership do not affect it; lower fractions are
// nested subsets of higher fractions for a fixed seed. An attribute outside
// the species' components is refused with std::invalid_argument.
[[nodiscard]] ParticleSample readParticleSample(
    const std::filesystem::path& plotfile, const std::string& species,
    double fraction, std::uint64_t seed = 0,
    StopToken cancellation = {},
    std::size_t maximumPoints = std::numeric_limits<std::size_t>::max(),
    std::optional<ParticleAttribute> attribute = std::nullopt);

} // namespace amrvis
