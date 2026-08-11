#pragma once

// Shared low-level parsing for AMReX FAB and plotfile headers. These helpers
// were previously duplicated (with drifting strictness) across the four
// readers; this is the one strict definition. Each reader reports failures
// through its own public error type (MetadataReadError, BlockReadError), so
// every throwing helper is templated on that error type — call sites choose
// the exception their callers and tests already expect.

#include <amrexplorer/core/Geometry.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace amrvis::detail {

// A FAB header is one line of text at the head of an otherwise binary file, so
// it is read from a stream that has no idea where the text ends. Real headers
// are a few hundred bytes -- "FAB " plus a RealDescriptor, a Box, and a
// component count -- and this ceiling leaves two orders of magnitude of room.
inline constexpr std::size_t maximumFabHeaderLineBytes = 16U * 1024U;

// std::getline with a ceiling, and otherwise its semantics: false when nothing
// could be read, the line without its terminator otherwise, and the stream left
// positioned just past the newline. The ceiling is the point: a file that opens
// "FAB " and never supplies a newline makes plain getline accumulate the whole
// remaining file into the string before the parse can reject it. An overlong
// line is malformed by definition, so it throws the caller's error type rather
// than returning a truncated one that might parse.
template <typename Error>
[[nodiscard]] bool readBoundedLine(std::istream& input, std::string& line,
    std::size_t limit = maximumFabHeaderLineBytes)
{
    line.clear();
    for (;;) {
        const auto character = input.get();
        if (character == std::char_traits<char>::eof()) {
            return !line.empty();
        }
        if (character == '\n') {
            return true;
        }
        if (line.size() >= limit) {
            throw Error("FAB header line exceeds the supported length");
        }
        line.push_back(static_cast<char>(character));
    }
}

// Every integer in the text, in order; any non-numeric characters act as
// separators. Never throws — callers validate the count/shape themselves.
[[nodiscard]] inline std::vector<int> parseIntegers(const std::string& text)
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

// One past the ')' matching the '(' at start.
template <typename Error>
[[nodiscard]] std::size_t balancedExpressionEnd(
    const std::string& text, std::size_t start)
{
    if (start >= text.size() || text[start] != '(') {
        throw Error("expected a parenthesized FAB header expression");
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
    throw Error("unterminated FAB header expression");
}

// AMReX Box text "((lo...) (hi...) (type...))" with the dimension known.
template <typename Error>
[[nodiscard]] IntBox parseAmrexBox(const std::string& text, int dimension)
{
    const auto values = parseIntegers(text);
    if (values.size() != static_cast<std::size_t>(dimension * 3)) {
        throw Error("malformed AMReX Box in FAB header");
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

// AMReX Box text with the dimension inferred from the first tuple's length.
template <typename Error>
[[nodiscard]] IntBox parseAmrexBoxInferDimension(
    const std::string& text, int& dimension)
{
    const auto firstTuple = text.find('(', 1);
    const auto firstEnd = text.find(')', firstTuple);
    if (firstTuple == std::string::npos || firstEnd == std::string::npos) {
        throw Error("cannot infer FAB dimension");
    }
    dimension = static_cast<int>(
        parseIntegers(text.substr(firstTuple, firstEnd - firstTuple + 1))
            .size());
    if (dimension < 1 || dimension > 3) {
        throw Error("malformed AMReX Box in FAB header");
    }
    return parseAmrexBox<Error>(text, dimension);
}

// Parsed FAB RealDescriptor: IEEE-32 or IEEE-64 with a contiguous byte
// order. This is the strict parse (format-entry count, the full format
// tuple, and the byte-order permutation are all validated); the previously
// separate catalog-side variant skipped the byte-order check, so a
// descriptor could pass cataloging and fail at the first block read.
struct ParsedRealDescriptor {
    std::size_t bytes = 0;
    bool littleEndian = false;
};

template <typename Error>
[[nodiscard]] ParsedRealDescriptor parseRealDescriptor(
    const std::string& descriptor)
{
    const auto values = parseIntegers(descriptor);
    constexpr std::size_t formatCountIndex = 0;
    constexpr std::size_t formatStartIndex = 1;
    constexpr std::size_t formatEntries = 8;
    constexpr std::size_t orderCountIndex = formatStartIndex + formatEntries;
    if (values.size() <= orderCountIndex
        || values[formatCountIndex] != static_cast<int>(formatEntries)) {
        throw Error("malformed FAB RealDescriptor");
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
        throw Error("only IEEE-32 and IEEE-64 FAB data are supported");
    }

    if (values[orderCountIndex] != bytes
        || values.size() < orderCountIndex + 1 + static_cast<std::size_t>(bytes)) {
        throw Error("malformed FAB byte-order descriptor");
    }

    bool ascending = true;
    bool descending = true;
    for (int byte = 0; byte < bytes; ++byte) {
        const auto value = values[orderCountIndex + 1 + static_cast<std::size_t>(byte)];
        ascending = ascending && value == byte + 1;
        descending = descending && value == bytes - byte;
    }
    if (!ascending && !descending) {
        throw Error("unsupported non-contiguous FAB byte order");
    }
    return {static_cast<std::size_t>(bytes), descending};
}

// The box grown by the ghost width on every active axis, guarded against
// integer overflow (one previously duplicated copy did this arithmetic in
// plain int).
template <typename Error>
[[nodiscard]] IntBox grownBox(
    const IntBox& source, const Int3& ghost, int dimension)
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
            throw Error("ghost-grown FAB box exceeds supported integer range");
        }
        result.lower[i] = static_cast<int>(lower);
        result.upper[i] = static_cast<int>(upper);
    }
    return result;
}

} // namespace amrvis::detail
