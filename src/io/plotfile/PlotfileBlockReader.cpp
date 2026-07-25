#include <amrexplorer/io/PlotfileBlockReader.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace amrvis {
namespace {

struct RealEncoding {
    std::size_t bytes = 0;
    bool littleEndian = false;
};

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
    while (input >> value) {
        values.push_back(value);
    }
    return values;
}

IntBox parseAmrexBox(const std::string& text, int dimension)
{
    const auto values = parseIntegers(text);
    if (values.size() != static_cast<std::size_t>(dimension * 3)) {
        throw BlockReadError("malformed AMReX Box in FAB header");
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

std::size_t balancedExpressionEnd(const std::string& text, std::size_t start)
{
    if (start >= text.size() || text[start] != '(') {
        throw BlockReadError("expected a parenthesized FAB header expression");
    }
    int depth = 0;
    for (std::size_t i = start; i < text.size(); ++i) {
        if (text[i] == '(') {
            ++depth;
        } else if (text[i] == ')') {
            --depth;
            if (depth == 0) {
                return i + 1;
            }
        }
    }
    throw BlockReadError("unterminated FAB header expression");
}

RealEncoding parseRealDescriptor(const std::string& descriptor)
{
    const auto values = parseIntegers(descriptor);
    constexpr std::size_t formatCountIndex = 0;
    constexpr std::size_t formatStartIndex = 1;
    constexpr std::size_t formatEntries = 8;
    constexpr std::size_t orderCountIndex = formatStartIndex + formatEntries;
    if (values.size() <= orderCountIndex
        || values[formatCountIndex] != static_cast<int>(formatEntries)) {
        throw BlockReadError("malformed FAB RealDescriptor");
    }

    constexpr std::array<int, formatEntries> ieee32{
        32, 8, 23, 0, 1, 9, 0, 127};
    constexpr std::array<int, formatEntries> ieee64{
        64, 11, 52, 0, 1, 12, 0, 1023};
    const auto matchesFormat = [&values](const auto& expected) {
        return std::equal(expected.begin(), expected.end(),
            values.begin() + static_cast<std::ptrdiff_t>(formatStartIndex));
    };
    const auto bytes = matchesFormat(ieee32) ? 4
        : matchesFormat(ieee64) ? 8 : 0;
    if (bytes == 0) {
        throw BlockReadError("only IEEE-32 and IEEE-64 FAB data are supported");
    }

    if (values[orderCountIndex] != bytes
        || values.size() < orderCountIndex + 1 + static_cast<std::size_t>(bytes)) {
        throw BlockReadError("malformed FAB byte-order descriptor");
    }

    bool ascending = true;
    bool descending = true;
    for (int byte = 0; byte < bytes; ++byte) {
        const auto value = values[orderCountIndex + 1 + static_cast<std::size_t>(byte)];
        ascending = ascending && value == byte + 1;
        descending = descending && value == bytes - byte;
    }
    if (!ascending && !descending) {
        throw BlockReadError("unsupported non-contiguous FAB byte order");
    }
    return {static_cast<std::size_t>(bytes), descending};
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
    auto result = source;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto lower = static_cast<std::int64_t>(source.lower[i]) - ghost[i];
        const auto upper = static_cast<std::int64_t>(source.upper[i]) + ghost[i];
        if (lower < std::numeric_limits<int>::min()
            || lower > std::numeric_limits<int>::max()
            || upper < std::numeric_limits<int>::min()
            || upper > std::numeric_limits<int>::max()) {
            throw BlockReadError("ghost-grown FAB box exceeds supported integer range");
        }
        result.lower[i] = static_cast<int>(lower);
        result.upper[i] = static_cast<int>(upper);
    }
    return result;
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

double decodeReal(const unsigned char* source, const RealEncoding& encoding)
{
    const bool nativeLittle = std::endian::native == std::endian::little;
    if (encoding.bytes == sizeof(double)) {
        std::array<unsigned char, sizeof(double)> bytes{};
        std::copy_n(source, bytes.size(), bytes.begin());
        if (nativeLittle != encoding.littleEndian) {
            std::reverse(bytes.begin(), bytes.end());
        }
        double value = 0.0;
        std::memcpy(&value, bytes.data(), sizeof(value));
        return value;
    }

    std::array<unsigned char, sizeof(float)> bytes{};
    std::copy_n(source, bytes.size(), bytes.begin());
    if (nativeLittle != encoding.littleEndian) {
        std::reverse(bytes.begin(), bytes.end());
    }
    float value = 0.0F;
    std::memcpy(&value, bytes.data(), sizeof(value));
    return static_cast<double>(value);
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

    std::vector<unsigned char> encoded(static_cast<std::size_t>(componentBytes));
    constexpr std::size_t cancellationChunkBytes = 1024U * 1024U;
    std::size_t bytesCompleted = 0;
    while (bytesCompleted < encoded.size()) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto chunk = std::min(
            cancellationChunkBytes, encoded.size() - bytesCompleted);
        input.read(reinterpret_cast<char*>(encoded.data() + bytesCompleted),
            static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            throw BlockReadError("FAB component payload is truncated");
        }
        bytesCompleted += chunk;
    }

    auto block = std::make_shared<FabBlock>();
    block->box = header.box;
    block->field = request.field;
    block->component = 0;
    const auto decodeValues = [&]<typename Value>() {
        std::vector<Value> values(static_cast<std::size_t>(valuesPerComponent));
        for (std::size_t value = 0; value < values.size(); ++value) {
            if ((value & 4095U) == 0U && cancellation.stop_requested()) {
                throw ReadCancelled();
            }
            values[value] = static_cast<Value>(decodeReal(
                encoded.data() + value * header.encoding.bytes, header.encoding));
        }
        return FabValues{std::move(values)};
    };
    block->values = header.encoding.bytes == sizeof(float)
        ? decodeValues.template operator()<float>()
        : decodeValues.template operator()<double>();

    return {
        std::shared_ptr<const FabBlock>(std::move(block)),
        BlockReadMetrics{1, header.headerBytes + componentBytes, valuesPerComponent}
    };
}

} // namespace amrvis
