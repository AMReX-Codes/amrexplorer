#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace amrvis {

// A path's name in a std::string that round-trips through pathFromText:
// UTF-8 on Windows, the native bytes elsewhere. path::string() converts
// through the ANSI code page on Windows, and a name outside it throws
// std::system_error, so names stored in metadata use this instead.
[[nodiscard]] inline std::string pathString(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// The path a pathString names. Constructing a path from a std::string reads
// it in the ANSI code page on Windows; this reads it as UTF-8 there, and
// keeps the bytes as they are on POSIX.
[[nodiscard]] inline std::filesystem::path pathFromText(std::string_view text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

namespace detail {

// The length of the well-formed UTF-8 sequence at `at`, or 0 when the bytes
// there are not one (truncated, overlong, a surrogate, or past U+10FFFF).
inline std::size_t utf8SequenceLength(std::string_view text, std::size_t at)
{
    const auto byte = [&](std::size_t i) {
        return static_cast<unsigned char>(text[i]);
    };
    const auto lead = byte(at);
    if (lead < 0x80) {
        return 1;
    }
    std::size_t length = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        low = lead == 0xE0 ? 0xA0 : 0x80;
        high = lead == 0xED ? 0x9F : 0xBF;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        low = lead == 0xF0 ? 0x90 : 0x80;
        high = lead == 0xF4 ? 0x8F : 0xBF;
    } else {
        return 0;
    }
    if (at + length > text.size()) {
        return 0;
    }
    if (byte(at + 1) < low || byte(at + 1) > high) {
        return 0;
    }
    for (std::size_t i = 2; i < length; ++i) {
        if (byte(at + i) < 0x80 || byte(at + i) > 0xBF) {
            return 0;
        }
    }
    return length;
}

// Appends code point `value` to `text` as UTF-8.
inline void appendUtf8(std::string& text, char32_t value)
{
    if (value < 0x80) {
        text += static_cast<char>(value);
    } else if (value < 0x800) {
        text += static_cast<char>(0xC0 | (value >> 6));
        text += static_cast<char>(0x80 | (value & 0x3F));
    } else if (value < 0x10000) {
        text += static_cast<char>(0xE0 | (value >> 12));
        text += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (value & 0x3F));
    } else {
        text += static_cast<char>(0xF0 | (value >> 18));
        text += static_cast<char>(0x80 | ((value >> 12) & 0x3F));
        text += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (value & 0x3F));
    }
}

// Appends `digits` lowercase hex digits of `value` after `prefix`.
inline void appendEscape(
    std::string& text, const char* prefix, unsigned value, int digits)
{
    constexpr char hex[] = "0123456789abcdef";
    text += prefix;
    for (int shift = 4 * (digits - 1); shift >= 0; shift -= 4) {
        text += hex[(value >> shift) & 0xF];
    }
}

// UTF-16 code units as UTF-8, a lone surrogate written as a \udXXX escape
// (Python's backslashreplace) rather than failing, as the standard
// conversion does. Windows names are UTF-16 that NTFS does not require to be
// well formed.
template <typename Unit>
std::string utf16Text(std::basic_string_view<Unit> units)
{
    static_assert(sizeof(Unit) == 2);
    std::string text;
    text.reserve(units.size());
    for (std::size_t at = 0; at < units.size(); ++at) {
        const auto unit = static_cast<char32_t>(
            static_cast<std::uint16_t>(units[at]));
        const bool high = unit >= 0xD800 && unit <= 0xDBFF;
        const bool low = unit >= 0xDC00 && unit <= 0xDFFF;
        if (high && at + 1 < units.size()) {
            const auto next = static_cast<char32_t>(
                static_cast<std::uint16_t>(units[at + 1]));
            if (next >= 0xDC00 && next <= 0xDFFF) {
                appendUtf8(text,
                    0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
                ++at;
                continue;
            }
        }
        if (high || low) {
            appendEscape(text, "\\u", static_cast<unsigned>(unit), 4);
            continue;
        }
        appendUtf8(text, unit);
    }
    return text;
}

} // namespace detail

// Bytes as valid UTF-8 text, for messages: well-formed UTF-8 is kept, and
// any other byte is written as a \xNN escape (Python's backslashreplace). An
// error message the Python binding cannot decode would turn into a
// UnicodeDecodeError and hide the error it describes.
[[nodiscard]] inline std::string utf8Text(std::string_view bytes)
{
    std::string text;
    text.reserve(bytes.size());
    std::size_t at = 0;
    while (at < bytes.size()) {
        if (const auto length = detail::utf8SequenceLength(bytes, at)) {
            text.append(bytes, at, length);
            at += length;
            continue;
        }
        detail::appendEscape(
            text, "\\x", static_cast<unsigned char>(bytes[at]), 2);
        ++at;
    }
    return text;
}

// Whether `bytes` are well-formed UTF-8.
[[nodiscard]] inline bool isUtf8(std::string_view bytes)
{
    std::size_t at = 0;
    while (at < bytes.size()) {
        const auto length = detail::utf8SequenceLength(bytes, at);
        if (length == 0) {
            return false;
        }
        at += length;
    }
    return true;
}

// A path as valid UTF-8 text, for messages and display. Never throws for a
// name the filesystem gave: a POSIX name's bytes go through utf8Text, and a
// Windows name's UTF-16 through detail::utf16Text, since path::u8string()
// throws there for a lone surrogate.
[[nodiscard]] inline std::string pathText(const std::filesystem::path& path)
{
#ifdef _WIN32
    return detail::utf16Text(std::wstring_view(path.native()));
#else
    return utf8Text(pathString(path));
#endif
}

} // namespace amrvis
