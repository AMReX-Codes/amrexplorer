#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/detail/FabHeaderParsing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace amrvis {
namespace {

// Shared strict parsers from FabHeaderParsing.hpp, bound to this reader's
// error type.
using RealEncoding = detail::ParsedRealDescriptor;

IntBox parseAmrexBox(const std::string& text, int dimension)
{
    return detail::parseAmrexBox<BlockReadError>(text, dimension);
}

std::size_t balancedExpressionEnd(const std::string& text, std::size_t start)
{
    return detail::balancedExpressionEnd<BlockReadError>(text, start);
}

RealEncoding parseRealDescriptor(const std::string& descriptor)
{
    return detail::parseRealDescriptor<BlockReadError>(descriptor);
}

struct FabHeader {
    RealEncoding encoding;
    IntBox box;
    int components = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t headerBytes = 0;
};

FabHeader readFabHeader(
    std::ifstream& input, std::uint64_t fileOffset, int dimension)
{
    input.seekg(static_cast<std::streamoff>(fileOffset), std::ios::beg);
    if (!input) {
        throw BlockReadError("cannot seek to indexed FAB offset");
    }
    std::string line;
    if (!std::getline(input, line) || !line.starts_with("FAB ")) {
        throw BlockReadError("indexed FAB does not have a supported header");
    }
    const auto descriptorStart = line.find('(', 4);
    const auto descriptorEnd = balancedExpressionEnd(line, descriptorStart);
    const auto boxStart = line.find('(', descriptorEnd);
    const auto boxEnd = balancedExpressionEnd(line, boxStart);

    const auto descriptor = line.substr(descriptorStart, descriptorEnd - descriptorStart);
    const auto boxText = line.substr(boxStart, boxEnd - boxStart);
    std::istringstream componentInput(line.substr(boxEnd));
    int components = 0;
    if (!(componentInput >> components) || components <= 0) {
        throw BlockReadError("FAB header has an invalid component count");
    }
    const auto dataPosition = input.tellg();
    if (dataPosition < 0) {
        throw BlockReadError("cannot determine FAB payload offset");
    }
    const auto dataOffset = static_cast<std::uint64_t>(dataPosition);
    return {
        parseRealDescriptor(descriptor),
        parseAmrexBox(boxText, dimension),
        components,
        dataOffset,
        dataOffset - fileOffset
    };
}

IntBox grownBox(const IntBox& source, const Int3& ghost, int dimension)
{
    return detail::grownBox<BlockReadError>(source, ghost, dimension);
}

std::uint64_t pointCount(const IntBox& box, int dimension)
{
    std::uint64_t result = 1;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto signedLength = static_cast<std::int64_t>(box.upper[i])
            - box.lower[i] + 1;
        if (signedLength <= 0) {
            throw BlockReadError("FAB box has a non-positive extent");
        }
        const auto length = static_cast<std::uint64_t>(signedLength);
        if (result > std::numeric_limits<std::uint64_t>::max() / length) {
            throw BlockReadError("FAB point count overflows byte accounting");
        }
        result *= length;
    }
    return result;
}

} // namespace

FabValues::FabValues(std::vector<float> values) noexcept
    : m_storage(std::move(values))
{}

FabValues::FabValues(std::vector<double> values) noexcept
    : m_storage(std::move(values))
{}

FabRealPrecision FabValues::precision() const noexcept
{
    return std::holds_alternative<std::vector<float>>(m_storage)
        ? FabRealPrecision::Single : FabRealPrecision::Double;
}

std::size_t FabValues::size() const noexcept
{
    return std::visit([](const auto& values) { return values.size(); }, m_storage);
}

std::size_t FabValues::elementBytes() const noexcept
{
    return precision() == FabRealPrecision::Single ? sizeof(float) : sizeof(double);
}

std::uint64_t FabValues::residentBytes() const noexcept
{
    return std::visit([](const auto& values) {
        using Value = typename std::decay_t<decltype(values)>::value_type;
        return static_cast<std::uint64_t>(values.capacity()) * sizeof(Value);
    }, m_storage);
}

double FabValues::operator[](std::size_t index) const noexcept
{
    return std::visit([index](const auto& values) {
        return static_cast<double>(values[index]);
    }, m_storage);
}

PlotfileBlockReader::PlotfileBlockReader(
    std::filesystem::path plotfile, std::shared_ptr<const DatasetMetadata> metadata)
    : m_plotfile(std::move(plotfile))
    , m_metadata(std::move(metadata))
{
    if (!m_metadata) {
        throw std::invalid_argument("PlotfileBlockReader requires dataset metadata");
    }
}

