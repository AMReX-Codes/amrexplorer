#pragma once

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

} // namespace detail

// A path as valid UTF-8 text, for messages. On Windows the name is UTF-8
// already. A POSIX name is bytes, and bytes that are not UTF-8 are written
// as \xNN escapes (Python's backslashreplace): an error message the Python
// binding cannot decode would turn into a UnicodeDecodeError and hide the
// error it describes.
[[nodiscard]] inline std::string pathText(const std::filesystem::path& path)
{
    const auto raw = pathString(path);
    std::string text;
    text.reserve(raw.size());
    std::size_t at = 0;
    while (at < raw.size()) {
        if (const auto length = detail::utf8SequenceLength(raw, at)) {
            text.append(raw, at, length);
            at += length;
            continue;
        }
        constexpr char digits[] = "0123456789abcdef";
        const auto value = static_cast<unsigned char>(raw[at]);
        text += "\\x";
        text += digits[value >> 4];
        text += digits[value & 0xF];
        ++at;
    }
    return text;
}

} // namespace amrvis
