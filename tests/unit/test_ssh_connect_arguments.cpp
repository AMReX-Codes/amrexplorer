#include "SshConnectArguments.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
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
    constexpr std::array<std::string_view, 2> sshDefault{
        "frontier", "/remote/plt00010"};
    const auto defaultServer
        = amrvis::qt::parseSshConnectArguments(sshDefault);
    require(defaultServer.request
            && defaultServer.request->destination == "frontier"
            && defaultServer.request->serverExecutable.empty()
            && defaultServer.request->paths.size() == 1,
        "SSH config alias arguments were not accepted");

    constexpr std::array<std::string_view, 5> sshExplicit{
        "user@frontier", "--server", "/opt/amrex explorer/bin/server",
        "/remote/plt00010", "/remote/plt00020"};
    const auto explicitServer
        = amrvis::qt::parseSshConnectArguments(sshExplicit);
    require(explicitServer.request
            && explicitServer.request->serverExecutable
                == "/opt/amrex explorer/bin/server"
            && explicitServer.request->paths.size() == 2,
        "explicit remote server executable was not preserved");

    // A destination alone establishes the session without opening anything.
    constexpr std::array<std::string_view, 1> sessionOnly{"frontier"};
    const auto session = amrvis::qt::parseSshConnectArguments(sessionOnly);
    require(session.request && session.request->paths.empty(),
        "session-only arguments were not accepted");
    constexpr std::array<std::string_view, 3> sessionOnlyServer{
        "frontier", "--server", "~/bin/amrexplorer-server"};
    const auto sessionServer
        = amrvis::qt::parseSshConnectArguments(sessionOnlyServer);
    require(sessionServer.request && sessionServer.request->paths.empty()
            && sessionServer.request->serverExecutable
                == "~/bin/amrexplorer-server",
        "session-only arguments with --server were not accepted");

    constexpr std::array<std::string_view, 2> optionDestination{
        "-oProxyCommand=bad", "/remote/plt00010"};
    require(!amrvis::qt::parseSshConnectArguments(optionDestination).request,
        "option-like SSH destination was accepted");
    constexpr std::array<std::string_view, 2> missingServer{
        "frontier", "--server"};
    require(!amrvis::qt::parseSshConnectArguments(missingServer).request,
        "--server without a value was accepted");
    constexpr std::array<std::string_view, 2> emptyPath{"frontier", ""};
    require(!amrvis::qt::parseSshConnectArguments(emptyPath).request,
        "empty remote path was accepted");
    // A mistyped option is refused, not sent to the server as a path; a path
    // that starts with '-' is spelled after "--".
    constexpr std::array<std::string_view, 3> mistypedOption{
        "frontier", "--sever", "/remote/plt00010"};
    const auto mistyped = amrvis::qt::parseSshConnectArguments(mistypedOption);
    require(!mistyped.request
            && mistyped.error.find("unknown option: --sever") == 0,
        "an option-like remote path was accepted");
    constexpr std::array<std::string_view, 5> dashPaths{"frontier", "--server",
        "~/bin/amrexplorer-server", "--", "-plt00010"};
    const auto dashed = amrvis::qt::parseSshConnectArguments(dashPaths);
    require(dashed.request && dashed.request->paths.size() == 1
            && dashed.request->paths.front() == "-plt00010",
        "a dash-prefixed path after -- was not accepted");
    constexpr std::array<std::string_view, 3> lateServer{
        "frontier", "/remote/plt00010", "--server"};
    require(!amrvis::qt::parseSshConnectArguments(lateServer).request,
        "--server after a path was accepted as a path");
    require(!amrvis::qt::parseSshConnectArguments({}).request,
        "empty argument list was accepted");
    return 0;
}
