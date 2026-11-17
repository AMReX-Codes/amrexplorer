#pragma once

#include <filesystem>
#include <string>

namespace amrvis {

// A path as UTF-8 text, for messages. path::string() converts through the
// ANSI code page on Windows: a name outside it throws std::system_error, and
// one inside it comes back in that code page rather than UTF-8. Either way an
// error about a file would turn into a different error, or into bytes the
// Python binding cannot decode.
[[nodiscard]] inline std::string pathText(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

} // namespace amrvis
