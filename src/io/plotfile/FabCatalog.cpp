#include <amrexplorer/io/FabCatalog.hpp>

#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

namespace amrvis {
namespace {

std::vector<int> parseIntegers(const std::string& text)
{
    std::string numbers = text;
    std::replace_if(numbers.begin(), numbers.end(), [](char character) {
        return !(character >= '0' && character <= '9')
            && character != '-' && character != '+';
    }, ' ');
    std::istringstream input(numbers);
    std::vector<int> values;
    int value = 0;
    while (input >> value) values.push_back(value);
    return values;
}

std::size_t balancedEnd(const std::string& text, std::size_t start)
{
    if (start >= text.size() || text[start] != '(') {
        throw MetadataReadError("expected a parenthesized FAB header expression");
    }
    int depth = 0;
    for (std::size_t i = start; i < text.size(); ++i) {
        depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
        if (depth == 0) return i + 1;
    }
    throw MetadataReadError("unterminated FAB header expression");
}

std::pair<FabRealPrecision, std::size_t> parsePrecision(
    const std::string& descriptor)
{
    const auto values = parseIntegers(descriptor);
    constexpr std::size_t start = 1;
    constexpr std::array<int, 8> ieee32{32, 8, 23, 0, 1, 9, 0, 127};
    constexpr std::array<int, 8> ieee64{64, 11, 52, 0, 1, 12, 0, 1023};
    if (values.size() >= 9
        && std::equal(ieee32.begin(), ieee32.end(), values.begin() + start)) {
        return {FabRealPrecision::Single, 4};
    }
    if (values.size() >= 9
        && std::equal(ieee64.begin(), ieee64.end(), values.begin() + start)) {
        return {FabRealPrecision::Double, 8};
    }
    throw MetadataReadError("only IEEE-32 and IEEE-64 FAB data are supported");
}

IntBox parseBox(const std::string& text, int& dimension)
{
    const auto firstTuple = text.find('(', 1);
    const auto firstEnd = text.find(')', firstTuple);
    if (firstTuple == std::string::npos || firstEnd == std::string::npos) {
        throw MetadataReadError("cannot infer FAB dimension");
    }
    dimension = static_cast<int>(
        parseIntegers(text.substr(firstTuple, firstEnd - firstTuple + 1)).size());
    const auto values = parseIntegers(text);
    if (dimension < 1 || dimension > 3
        || values.size() != static_cast<std::size_t>(dimension * 3)) {
        throw MetadataReadError("malformed AMReX Box in FAB header");
    }
    IntBox box;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        box.lower[i] = values[i];
        box.upper[i] = values[static_cast<std::size_t>(dimension + axis)];
        box.centering[i] = values[static_cast<std::size_t>(2 * dimension + axis)];
    }
    return box;
}

std::uint64_t payloadBytes(
    const IntBox& box, int dimension, int components, std::size_t bytes)
{
    std::uint64_t points = 1;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto extent = static_cast<std::int64_t>(box.upper[i])
            - box.lower[i] + 1;
        if (extent <= 0
            || points > std::numeric_limits<std::uint64_t>::max()
                / static_cast<std::uint64_t>(extent)) {
            throw MetadataReadError("FAB extent overflows payload size");
        }
        points *= static_cast<std::uint64_t>(extent);
    }
    if (components <= 0
        || points > std::numeric_limits<std::uint64_t>::max()
            / static_cast<std::uint64_t>(components)
        || points * static_cast<std::uint64_t>(components)
            > std::numeric_limits<std::uint64_t>::max() / bytes) {
        throw MetadataReadError("FAB payload size overflows");
    }
    return points * static_cast<std::uint64_t>(components) * bytes;
}

