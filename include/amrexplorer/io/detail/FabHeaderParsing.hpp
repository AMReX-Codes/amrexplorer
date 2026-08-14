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
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <istream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace amrvis::detail {

// A header line is text at the head of a file the reader otherwise treats as
// opaque, so it is read from a stream that has no idea where the text ends.
// Real ones are a few hundred bytes -- a FAB's "FAB " plus a RealDescriptor, a
// Box and a component count; a plotfile's version string -- and this ceiling
// leaves two orders of magnitude of room. Named for headers generally, and now
// genuinely shared: the FAB path and the plotfile text readers both bound their
// lines here.
inline constexpr std::size_t maximumHeaderLineBytes = 16U * 1024U;

// The same idea for a whitespace-delimited token: a version string, a component
// name, a FAB filename. Extraction with >> is otherwise unbounded -- it reads to
// the next whitespace -- so one enormous whitespace-free run is allocated whole
// before any count or cap can reject it.
inline constexpr std::size_t maximumHeaderTokenBytes = 4U * 1024U;

// The largest speculative reserve any header parse may take. Beyond this the
// container simply grows as records are parsed, which costs amortized copying
// and nothing else -- so this is the point where an optimization stops being
// worth a crafted-input risk.
//
// It exists because the file's own size is *not* trustworthy evidence on its
// own: std::filesystem::file_size reports the apparent size, so `truncate -s
// 80M` on a short crafted header yields 80 MB of "evidence" while occupying
// one filesystem block, and a bound derived from it alone is forgeable at
// almost no cost. The size still tightens the bound for ordinary files; this
// cap is what makes it hold for hostile ones.
inline constexpr std::size_t maximumSpeculativeReserve = 64U * 1024U;

// How many entries to reserve for a count a header declares. A declared count
// is a claim; the file's size is evidence against it, since a header cannot
// describe more records than its bytes allow. Both are advisory, and the cap
// above is the backstop: every caller is *reserving*, never sizing, so
// under-reserving costs one reallocation while over-reserving is the whole
// attack. That asymmetry is also why the per-record floors callers pass sit
// below the true minimum rather than being tuned to it.
[[nodiscard]] inline std::size_t evidenceBoundedCount(std::uint64_t declared,
    std::uintmax_t fileBytes, std::uint64_t minimumBytesPerRecord)
{
    const auto describable
        = static_cast<std::uint64_t>(fileBytes) / minimumBytesPerRecord;
    return static_cast<std::size_t>(std::min({declared, describable,
        static_cast<std::uint64_t>(maximumSpeculativeReserve)}));
}

// std::getline with a ceiling, and otherwise its semantics: false when nothing
// could be read, the line without its terminator otherwise, and the stream left
// positioned just past the newline. The ceiling is the point: a file that opens
// "FAB " and never supplies a newline makes plain getline accumulate the whole
// remaining file into the string before the parse can reject it. An overlong
// line is malformed by definition, so it throws the caller's error type rather
// than returning a truncated one that might parse.
//
// Reads through the stream buffer rather than istream::get(), which builds a
// sentry per character. Measured over a 2.4 MB, 100,000-line Header: 10.1 ms
// through get(), 5.0 ms this way, against std::getline's 1.34 ms. Real Headers
// carry a handful of components, so the absolute cost is small either way --
// but the ceiling is the only reason to leave getline behind, and paying 7x
// for it was not part of that bargain. The remaining gap buys the bound.
//
// The stream-state handling is what makes this a drop-in, and it is written
// out rather than inherited because sbumpc sets no state of its own:
//   - nothing read at end of input: eofbit *and* failbit, as getline sets when
//     it extracts nothing, so a caller's `while (readBoundedLine(...))` ends;
//   - characters read, then end of input: eofbit only, so the final line of a
//     file with no trailing newline is returned rather than discarded;
//   - a stream that is not good() on entry: failbit, matching the sentry
//     get() would have constructed and failed.
template <typename Error>
[[nodiscard]] bool readBoundedLine(std::istream& input, std::string& line,
    std::size_t limit = maximumHeaderLineBytes)
{
    line.clear();
    if (!input.good()) {
        input.setstate(std::ios::failbit);
        return false;
    }
    auto* buffer = input.rdbuf();
    // An unformatted input function turns a streambuf exception into badbit
    // and rethrows only when the caller asked for it; sbumpc does neither, so
    // that translation is done here rather than quietly dropped. (A null rdbuf
    // needs no guard: both constructing a stream with one and swapping one in
    // set badbit, so good() is already false above -- verified, not assumed.)
    const auto next = [buffer, &input] {
        try {
            return buffer->sbumpc();
        } catch (...) {
            input.setstate(std::ios::badbit);
            throw;
        }
    };
    for (;;) {
        const auto character = next();
        if (character == std::char_traits<char>::eof()) {
            input.setstate(line.empty()
                    ? (std::ios::eofbit | std::ios::failbit)
                    : std::ios::eofbit);
            return !line.empty();
        }
        if (character == '\n') {
            return true;
        }
        if (line.size() >= limit) {
            throw Error("header line exceeds the supported length");
        }
        line.push_back(static_cast<char>(character));
    }
}

// operator>>(std::string) with a ceiling, and with the truncation hazard the
// ceiling introduces handled here once rather than at each call site.
//
// width() is how the standard bounds the extraction, but it does not fail it:
// >> sets failbit only when it extracts *nothing*, so an over-long token would
// leave its tail in the stream to be read as the next field, and every field
// after it would shift. That converts an out-of-memory failure into a silent
// misparse, which is worse. A token this long is malformed, so say so.
//
// Reaching the limit is not by itself proof of truncation, and the difference
// is what the stream position tells: what follows a complete token is
// whitespace or end of file, and anything else is the rest of a token that did
// not fit. Without that check a well-formed token of exactly the limit is
// refused along with the truncated ones.
template <typename Error>
[[nodiscard]] std::string readBoundedToken(std::istream& input,
    std::string_view subject, std::string_view description,
    std::size_t limit = maximumHeaderTokenBytes)
{
    // width() is an int, and operator>> reads any non-positive width as "no
    // width" -- unbounded extraction, with the length check below then dead.
    // Both ends therefore need clamping, not just the top: a limit past
    // INT_MAX narrows to zero or negative, and a limit of zero is already
    // there. Either way the helper would silently become the thing it exists
    // to prevent, so the applied width is what the check compares against.
    const auto effectiveLimit = std::clamp<std::size_t>(limit, 1,
        static_cast<std::size_t>(std::numeric_limits<int>::max()));
    std::string value;
    input >> std::setw(static_cast<int>(effectiveLimit)) >> value;
    if (!input) {
        throw Error("malformed " + std::string(subject) + " while reading "
            + std::string(description));
    }
    // good() is false when the extraction stopped at end of file, which is one
    // of the two ways a complete token ends -- and peeking a stream that is not
    // good() would set failbit on a stream the caller may still be reading.
    if (value.size() >= effectiveLimit && input.good()) {
        const auto next = input.peek();
        if (next != std::char_traits<char>::eof()
            && std::isspace(static_cast<unsigned char>(next)) == 0) {
            throw Error(std::string(subject) + " " + std::string(description)
                + " exceeds the supported length");
        }
    }
    return value;
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
