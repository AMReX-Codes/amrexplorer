#include <amrexplorer/io/FabCatalog.hpp>
#include <amrexplorer/io/detail/FabHeaderParsing.hpp>

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

std::size_t balancedEnd(const std::string& text, std::size_t start)
{
    return detail::balancedExpressionEnd<MetadataReadError>(text, start);
}

std::pair<FabRealPrecision, std::size_t> parsePrecision(
    const std::string& descriptor)
{
    // The strict shared parse: unlike the previous catalog-local variant it
    // also validates the format-entry count and the byte order, so a
    // descriptor that would fail at the first block read now fails at
    // cataloging too instead of slipping through.
    const auto parsed
        = detail::parseRealDescriptor<MetadataReadError>(descriptor);
    return {parsed.bytes == 4 ? FabRealPrecision::Single
                              : FabRealPrecision::Double,
        parsed.bytes};
}

IntBox parseBox(const std::string& text, int& dimension)
{
    return detail::parseAmrexBoxInferDimension<MetadataReadError>(
        text, dimension);
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
    return detail::grownBox<MetadataReadError>(validBox, ghostWidth, dimension);
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
    if (!detail::readBoundedLine<MetadataReadError>(input, line)
        || !line.starts_with("FAB ")) {
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
