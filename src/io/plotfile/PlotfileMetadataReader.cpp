#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/detail/FabHeaderParsing.hpp>
#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/detail/VisMfIndex.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace amrvis {
namespace {

constexpr int maximumComponents = 100'000;
constexpr int maximumLevels = 1'000;
constexpr int maximumGridsPerLevel = 10'000'000;

// Every Header field goes through here. String fields -- the file version, the
// FabOnDisk prefix and filename, the level data path -- are bounded and
// length-checked by the shared helper; the numeric ones need no ceiling because
// >> stops at the first character that cannot extend the value.
template <typename T>
T readRequired(std::istream& input, std::string_view description)
{
    if constexpr (std::is_same_v<T, std::string>) {
        return detail::readBoundedToken<MetadataReadError>(
            input, "plotfile Header", description);
    } else {
        T value{};
        if (!(input >> value)) {
            throw MetadataReadError("malformed plotfile Header while reading "
                + std::string(description));
        }
        return value;
    }
}

// Reads one VisMF min/max statistic, which -- unlike the geometry fields --
// may be non-finite. AMReX's FArrayBox::min/max propagate +/-inf (and can
// leave a NaN) from an overflowed run, and its plotfile headers carry those
// per-block/FabArray extrema serialized as the "inf" / "-inf" / "nan" text
// C++ ostreams emit. operator>>(double) cannot parse that text, so a plain
// readRequired<double> made the whole (loadable) plotfile refuse to open --
// exactly the run a user opens a viewer to debug. std::strtod does accept
// those tokens, so read a comma/whitespace-delimited token and strtod it;
// genuinely malformed tokens still fault. The non-finite value is stored
// as-is: metadataValueRange discards non-finite block statistics, so File/
// Level range modes degrade to Visible rather than the open failing (see
// nonfinite-header-statistics-unopenable).
double readStatisticValue(std::istream& input, std::string_view description)
{
    input >> std::ws;
    std::string token;
    for (auto next = input.peek();
         next != std::char_traits<char>::eof()
             && next != ','
             && std::isspace(static_cast<unsigned char>(next)) == 0;
         next = input.peek()) {
        // The loop is delimited by the file's own content, so a run without a
        // comma or whitespace would otherwise accumulate without limit. Unlike
        // a name or a path, a numeric token has no legitimate length anywhere
        // near this ceiling, so hitting it needs no complete-versus-truncated
        // distinction -- it is malformed either way.
        if (token.size() >= detail::maximumHeaderTokenBytes) {
            throw MetadataReadError("plotfile Header "
                + std::string(description) + " exceeds the supported length");
        }
        token.push_back(static_cast<char>(input.get()));
    }
    const char* begin = token.c_str();
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (token.empty() || end != begin + token.size()) {
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

// The function that walks a plotfile Header, so its ceiling matters most: a
// Header that never supplies a newline would otherwise accumulate the whole
// remaining file into one line before the parse could reject it.
std::string readNonEmptyLine(std::istream& input, std::string_view description)
{
    std::string line;
    while (detail::readBoundedLine<MetadataReadError>(input, line)) {
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

using detail::parseIntegers;

// Rejects a metadata-derived path that could redirect reads outside the
// plotfile directory when joined to the plotfile root: an absolute path
// replaces the root entirely, and a '..' component walks above it. AMReX only
// ever writes relative names within the tree, so anything else is malformed
// or crafted.
void requireContainedPath(const std::string& value, std::string_view what)
{
    const std::filesystem::path path(value);
    // Reject anything with a root: a root-name (drive/UNC) or a root-directory
    // (leading separator). is_absolute() is not enough — it is platform
    // specific, so a POSIX-style "/etc/..." reads as relative on Windows yet
    // still escapes to the drive root when joined to the plotfile path.
    if (value.empty() || path.has_root_name() || path.has_root_directory()) {
        throw MetadataReadError(std::string(what)
            + " must be a relative path inside the plotfile: '" + value + "'");
    }
    for (const auto& component : path) {
        if (component == "..") {
            throw MetadataReadError(std::string(what)
                + " must not contain a parent-directory component: '"
                + value + "'");
        }
    }
}

// Reads a comma-separated VisMF real matrix. AMReX always writes exactly
// expectedRows x expectedColumns (one row per box, one column per component),
// so the claimed dimensions are checked against that before any allocation:
// a crafted header cannot request a huge matrix and OOM the process (the
// dimensions were previously capped only independently, then allocated in
// full before the count was cross-checked).
std::vector<std::vector<double>> readRealMatrix(
    std::istream& input, std::string_view description,
    std::uint64_t expectedRows, std::uint64_t expectedColumns)
{
    const auto rows = readRequired<std::uint64_t>(input, description);
    char comma = '\0';
    if (!(input >> comma) || comma != ',') {
        throw MetadataReadError("malformed VisMF matrix dimensions");
    }
    const auto columns = readRequired<std::uint64_t>(input, description);
    if (rows != expectedRows || columns != expectedColumns) {
        throw MetadataReadError("VisMF matrix dimensions do not match the "
            "BoxArray size and component count");
    }

    std::vector<std::vector<double>> matrix(
        static_cast<std::size_t>(rows),
        std::vector<double>(static_cast<std::size_t>(columns)));
    for (auto& row : matrix) {
        for (auto& value : row) {
            value = readStatisticValue(input, description);
            if (!(input >> comma) || comma != ',') {
                throw MetadataReadError("malformed comma-separated VisMF matrix");
            }
        }
    }
    return matrix;
}

} // namespace

detail::VisMfIndex detail::readVisMfIndex(
    const std::filesystem::path& headerPath, int dimension,
    StopToken cancellation)
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
    // The cap above rejects the absurd; the file's own size bounds what is
    // merely large. A declared count is a claim, and the Header cannot describe
    // more entries than its bytes allow: the shortest legal BoxArray entry is
    // "((0)(0)(0))", eleven bytes at one dimension and more at two or three,
    // and the shortest legal location record is "FabOnDisk: a 0", fourteen.
    // Reserving from the declared count alone let a Header of a few dozen bytes
    // claim ten million boxes plus ten million names and offsets -- roughly
    // 750 MB -- before a single entry was parsed, which is the same trade the
    // particle grid table already refuses to make. Both reserves are
    // optimizations, so under-reserving a legitimate file costs one
    // reallocation and nothing else; that is why the floors here sit below the
    // true minimums rather than being tuned to them.
    constexpr std::uint64_t minimumBytesPerBoxEntry = 8;
    constexpr std::uint64_t minimumBytesPerLocationRecord = 12;
    const auto evidenceBoundedCount = [headerSize](std::uint64_t declared,
                                          std::uint64_t bytesPerRecord) {
        return static_cast<std::size_t>(std::min(
            declared, static_cast<std::uint64_t>(headerSize) / bytesPerRecord));
    };
    index.boxes.reserve(evidenceBoundedCount(
        static_cast<std::uint64_t>(boxCount), minimumBytesPerBoxEntry));
    for (std::size_t box = 0; box < boxCount; ++box) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        index.boxes.push_back(readAmrexBox(input, dimension, "VisMF BoxArray entry"));
    }
    if (readNonEmptyLine(input, "VisMF BoxArray terminator") != ")") {
        throw MetadataReadError("malformed VisMF BoxArray terminator");
    }

    const auto locationCount = readRequired<std::uint64_t>(input, "VisMF location count");
    if (locationCount != boxCount) {
        throw MetadataReadError("VisMF location count does not match BoxArray size");
    }
    const auto reservableLocations = evidenceBoundedCount(
        static_cast<std::uint64_t>(boxCount), minimumBytesPerLocationRecord);
    index.fileNames.reserve(reservableLocations);
    index.fileOffsets.reserve(reservableLocations);
    for (std::size_t block = 0; block < boxCount; ++block) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto prefix = readRequired<std::string>(input, "FabOnDisk prefix");
        if (prefix != "FabOnDisk:") {
            throw MetadataReadError("malformed FabOnDisk record");
        }
        index.fileNames.push_back(readRequired<std::string>(input, "FAB data filename"));
        requireContainedPath(index.fileNames.back(), "FAB data filename");
        index.fileOffsets.push_back(readRequired<std::uint64_t>(input, "FAB data offset"));
    }

    if (index.version == 1 || index.version == 3) {
        index.minimum = readRealMatrix(input, "per-block minima",
            static_cast<std::uint64_t>(boxCount),
            static_cast<std::uint64_t>(index.components));
        index.maximum = readRealMatrix(input, "per-block maxima",
            static_cast<std::uint64_t>(boxCount),
            static_cast<std::uint64_t>(index.components));
        index.hasPerBlockStatistics = true;
        if (index.minimum.size() != boxCount || index.maximum.size() != boxCount) {
            throw MetadataReadError("VisMF statistics do not match BoxArray size");
        }
    } else if (index.version == 4) {
        index.minimum.push_back({});
        index.maximum.push_back({});
        char comma = '\0';
        for (int component = 0; component < index.components; ++component) {
            index.minimum.front().push_back(
                readStatisticValue(input, "FabArray minimum"));
            if (!(input >> comma) || comma != ',') {
                throw MetadataReadError("malformed FabArray minima");
            }
        }
        for (int component = 0; component < index.components; ++component) {
            index.maximum.front().push_back(
                readStatisticValue(input, "FabArray maximum"));
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
    const std::filesystem::path& plotfile, StopToken cancellation) const
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
    if (cancellation.stop_requested()) {
        throw ReadCancelled();
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
        metadata->fields.push_back({name, Centering::Cell, {std::move(name)}});
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
            if (cancellation.stop_requested()) {
                throw ReadCancelled();
            }
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
        requireContainedPath(level.dataPath, "plotfile level data path");
    }

    const auto issues = validateMetadata(*metadata);
    if (!issues.empty()) {
        throw MetadataReadError("invalid plotfile metadata at " + issues.front().path
            + ": " + issues.front().message);
    }

    MetadataReadMetrics metrics{1, headerSize, 0, 0};
    for (auto& level : metadata->levels) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto dataPrefix = plotfile / level.dataPath;
        const auto indexPath = std::filesystem::path(dataPrefix.string() + "_H");
        const auto visMf = detail::readVisMfIndex(indexPath, metadata->dimension, cancellation);
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
