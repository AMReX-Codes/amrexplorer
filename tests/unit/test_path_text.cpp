#include <amrexplorer/core/PathText.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

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

    // utf8Text and isUtf8 on bytes, as header text arrives.
    require(amrvis::utf8Text("plain/\xc3\xa9") == "plain/\xc3\xa9",
        "utf8Text changed valid UTF-8");
    require(amrvis::utf8Text("/\xe9tc") == "/\\xe9tc",
        "utf8Text did not escape a stray byte");
    require(amrvis::isUtf8("Level_0/Cell\xc3\xa9"), "isUtf8 refused UTF-8");
    require(!amrvis::isUtf8("/\xe9tc"), "isUtf8 accepted a stray byte");

    // UTF-16 as Windows names are: a pair is one code point, and a lone
    // surrogate is escaped rather than thrown on.
    using amrvis::detail::utf16Text;
    require(utf16Text(std::u16string_view(u"aé\U0001F30C"))
            == "a\xc3\xa9\xf0\x9f\x8c\x8c",
        "utf16Text mis-encoded a well-formed name");
    const char16_t lone[] = {u'a', 0xD83C, u'b', 0xDF0C, 0};
    require(utf16Text(std::u16string_view(lone)) == "a\\ud83cb\\udf0c",
        "utf16Text did not escape lone surrogates");
    const char16_t trailing[] = {u'x', 0xD800, 0};
    require(utf16Text(std::u16string_view(trailing)) == "x\\ud800",
        "utf16Text did not escape a surrogate at the end");
    return 0;
}
