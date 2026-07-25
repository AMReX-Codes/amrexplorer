#include <amrexplorer/io/ParticleReader.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>

namespace amrvis {
namespace {

constexpr int maximumComponents = 100'000;
constexpr int maximumLevels = 1'000;
constexpr int maximumGridsPerLevel = 10'000'000;

struct GridRecord {
    int level = 0;
    int fileNumber = 0;
    std::uint64_t count = 0;
    std::uint64_t offset = 0;
};

struct ParsedHeader {
    ParticleSpeciesMetadata metadata;
    bool expandedIds = false;
    int finestLevel = 0;
    std::vector<GridRecord> grids;
};

template <typename T>
T readRequired(std::istream& input, std::string_view description)
{
    T value{};
    if (!(input >> value)) {
        throw ParticleReadError(
            "malformed particle Header while reading " + std::string(description));
    }
    return value;
}

std::uint64_t checkedProduct(
    std::uint64_t lhs, std::uint64_t rhs, std::string_view description)
{
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw ParticleReadError(
            "particle " + std::string(description) + " exceeds supported size");
    }
    return lhs * rhs;
}

ParsedHeader parseHeader(
    const std::filesystem::path& path, const std::string& species)
{
    std::ifstream input(path);
    if (!input) {
        throw ParticleReadError(
            "cannot open particle Header '" + path.string() + "'");
    }

    ParsedHeader result;
    result.metadata.name = species;
    const auto version = readRequired<std::string>(input, "version");
    if (version.find("Version_One_Dot_Zero") == std::string::npos
        && version.find("Version_One_Dot_One") == std::string::npos
        && version.find("Version_Two_Dot_Zero") == std::string::npos
        && version.find("Version_Two_Dot_One") == std::string::npos) {
        throw ParticleReadError("unsupported AMReX particle version '" + version + "'");
    }
    result.expandedIds
        = version.find("Version_Two_Dot_One") != std::string::npos;
    if (version.find("Version_One_Dot_Zero") != std::string::npos
        || version.find("_double") != std::string::npos) {
        result.metadata.precision = ParticleRealPrecision::Double;
    } else if (version.find("_single") != std::string::npos) {
        result.metadata.precision = ParticleRealPrecision::Single;
    } else {
        throw ParticleReadError(
            "particle version does not specify single or double precision");
    }

    result.metadata.dimension = readRequired<int>(input, "dimension");
    if (result.metadata.dimension < 1 || result.metadata.dimension > 3) {
        throw ParticleReadError("particle dimension must be between 1 and 3");
    }
    result.metadata.realComponentCount
        = readRequired<int>(input, "real component count");
    if (result.metadata.realComponentCount < 0
        || result.metadata.realComponentCount > maximumComponents) {
        throw ParticleReadError("particle real component count is outside supported bounds");
    }
    for (int i = 0; i < result.metadata.realComponentCount; ++i) {
        (void)readRequired<std::string>(input, "real component name");
    }
    result.metadata.intComponentCount
        = readRequired<int>(input, "integer component count");
    if (result.metadata.intComponentCount < 0
        || result.metadata.intComponentCount > maximumComponents) {
        throw ParticleReadError(
            "particle integer component count is outside supported bounds");
    }
    for (int i = 0; i < result.metadata.intComponentCount; ++i) {
        (void)readRequired<std::string>(input, "integer component name");
    }
    (void)readRequired<int>(input, "checkpoint flag");
    result.metadata.particleCount
        = readRequired<std::uint64_t>(input, "particle count");
    (void)readRequired<std::uint64_t>(input, "next particle id");
    result.finestLevel = readRequired<int>(input, "finest particle level");
    if (result.finestLevel < 0 || result.finestLevel >= maximumLevels) {
        throw ParticleReadError("particle finest level is outside supported bounds");
    }

    std::vector<int> gridCounts(
        static_cast<std::size_t>(result.finestLevel + 1));
    std::uint64_t totalGrids = 0;
    for (auto& count : gridCounts) {
        count = readRequired<int>(input, "level grid count");
        if (count <= 0 || count > maximumGridsPerLevel) {
            throw ParticleReadError("particle grid count is outside supported bounds");
        }
        totalGrids += static_cast<std::uint64_t>(count);
    }
    result.grids.reserve(static_cast<std::size_t>(totalGrids));
    std::uint64_t recordedParticles = 0;
    for (int level = 0; level <= result.finestLevel; ++level) {
        for (int grid = 0; grid < gridCounts[static_cast<std::size_t>(level)]; ++grid) {
            GridRecord record;
            record.level = level;
            record.fileNumber = readRequired<int>(input, "particle data file number");
            record.count = readRequired<std::uint64_t>(input, "grid particle count");
            record.offset = readRequired<std::uint64_t>(input, "grid data offset");
            if (record.fileNumber < 0) {
                throw ParticleReadError("particle data file number must be nonnegative");
            }
            recordedParticles += record.count;
            result.grids.push_back(record);
        }
    }
    if (recordedParticles != result.metadata.particleCount) {
        throw ParticleReadError(
            "particle grid counts do not match the Header particle count");
    }
    return result;
}

std::uint64_t splitmix64(std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

bool selected(std::uint64_t id, double fraction, std::uint64_t seed) noexcept
{
    if (fraction <= 0.0) {
        return false;
    }
    if (fraction >= 1.0) {
        return true;
    }
    const auto threshold = static_cast<long double>(fraction)
        * static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
    return static_cast<long double>(splitmix64(id ^ seed)) <= threshold;
}

std::optional<std::uint64_t> decodeIdCpu(
    std::int32_t first, std::int32_t second, bool expanded) noexcept
{
    constexpr auto validBit = std::uint64_t{1} << 63U;
    constexpr auto cpuMask = (std::uint64_t{1} << 24U) - 1U;
    if (!expanded) {
        if (first <= 0) {
            return std::nullopt;
        }
        return validBit | (static_cast<std::uint64_t>(first) << 24U)
            | (static_cast<std::uint64_t>(
                   std::bit_cast<std::uint32_t>(second))
                & cpuMask);
    }
    const auto high = static_cast<std::uint64_t>(
        std::bit_cast<std::uint32_t>(first));
    const auto low = static_cast<std::uint64_t>(
        std::bit_cast<std::uint32_t>(second));
    const auto packed = (high << 32U) | low;
    if ((packed >> 63U) == 0) {
        return std::nullopt;
    }
    return packed;
}

std::filesystem::path particleDataPath(
    const std::filesystem::path& speciesPath, int level, int fileNumber)
{
    const auto levelPath = speciesPath / ("Level_" + std::to_string(level));
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(levelPath, error)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        constexpr std::string_view prefix = "DATA_";
        if (!name.starts_with(prefix)) {
            continue;
        }
        try {
            std::size_t consumed = 0;
            const auto number = std::stoi(name.substr(prefix.size()), &consumed);
            if (consumed == name.size() - prefix.size() && number == fileNumber) {
                return entry.path();
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    throw ParticleReadError("cannot find particle DATA file "
        + std::to_string(fileNumber) + " in '" + levelPath.string() + "'");
}

template <typename Real>
void readGrid(const std::filesystem::path& dataPath, const GridRecord& grid,
    const ParsedHeader& header, double fraction, std::uint64_t seed,
    StopToken cancellation, std::vector<ParticlePoint>& output)
{
    std::ifstream input(dataPath, std::ios::binary);
    if (!input) {
        throw ParticleReadError(
            "cannot open particle data file '" + dataPath.string() + "'");
    }
    input.seekg(static_cast<std::streamoff>(grid.offset));
    if (!input) {
        throw ParticleReadError("cannot seek in particle data file");
    }

    const auto intValues = static_cast<std::uint64_t>(
        2 + header.metadata.intComponentCount);
    const auto intRecordBytes = checkedProduct(
        intValues, sizeof(std::int32_t), "integer record");
    const auto realValues = static_cast<std::uint64_t>(
        header.metadata.dimension + header.metadata.realComponentCount);
    const auto realRecordBytes = checkedProduct(
        realValues, sizeof(Real), "real record");

    std::vector<std::uint8_t> keep(static_cast<std::size_t>(grid.count), 0);
    std::vector<std::uint64_t> ids(static_cast<std::size_t>(grid.count), 0);
    std::array<std::int32_t, 2> idWords{};
    for (std::uint64_t index = 0; index < grid.count; ++index) {
        if (cancellation.stop_requested()) {
            throw ParticleReadError("particle read cancelled");
        }
        input.read(reinterpret_cast<char*>(idWords.data()),
            static_cast<std::streamsize>(sizeof(idWords)));
        if (!input) {
            throw ParticleReadError("truncated particle integer data");
        }
        const auto idcpu = decodeIdCpu(
            idWords[0], idWords[1], header.expandedIds);
        if (idcpu.has_value() && selected(*idcpu, fraction, seed)) {
            keep[static_cast<std::size_t>(index)] = 1;
            ids[static_cast<std::size_t>(index)] = *idcpu;
        }
        input.seekg(static_cast<std::streamoff>(
            intRecordBytes - sizeof(idWords)), std::ios::cur);
        if (!input) {
            throw ParticleReadError("truncated particle integer data");
        }
    }

    std::array<Real, 3> position{};
    for (std::uint64_t index = 0; index < grid.count; ++index) {
        if (cancellation.stop_requested()) {
            throw ParticleReadError("particle read cancelled");
        }
        if (keep[static_cast<std::size_t>(index)] != 0) {
            input.read(reinterpret_cast<char*>(position.data()),
                static_cast<std::streamsize>(
                    static_cast<std::uint64_t>(header.metadata.dimension)
                    * sizeof(Real)));
            if (!input) {
                throw ParticleReadError("truncated particle real data");
            }
            ParticlePoint point;
            point.id = ids[static_cast<std::size_t>(index)];
            for (int axis = 0; axis < header.metadata.dimension; ++axis) {
                point.position[static_cast<std::size_t>(axis)]
                    = static_cast<double>(position[static_cast<std::size_t>(axis)]);
            }
            output.push_back(point);
            input.seekg(static_cast<std::streamoff>(
                static_cast<std::uint64_t>(header.metadata.realComponentCount)
                * sizeof(Real)), std::ios::cur);
        } else {
            input.seekg(static_cast<std::streamoff>(realRecordBytes), std::ios::cur);
        }
        if (!input) {
            throw ParticleReadError("truncated particle real data");
        }
    }
}

} // namespace

std::vector<ParticleSpeciesMetadata> discoverParticleSpecies(
    const std::filesystem::path& plotfile)
{
    std::vector<ParticleSpeciesMetadata> result;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(plotfile, error)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto headerPath = entry.path() / "Header";
        std::ifstream probe(headerPath);
        std::string version;
        if (!(probe >> version) || !version.starts_with("Version_")) {
            continue;
        }
        result.push_back(parseHeader(
            headerPath, entry.path().filename().string()).metadata);
    }
    std::ranges::sort(result, {}, &ParticleSpeciesMetadata::name);
    return result;
}

ParticleSample readParticleSample(
    const std::filesystem::path& plotfile, const std::string& species,
    double fraction, std::uint64_t seed, StopToken cancellation)
{
    if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) {
        throw std::invalid_argument("particle sample fraction must be between 0 and 1");
    }
    const auto speciesPath = plotfile / species;
    const auto header = parseHeader(speciesPath / "Header", species);
    ParticleSample result;
    result.species = header.metadata;
    if (fraction == 0.0 || header.metadata.particleCount == 0) {
        return result;
    }
    const auto expected = static_cast<long double>(header.metadata.particleCount)
        * static_cast<long double>(fraction);
    result.points.reserve(static_cast<std::size_t>(std::min<long double>(
        expected + 16.0L,
        static_cast<long double>(std::numeric_limits<std::size_t>::max()))));
    for (const auto& grid : header.grids) {
        if (grid.count == 0) {
            continue;
        }
        const auto dataPath = particleDataPath(
            speciesPath, grid.level, grid.fileNumber);
        if (header.metadata.precision == ParticleRealPrecision::Single) {
            readGrid<float>(dataPath, grid, header, fraction, seed,
                cancellation, result.points);
        } else {
            readGrid<double>(dataPath, grid, header, fraction, seed,
                cancellation, result.points);
        }
    }
    return result;
}

} // namespace amrvis
