#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/detail/VisMfIndex.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace amrvis {
namespace {

constexpr int maximumComponents = 100'000;
constexpr int maximumLevels = 1'000;
constexpr int maximumGridsPerLevel = 10'000'000;

template <typename T>
T readRequired(std::istream& input, std::string_view description)
{
    T value{};
    if (!(input >> value)) {
        throw MetadataReadError("malformed plotfile Header while reading "
            + std::string(description));
    }
    return value;
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string readNonEmptyLine(std::istream& input, std::string_view description)
{
    std::string line;
    while (std::getline(input, line)) {
        line = trim(std::move(line));
        if (!line.empty()) {
            return line;
        }
    }
    throw MetadataReadError("malformed plotfile Header while reading "
        + std::string(description));
}

void expectCharacter(std::istream& input, char expected, std::string_view description)
{
    input >> std::ws;
    char actual = '\0';
    if (!input.get(actual) || actual != expected) {
        throw MetadataReadError("malformed AMReX Box while reading "
            + std::string(description));
    }
}

Int3 readIntTuple(std::istream& input, int dimension, std::string_view description)
{
    expectCharacter(input, '(', description);
    Int3 tuple;
    for (int axis = 0; axis < dimension; ++axis) {
        tuple[static_cast<std::size_t>(axis)] = readRequired<int>(input, description);
        if (axis + 1 < dimension) {
            expectCharacter(input, ',', description);
        }
    }
    expectCharacter(input, ')', description);
    return tuple;
}

IntBox readAmrexBox(std::istream& input, int dimension, std::string_view description)
{
    expectCharacter(input, '(', description);
    IntBox box;
    box.lower = readIntTuple(input, dimension, description);
    box.upper = readIntTuple(input, dimension, description);
    box.centering = readIntTuple(input, dimension, description);
    expectCharacter(input, ')', description);
    return box;
}

std::vector<int> parseIntegers(const std::string& line)
{
    std::string numbers = line;
    std::replace_if(numbers.begin(), numbers.end(), [](char character) {
        return !(character >= '0' && character <= '9')
            && character != '-' && character != '+';
    }, ' ');
    std::istringstream input(numbers);
    std::vector<int> values;
    int value = 0;
    while (input >> value) {
        values.push_back(value);
    }
    return values;
}

std::vector<std::vector<double>> readRealMatrix(
    std::istream& input, std::string_view description)
{
    const auto rows = readRequired<std::uint64_t>(input, description);
    char comma = '\0';
    if (!(input >> comma) || comma != ',') {
        throw MetadataReadError("malformed VisMF matrix dimensions");
    }
    const auto columns = readRequired<std::uint64_t>(input, description);
    if (rows > static_cast<std::uint64_t>(maximumGridsPerLevel)
        || columns > static_cast<std::uint64_t>(maximumComponents)) {
        throw MetadataReadError("VisMF matrix dimensions are outside supported bounds");
    }

    std::vector<std::vector<double>> matrix(
        static_cast<std::size_t>(rows),
        std::vector<double>(static_cast<std::size_t>(columns)));
    for (auto& row : matrix) {
        for (auto& value : row) {
            value = readRequired<double>(input, description);
            if (!(input >> comma) || comma != ',') {
                throw MetadataReadError("malformed comma-separated VisMF matrix");
            }
        }
    }
    return matrix;
}

} // namespace

detail::VisMfIndex detail::readVisMfIndex(
    const std::filesystem::path& headerPath, int dimension)
{
    std::error_code sizeError;
    const auto headerSize = std::filesystem::file_size(headerPath, sizeError);
    if (sizeError) {
        throw MetadataReadError("cannot stat VisMF Header '" + headerPath.string()
            + "': " + sizeError.message());
    }
    std::ifstream input(headerPath, std::ios::binary);
    if (!input) {
        throw MetadataReadError("cannot open VisMF Header '" + headerPath.string() + "'");
    }

    VisMfIndex index;
    index.bytesRead = headerSize;
    index.version = readRequired<int>(input, "VisMF header version");
    if (index.version < 1 || index.version > 4) {
        throw MetadataReadError("unsupported VisMF header version");
    }
    [[maybe_unused]] const auto fileLayout = readRequired<int>(input, "VisMF file layout");
    index.components = readRequired<int>(input, "VisMF component count");
    if (index.components < 0 || index.components > maximumComponents) {
        throw MetadataReadError("VisMF component count is outside supported bounds");
    }
    input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    const auto ghostValues = parseIntegers(readNonEmptyLine(input, "VisMF ghost width"));
    if (ghostValues.size() == 1) {
        index.ghostWidth = {{ghostValues[0], ghostValues[0], ghostValues[0]}};
    } else if (ghostValues.size() >= static_cast<std::size_t>(dimension)) {
        for (int axis = 0; axis < dimension; ++axis) {
            index.ghostWidth[static_cast<std::size_t>(axis)] =
                ghostValues[static_cast<std::size_t>(axis)];
        }
    } else {
        throw MetadataReadError("malformed VisMF ghost width");
    }

    const auto boxArrayHeader = parseIntegers(
        readNonEmptyLine(input, "VisMF BoxArray header"));
    if (boxArrayHeader.empty() || boxArrayHeader.front() < 0
        || boxArrayHeader.front() > maximumGridsPerLevel) {
        throw MetadataReadError("VisMF BoxArray size is outside supported bounds");
    }
    const auto boxCount = static_cast<std::size_t>(boxArrayHeader.front());
    index.boxes.reserve(boxCount);
    for (std::size_t box = 0; box < boxCount; ++box) {
        index.boxes.push_back(readAmrexBox(input, dimension, "VisMF BoxArray entry"));
    }
    if (readNonEmptyLine(input, "VisMF BoxArray terminator") != ")") {
        throw MetadataReadError("malformed VisMF BoxArray terminator");
    }

    const auto locationCount = readRequired<std::uint64_t>(input, "VisMF location count");
    if (locationCount != boxCount) {
        throw MetadataReadError("VisMF location count does not match BoxArray size");
    }
    index.fileNames.reserve(boxCount);
    index.fileOffsets.reserve(boxCount);
    for (std::size_t block = 0; block < boxCount; ++block) {
        const auto prefix = readRequired<std::string>(input, "FabOnDisk prefix");
        if (prefix != "FabOnDisk:") {
            throw MetadataReadError("malformed FabOnDisk record");
        }
        index.fileNames.push_back(readRequired<std::string>(input, "FAB data filename"));
        index.fileOffsets.push_back(readRequired<std::uint64_t>(input, "FAB data offset"));
    }

    if (index.version == 1 || index.version == 3) {
        index.minimum = readRealMatrix(input, "per-block minima");
        index.maximum = readRealMatrix(input, "per-block maxima");
        index.hasPerBlockStatistics = true;
        if (index.minimum.size() != boxCount || index.maximum.size() != boxCount) {
            throw MetadataReadError("VisMF statistics do not match BoxArray size");
        }
    } else if (index.version == 4) {
        index.minimum.push_back({});
        index.maximum.push_back({});
        char comma = '\0';
        for (int component = 0; component < index.components; ++component) {
            index.minimum.front().push_back(readRequired<double>(input, "FabArray minimum"));
            if (!(input >> comma) || comma != ',') {
                throw MetadataReadError("malformed FabArray minima");
            }
        }
        for (int component = 0; component < index.components; ++component) {
            index.maximum.front().push_back(readRequired<double>(input, "FabArray maximum"));
            if (!(input >> comma) || comma != ',') {
                throw MetadataReadError("malformed FabArray maxima");
            }
        }
    }

    if (index.version >= 2) {
        // AMReX's VisMF serializer writes a blank separator line before the
        // RealDescriptor in header versions 2 and 3 (a trailing '\n' after
        // the FabOnDisk list and after each per-block min/max matrix).
        // readNonEmptyLine skips blank lines, mirroring how AMReX reads the
        // descriptor with operator>>. Version 4 emits no separator.
        index.realDescriptor = readNonEmptyLine(input, "VisMF RealDescriptor");
    }
    return index;
}

namespace {

IntBox physicalBoundsToCellBox(
    const Real3& lower, const Real3& upper, const Real3& problemLower,
    const Real3& cellSize, const Int3& domainLower, const Int3& centering,
    int dimension)
{
    IntBox box;
    box.centering = centering;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto loValue = std::round(
            (lower[i] - problemLower[i]) / cellSize[i]);
        const auto hiValue = std::round(
            (upper[i] - problemLower[i]) / cellSize[i]);
        if (!std::isfinite(loValue) || !std::isfinite(hiValue)
            || loValue < static_cast<double>(std::numeric_limits<int>::min())
            || loValue > static_cast<double>(std::numeric_limits<int>::max())
            || hiValue < static_cast<double>(std::numeric_limits<int>::min()) + 1.0
            || hiValue > static_cast<double>(std::numeric_limits<int>::max())) {
            throw MetadataReadError("grid bounds exceed supported integer range");
        }
        const auto indexedLower = static_cast<std::int64_t>(domainLower[i])
            + static_cast<std::int64_t>(loValue);
        const auto indexedUpper = static_cast<std::int64_t>(domainLower[i])
            + static_cast<std::int64_t>(hiValue) - 1;
        if (indexedLower < std::numeric_limits<int>::min()
            || indexedLower > std::numeric_limits<int>::max()
            || indexedUpper < std::numeric_limits<int>::min()
            || indexedUpper > std::numeric_limits<int>::max()) {
            throw MetadataReadError("grid bounds plus domain origin exceed integer range");
        }
        box.lower[i] = static_cast<int>(indexedLower);
        box.upper[i] = static_cast<int>(indexedUpper);
    }
    return box;
}

} // namespace

