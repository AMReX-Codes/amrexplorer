#include <amrexplorer/core/PathText.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using amrvis::pathFromText;
    using amrvis::pathString;
    using amrvis::pathText;

    // UTF-8 names come back unchanged, and pathString round-trips.
    const std::string unicode = "donn\xc3\xa9" "es_\xe6\xb8\xac_\xf0\x9f\x8c\x8c";
    require(pathText(pathFromText(unicode)) == unicode,
        "pathText changed a UTF-8 name");
    require(pathString(pathFromText(unicode)) == unicode,
        "pathString did not round-trip a UTF-8 name");

#ifndef _WIN32
    // A POSIX name is bytes. pathString keeps them, so the file can be
    // opened again; pathText escapes the ones that are not UTF-8, so the
    // message stays decodable.
    const std::string latin1 = "donn\xe9" "es/plt";
    require(pathString(pathFromText(latin1)) == latin1,
        "pathString did not keep a non-UTF-8 name's bytes");
    require(pathText(std::filesystem::path(latin1)) == "donn\\xe9es/plt",
        "pathText did not escape a stray byte");
    // Truncated, overlong, surrogate and out-of-range sequences.
    require(pathText(std::filesystem::path("a\xc3")) == "a\\xc3",
        "pathText kept a truncated sequence");
    require(pathText(std::filesystem::path("\xc0\xaf")) == "\\xc0\\xaf",
        "pathText kept an overlong sequence");
    require(pathText(std::filesystem::path("\xed\xa0\x80")) == "\\xed\\xa0\\x80",
        "pathText kept a surrogate");
    require(pathText(std::filesystem::path("\xf4\x90\x80\x80"))
            == "\\xf4\\x90\\x80\\x80",
        "pathText kept a code point past U+10FFFF");
#endif
    return 0;
}