std::optional<std::uint64_t> findNextFabHeader(
    const std::filesystem::path& path, std::uint64_t start,
    std::uint64_t fileSize)
{
    if (start >= fileSize) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw MetadataReadError("cannot reopen FAB '" + path.string() + "'");
    }
    input.seekg(static_cast<std::streamoff>(start));
    constexpr std::string_view marker = "FAB ";
    std::size_t matched = 0;
    char byte = '\0';
    std::uint64_t position = start;
    while (position < fileSize && input.get(byte)) {
        if (byte == marker[matched]) {
            ++matched;
            if (matched == marker.size()) {
                return position + 1U - marker.size();
            }
        } else {
            matched = byte == marker.front() ? 1U : 0U;
        }
        ++position;
    }
    return std::nullopt;
}

bool startsWithFabHeader(
    const std::filesystem::path& path, std::uint64_t offset)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw MetadataReadError("cannot open FAB '" + path.string() + "'");
    }
    input.seekg(static_cast<std::streamoff>(offset));
    std::array<char, 4> marker{};
    input.read(marker.data(), static_cast<std::streamsize>(marker.size()));
    return input.gcount() == static_cast<std::streamsize>(marker.size())
        && marker == std::array<char, 4>{'F', 'A', 'B', ' '};
}

std::filesystem::path companionHeaderPath(
    const std::filesystem::path& dataPath)
{
    const auto filename = dataPath.filename().string();
    const auto marker = filename.rfind("_D_");
    if (marker == std::string::npos || marker + 3 == filename.size()
        || !std::all_of(filename.begin() + static_cast<std::ptrdiff_t>(marker + 3),
            filename.end(), [](char character) {
                return character >= '0' && character <= '9';
            })) {
        throw MetadataReadError(
            "FAB data has no inline header and its filename does not identify "
            "a companion MultiFab _H file");
    }
    return dataPath.parent_path()
        / (filename.substr(0, marker) + "_H");
}

IntBox storedBox(
    const IntBox& validBox, const Int3& ghostWidth, int dimension)
{
    auto result = validBox;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto index = static_cast<std::size_t>(axis);
        const auto lower = static_cast<std::int64_t>(validBox.lower[index])
            - ghostWidth[index];
        const auto upper = static_cast<std::int64_t>(validBox.upper[index])
            + ghostWidth[index];
        if (lower < std::numeric_limits<int>::min()
            || lower > std::numeric_limits<int>::max()
            || upper < std::numeric_limits<int>::min()
            || upper > std::numeric_limits<int>::max()) {
            throw MetadataReadError(
                "companion MultiFab ghost-grown FAB box exceeds integer range");
        }
        result.lower[index] = static_cast<int>(lower);
        result.upper[index] = static_cast<int>(upper);
    }
    return result;
}

std::vector<FabRecord> recordsFromCompanion(
    const std::filesystem::path& dataPath)
{
    const auto headerPath = companionHeaderPath(dataPath);
    if (!std::filesystem::is_regular_file(headerPath)) {
        throw MetadataReadError(
            "FAB data has no inline header and companion MultiFab header '"
            + headerPath.string() + "' is unavailable");
    }
    const auto multifab = StandaloneMetadataReader{}.readMultiFab(headerPath);
    if (multifab.metadata->levels.size() != 1) {
        throw MetadataReadError(
            "companion MultiFab header does not describe one level");
    }
    const auto& level = multifab.metadata->levels.front();
    if (level.visMfHeaderVersion < 2 || level.realDescriptor.empty()) {
        throw MetadataReadError(
            "companion MultiFab header does not describe headerless FAB data");
    }
    const auto precision = fabPrecisionFromDescriptor(level.realDescriptor);
    std::vector<FabRecord> records;
    for (std::size_t block = 0; block < level.blocks.size(); ++block) {
        const auto& metadata = level.blocks[block];
        if (std::filesystem::path(metadata.filePath).filename()
            != dataPath.filename()) {
            continue;
        }
        records.push_back({
            block,
            dataPath,
            metadata.fileOffset,
            metadata.fileOffset,
            storedBox(metadata.box, level.ghostWidth,
                multifab.metadata->dimension),
            multifab.metadata->dimension,
            level.storedComponents,
            precision,
            level.realDescriptor,
            level.visMfHeaderVersion
        });
    }
    if (records.empty()) {
        throw MetadataReadError(
            "companion MultiFab header contains no FabOnDisk record for '"
            + dataPath.filename().string() + "'");
    }
    return records;
}

} // namespace