BlockReadResult PlotfileBlockReader::readBlock(
    const BlockRequest& request, StopToken cancellation) const
{
    if (cancellation.stop_requested()) {
        throw ReadCancelled();
    }
    if (request.level < 0
        || static_cast<std::size_t>(request.level) >= m_metadata->levels.size()) {
        throw BlockReadError("requested level is unavailable");
    }
    const auto& level = m_metadata->levels[static_cast<std::size_t>(request.level)];
    if (request.gridIndex < 0
        || static_cast<std::size_t>(request.gridIndex) >= level.blocks.size()) {
        throw BlockReadError("requested grid is unavailable");
    }
    if (request.componentCount != 1 || request.firstComponent != 0) {
        throw BlockReadError("the initial selective reader accepts one scalar field");
    }
    const auto physicalComponent = static_cast<std::uint64_t>(request.field.value);
    if (physicalComponent >= static_cast<std::uint64_t>(level.storedComponents)) {
        throw BlockReadError("requested field component is unavailable");
    }

    const auto& blockMetadata = level.blocks[static_cast<std::size_t>(request.gridIndex)];
    const auto dataPath = m_plotfile / blockMetadata.filePath;
    std::ifstream input(dataPath, std::ios::binary);
    if (!input) {
        throw BlockReadError("cannot open FAB data file '" + dataPath.string() + "'");
    }
    // Stat the file up front so a crafted box extent (which sizes the buffer
    // below) cannot force a huge transient allocation or bad_alloc: the
    // requested component must actually fit within the data file.
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(dataPath, sizeError);
    if (sizeError) {
        throw BlockReadError("cannot stat FAB data file '" + dataPath.string()
            + "': " + sizeError.message());
    }

    FabHeader header;
    if (level.visMfHeaderVersion == 1) {
        header = readFabHeader(
            input, blockMetadata.fileOffset, m_metadata->dimension);
    } else {
        header.encoding = parseRealDescriptor(level.realDescriptor);
        header.box = grownBox(blockMetadata.box, level.ghostWidth, m_metadata->dimension);
        header.components = level.storedComponents;
        header.dataOffset = blockMetadata.fileOffset;
    }
    if (header.components != level.storedComponents) {
        throw BlockReadError("FAB and VisMF component counts disagree");
    }

    const auto valuesPerComponent = pointCount(header.box, m_metadata->dimension);
    if (valuesPerComponent > std::numeric_limits<std::uint64_t>::max() / header.encoding.bytes) {
        throw BlockReadError("FAB component byte count overflows");
    }
    const auto componentBytes = valuesPerComponent * header.encoding.bytes;
    if (physicalComponent > std::numeric_limits<std::uint64_t>::max() / componentBytes) {
        throw BlockReadError("FAB component offset overflows");
    }
    const auto componentDelta = physicalComponent * componentBytes;
    if (header.dataOffset > std::numeric_limits<std::uint64_t>::max() - componentDelta) {
        throw BlockReadError("FAB component offset overflows");
    }
    const auto componentOffset = header.dataOffset + componentDelta;
    if (componentOffset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw BlockReadError("FAB component offset exceeds stream limits");
    }
    input.seekg(static_cast<std::streamoff>(componentOffset), std::ios::beg);
    if (!input) {
        throw BlockReadError("cannot seek to requested FAB component");
    }
    if (componentBytes > std::numeric_limits<std::size_t>::max()
        || componentBytes
            > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw BlockReadError("FAB component exceeds addressable memory");
    }
    // Bound the allocation by the actual file (staged to avoid overflow in
    // offset + bytes): a truncated file or an oversized claimed box is caught
    // here, before the buffer is sized, rather than after a failed read.
    if (componentOffset > fileSize || componentBytes > fileSize - componentOffset) {
        throw BlockReadError("FAB component extends past the end of the data file");
    }

    // Read the component straight into its typed value buffer instead of
    // staging raw bytes and decoding into a second, equally large vector: this
    // halves peak memory. IEEE-32/64 data always matches the target type's
    // size (see parseRealDescriptor), so on a native-endian file (the common
    // case) the read is the whole decode; a cross-endian file only needs an
    // in-place per-value byte swap afterward.
    const bool nativeEndian = (std::endian::native == std::endian::little)
        == header.encoding.littleEndian;
    const auto readComponent = [&]<typename Value>() {
        std::vector<Value> values(static_cast<std::size_t>(valuesPerComponent));
        constexpr std::size_t cancellationChunkBytes = 1024U * 1024U;
        auto* const storage = reinterpret_cast<char*>(values.data());
        std::size_t bytesCompleted = 0;
        const auto total = static_cast<std::size_t>(componentBytes);
        while (bytesCompleted < total) {
            if (cancellation.stop_requested()) {
                throw ReadCancelled();
            }
            const auto chunk = std::min(
                cancellationChunkBytes, total - bytesCompleted);
            input.read(storage + bytesCompleted,
                static_cast<std::streamsize>(chunk));
            if (input.gcount() != static_cast<std::streamsize>(chunk)) {
                throw BlockReadError("FAB component payload is truncated");
            }
            bytesCompleted += chunk;
        }
        if (!nativeEndian) {
            auto* const raw = reinterpret_cast<unsigned char*>(values.data());
            for (std::size_t value = 0; value < values.size(); ++value) {
                if ((value & 4095U) == 0U && cancellation.stop_requested()) {
                    throw ReadCancelled();
                }
                std::reverse(raw + value * sizeof(Value),
                    raw + (value + 1) * sizeof(Value));
            }
        }
        return FabValues{std::move(values)};
    };

    auto block = std::make_shared<FabBlock>();
    block->box = header.box;
    block->field = request.field;
    block->component = 0;
    block->values = header.encoding.bytes == sizeof(float)
        ? readComponent.template operator()<float>()
        : readComponent.template operator()<double>();

    return {
        std::shared_ptr<const FabBlock>(std::move(block)),
        BlockReadMetrics{1, header.headerBytes + componentBytes, valuesPerComponent}
    };
}

} // namespace amrvis
