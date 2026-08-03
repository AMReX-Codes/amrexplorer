#include "RemoteConnectArguments.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <sstream>
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
    using amrvis::qt::parseRemoteConnectArguments;

    std::istringstream tokenInput("secret-token\n");
    constexpr std::array<std::string_view, 4> arguments{
        "127.0.0.1:48192", "--token-stdin", "/remote/plt00010",
        "/remote/plt00020"};
    const auto parsed = parseRemoteConnectArguments(arguments, tokenInput);
    require(parsed.request.has_value() && parsed.error.empty(),
        "token-stdin connection arguments were rejected");
    require(parsed.request->endpoint.host == "127.0.0.1"
            && parsed.request->endpoint.port == 48192
            && parsed.request->endpoint.token == "secret-token",
        "token-stdin connection arguments were parsed incorrectly");
    require(parsed.request->paths.size() == 2,
        "remote sequence paths were not preserved");

    std::istringstream unusedInput;
    constexpr std::array<std::string_view, 3> inlineToken{
        "127.0.0.1:48192#secret-token", "--token-stdin",
        "/remote/plt00010"};
    const auto rejected
        = parseRemoteConnectArguments(inlineToken, unusedInput);
    require(!rejected.request && rejected.error.find("argv") != std::string::npos,
        "inline session token remained accepted in process arguments");

    std::istringstream emptyInput;
    constexpr std::array<std::string_view, 3> emptyToken{
        "127.0.0.1:48192", "--token-stdin", "/remote/plt00010"};
    const auto missing = parseRemoteConnectArguments(emptyToken, emptyInput);
    require(!missing.request && missing.error.find("standard input")
            != std::string::npos,
        "missing standard-input token was accepted");
    return 0;
}