FabRecord inspectFabRecord(
    const std::filesystem::path& path, std::uint64_t offset)
{
    if (!startsWithFabHeader(path, offset)) {
        auto records = recordsFromCompanion(path);
        const auto match = std::find_if(
            records.begin(), records.end(),
            [offset](const FabRecord& record) {
                return record.payloadOffset == offset;
            });
        if (match == records.end()) {
            throw MetadataReadError(
                "companion MultiFab header contains no FAB at byte "
                + std::to_string(offset));
        }
        return *match;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw MetadataReadError("cannot open FAB '" + path.string() + "'");
    }
    input.seekg(static_cast<std::streamoff>(offset));
    std::string line;
    if (!std::getline(input, line) || !line.starts_with("FAB ")) {
        throw MetadataReadError("expected FAB header at byte "
            + std::to_string(offset));
    }
    const auto descriptorStart = line.find('(', 4);
    const auto descriptorEnd = balancedEnd(line, descriptorStart);
    const auto boxStart = line.find('(', descriptorEnd);
    const auto boxEnd = balancedEnd(line, boxStart);
    const auto descriptor =
        line.substr(descriptorStart, descriptorEnd - descriptorStart);
    int dimension = 0;
    const auto box = parseBox(line.substr(boxStart, boxEnd - boxStart), dimension);
    int components = 0;
    std::istringstream tail(line.substr(boxEnd));
    if (!(tail >> components) || components <= 0) {
        throw MetadataReadError("FAB component count is invalid");
    }
    const auto [realPrecision, bytes] = parsePrecision(descriptor);
    const auto payload = input.tellg();
    if (payload < 0) {
        throw MetadataReadError("cannot determine FAB payload offset");
    }
    [[maybe_unused]] const auto checked =
        payloadBytes(box, dimension, components, bytes);
    return {0, path, offset, static_cast<std::uint64_t>(payload), box,
        dimension, components, realPrecision, descriptor, 1};
}

FabRealPrecision fabPrecisionFromDescriptor(std::string_view descriptor)
{
    return parsePrecision(std::string(descriptor)).first;
}

std::vector<FabRecord> scanFabFile(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw MetadataReadError("cannot stat FAB '" + path.string()
            + "': " + error.message());
    }
    if (size == 0) {
        throw MetadataReadError("raw FAB file is empty");
    }
    if (!startsWithFabHeader(path, 0)) {
        auto records = recordsFromCompanion(path);
        for (const auto& record : records) {
            const auto bytes =
                record.precision == FabRealPrecision::Single ? 4U : 8U;
            const auto payload = payloadBytes(
                record.storedBox, record.dimension, record.components, bytes);
            if (record.payloadOffset > size
                || payload > size - record.payloadOffset) {
                throw MetadataReadError(
                    "truncated headerless FAB payload at byte "
                    + std::to_string(record.payloadOffset));
            }
        }
        return records;
    }
    std::vector<FabRecord> records;
    std::uint64_t offset = 0;
    while (offset < size) {
        auto record = inspectFabRecord(path, offset);
        const auto bytes = record.precision == FabRealPrecision::Single ? 4U : 8U;
        const auto payload = payloadBytes(
            record.storedBox, record.dimension, record.components, bytes);
        if (record.payloadOffset > size || payload > size - record.payloadOffset) {
            throw MetadataReadError("truncated FAB payload at byte "
                + std::to_string(offset));
        }
        record.ordinal = records.size();
        records.push_back(record);
        const auto payloadEnd = record.payloadOffset + payload;
        if (payloadEnd == size) {
            break;
        }
        const auto next = findNextFabHeader(path, payloadEnd, size);
        if (!next) {
            break;
        }
        offset = *next;
    }
    if (records.empty()) {
        throw MetadataReadError("raw FAB file is empty");
    }
    return records;
}

} // namespace amrvis
