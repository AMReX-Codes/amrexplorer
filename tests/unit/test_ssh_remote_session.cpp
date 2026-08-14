#include "SshRemoteSession.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    using amrvis::qt::parseSshServerReadyLine;
    using amrvis::qt::sshAskpassEnvironment;
    using amrvis::qt::sshLocalForwardBindFailed;
    using amrvis::qt::sshRemoteProcessArguments;
    using amrvis::qt::sshRemoteServerCommand;

    const auto ready = parseSshServerReadyLine("LISTENING 127.0.0.1 41419 TOKEN secret-token\r");
    require(ready && ready->port == 41419 && ready->token == "secret-token",
            "valid server startup line was not parsed");
    require(!parseSshServerReadyLine("LISTENING 0.0.0.0 41419 TOKEN secret-token") &&
                !parseSshServerReadyLine("LISTENING 127.0.0.1 0 TOKEN secret-token") &&
                !parseSshServerReadyLine("LISTENING 127.0.0.1 41419 TOKEN secret token"),
            "unsafe or malformed server startup line was accepted");

    const auto command =
        sshRemoteServerCommand("/opt/AMReXplorer's build/bin/amrexplorer-server", 41419);
    require(command &&
                *command ==
                    "exec '/opt/AMReXplorer'\\''s build/bin/amrexplorer-server' --port 41419",
            "remote executable path and fixed tunnel port were not encoded safely");
    const auto homeCommand =
        sshRemoteServerCommand("~/AMReXplorer build/amrexplorer-server", 49152);
    require(homeCommand &&
                *homeCommand ==
                    "exec \"$HOME\"/'AMReXplorer build/amrexplorer-server' --port 49152",
            "remote home-relative executable path was not expanded safely");
    require(!sshRemoteServerCommand("", 41419), "empty remote executable path was accepted");
    require(!sshRemoteServerCommand("amrexplorer-server", 0), "zero remote port was accepted");
    require(sshRemoteServerCommand("~", 41419) ==
                std::optional<std::string>{"exec \"$HOME\" --port 41419"},
            "remote home executable did not retain the fixed tunnel port");

    const auto sshArguments =
        sshRemoteProcessArguments("frontier", "~/bin/amrexplorer-server", 41419);
    require(sshArguments &&
                *sshArguments ==
                    QStringList{QStringLiteral("-T"), QStringLiteral("-o"),
                                QStringLiteral("ExitOnForwardFailure=yes"), QStringLiteral("-L"),
                                QStringLiteral("127.0.0.1:41419:127.0.0.1:41419"),
                                QStringLiteral("--"), QStringLiteral("frontier"),
                                QStringLiteral("exec \"$HOME\"/'bin/amrexplorer-server' --port 41419")},
            "one SSH invocation did not contain both the tunnel and server command");

    const auto environment = sshAskpassEnvironment(QStringLiteral("/Applications/AMReXplorer"));
    require(environment.value(QStringLiteral("SSH_ASKPASS")) ==
                    QStringLiteral("/Applications/AMReXplorer") &&
                environment.value(QStringLiteral("SSH_ASKPASS_REQUIRE")) ==
                    QStringLiteral("force") &&
                environment.value(QStringLiteral("AMREXPLORER_SSH_ASKPASS_MODE")) ==
                    QStringLiteral("1"),
            "SSH process environment does not force the in-app askpass helper");

    require(sshLocalForwardBindFailed(
                QStringLiteral("bind [127.0.0.1]:41419: Address already in use\n"
                               "channel_setup_fwd_listener_tcpip: cannot listen to port: 41419\n"
                               "Could not request local forwarding.\n")),
            "a failed local forward bind was not detected");
    require(
        !sshLocalForwardBindFailed(QStringLiteral("user@host: Permission denied (publickey).\n")),
        "an authentication failure was misattributed to the local forward");
    require(!sshLocalForwardBindFailed(QString()),
            "empty ssh output was misattributed to the local forward");

    return 0;
}