PlotfileMetadataResult PlotfileMetadataReader::read(
    const std::filesystem::path& plotfile) const
{
    const auto headerPath = plotfile / "Header";
    std::error_code sizeError;
    const auto headerSize = std::filesystem::file_size(headerPath, sizeError);
    if (sizeError) {
        throw MetadataReadError("cannot stat plotfile Header '" + headerPath.string()
            + "': " + sizeError.message());
    }

    std::ifstream input(headerPath, std::ios::binary);
    if (!input) {
        throw MetadataReadError("cannot open plotfile Header '" + headerPath.string() + "'");
    }

    auto metadata = std::make_shared<DatasetMetadata>();
    const auto fileVersion = readRequired<std::string>(input, "file version");
    const auto componentCount = readRequired<int>(input, "component count");
    if (componentCount < 0 || componentCount > maximumComponents) {
        throw MetadataReadError("plotfile component count is outside supported bounds");
    }

    input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    metadata->fields.reserve(static_cast<std::size_t>(componentCount));
    for (int component = 0; component < componentCount; ++component) {
        auto name = readNonEmptyLine(input, "component name");
        metadata->fields.push_back({name, 1, Centering::Cell, {std::move(name)}});
    }

    metadata->dimension = readRequired<int>(input, "space dimension");
    metadata->time = readRequired<double>(input, "time");
    metadata->finestLevel = readRequired<int>(input, "finest level");
    if (metadata->dimension < 1 || metadata->dimension > 3) {
        throw MetadataReadError("plotfile space dimension must be between 1 and 3");
    }
    if (metadata->finestLevel < 0 || metadata->finestLevel >= maximumLevels) {
        throw MetadataReadError("plotfile finest level is outside supported bounds");
    }
    const auto levelCount = static_cast<std::size_t>(metadata->finestLevel + 1);

    for (int axis = 0; axis < metadata->dimension; ++axis) {
        metadata->physicalDomain.lower[static_cast<std::size_t>(axis)] =
            readRequired<double>(input, "physical lower bound");
    }
    for (int axis = 0; axis < metadata->dimension; ++axis) {
        metadata->physicalDomain.upper[static_cast<std::size_t>(axis)] =
            readRequired<double>(input, "physical upper bound");
    }

    for (int level = 0; level < metadata->finestLevel; ++level) {
        const auto storedRatio = readRequired<int>(input, "refinement ratio");
        if (storedRatio <= 0) {
            throw MetadataReadError("plotfile refinement ratio must be positive");
        }
    }
    input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    metadata->levels.resize(levelCount);
    for (std::size_t level = 0; level < levelCount; ++level) {
        auto& levelMetadata = metadata->levels[level];
        levelMetadata.level = static_cast<int>(level);
        levelMetadata.domain = readAmrexBox(
            input, metadata->dimension, "level domain");
        levelMetadata.refinementRatioToNext = {{1, 1, 1}};
    }
    for (std::size_t level = 0; level + 1 < levelCount; ++level) {
        for (int axis = 0; axis < metadata->dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            const auto coarseLength =
                static_cast<std::int64_t>(metadata->levels[level].domain.upper[i])
                - metadata->levels[level].domain.lower[i] + 1;
            const auto fineLength =
                static_cast<std::int64_t>(metadata->levels[level + 1].domain.upper[i])
                - metadata->levels[level + 1].domain.lower[i] + 1;
            if (coarseLength <= 0 || fineLength <= 0
                || fineLength % coarseLength != 0
                || fineLength / coarseLength > std::numeric_limits<int>::max()) {
                throw MetadataReadError(
                    "level domains do not define an integral refinement ratio");
            }
            metadata->levels[level].refinementRatioToNext[i] =
                static_cast<int>(fineLength / coarseLength);
        }
    }

    for (auto& level : metadata->levels) {
        level.step = readRequired<int>(input, "level step");
    }
    for (auto& level : metadata->levels) {
        for (int axis = 0; axis < metadata->dimension; ++axis) {
            level.cellSize[static_cast<std::size_t>(axis)] =
                readRequired<double>(input, "level cell size");
            const auto i = static_cast<std::size_t>(axis);
            level.indexOrigin[i] = metadata->physicalDomain.lower[i]
                - static_cast<double>(level.domain.lower[i]) * level.cellSize[i];
        }
    }

    metadata->coordinateSystem = readRequired<int>(input, "coordinate system");
    [[maybe_unused]] const auto boundaryWidth = readRequired<int>(input, "boundary width");

    for (std::size_t levelIndex = 0; levelIndex < levelCount; ++levelIndex) {
        const auto headerLevel = readRequired<int>(input, "level number");
        const auto gridCount = readRequired<int>(input, "grid count");
        [[maybe_unused]] const auto gridTime = readRequired<double>(input, "level time");
        const auto headerStep = readRequired<int>(input, "level step");
        if (headerLevel != static_cast<int>(levelIndex)) {
            throw MetadataReadError("plotfile level records are out of order");
        }
        if (gridCount < 0 || gridCount > maximumGridsPerLevel) {
            throw MetadataReadError("plotfile grid count is outside supported bounds");
        }

        auto& level = metadata->levels[levelIndex];
        level.step = headerStep;
        level.boxes.reserve(static_cast<std::size_t>(gridCount));
        for (int grid = 0; grid < gridCount; ++grid) {
            Real3 lower;
            Real3 upper;
            for (int axis = 0; axis < metadata->dimension; ++axis) {
                const auto i = static_cast<std::size_t>(axis);
                lower[i] = readRequired<double>(input, "grid physical lower bound");
                upper[i] = readRequired<double>(input, "grid physical upper bound");
            }
            level.boxes.push_back(physicalBoundsToCellBox(
                lower, upper, metadata->physicalDomain.lower, level.cellSize,
                level.domain.lower, level.domain.centering, metadata->dimension));
        }
        level.dataPath = readRequired<std::string>(input, "level data path");
    }

    const auto issues = validateMetadata(*metadata);
    if (!issues.empty()) {
        throw MetadataReadError("invalid plotfile metadata at " + issues.front().path
            + ": " + issues.front().message);
    }

    MetadataReadMetrics metrics{1, headerSize, 0, 0};
    for (auto& level : metadata->levels) {
        const auto dataPrefix = plotfile / level.dataPath;
        const auto indexPath = std::filesystem::path(dataPrefix.string() + "_H");
        const auto visMf = detail::readVisMfIndex(indexPath, metadata->dimension);
        ++metrics.filesRead;
        metrics.bytesRead += visMf.bytesRead;
        if (visMf.components != componentCount) {
            throw MetadataReadError("VisMF component count does not match plotfile Header");
        }
        if (visMf.boxes.size() != level.boxes.size()) {
            throw MetadataReadError("VisMF BoxArray does not match plotfile grid count");
        }

        level.boxes = visMf.boxes;
        level.ghostWidth = visMf.ghostWidth;
        level.storedComponents = visMf.components;
        level.visMfHeaderVersion = visMf.version;
        level.realDescriptor = visMf.realDescriptor;
        level.blocks.clear();
        level.blocks.reserve(visMf.boxes.size());
        for (std::size_t block = 0; block < visMf.boxes.size(); ++block) {
            BlockMetadata blockMetadata;
            blockMetadata.box = visMf.boxes[block];
            blockMetadata.filePath = (
                std::filesystem::path(level.dataPath).parent_path()
                / visMf.fileNames[block]).generic_string();
            blockMetadata.fileOffset = visMf.fileOffsets[block];
            if (visMf.hasPerBlockStatistics
                && visMf.minimum.size() == visMf.boxes.size()
                && visMf.maximum.size() == visMf.boxes.size()) {
                blockMetadata.statistics = BlockStatistics{
                    visMf.minimum[block], visMf.maximum[block]};
            }
            level.blocks.push_back(std::move(blockMetadata));
        }
    }

    const auto indexedIssues = validateMetadata(*metadata);
    if (!indexedIssues.empty()) {
        throw MetadataReadError("invalid indexed plotfile metadata at "
            + indexedIssues.front().path + ": " + indexedIssues.front().message);
    }

    return {
        std::shared_ptr<const DatasetMetadata>(std::move(metadata)),
        metrics,
        fileVersion
    };
}

} // namespace amrvis
