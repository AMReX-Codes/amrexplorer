#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace amrvis::qt {

struct SshConnectArguments {
    std::string destination;
    // Empty when --server was not given: the client then uses the executable
    // last used for this destination, else "amrexplorer-server" from the
    // remote PATH.
    std::string serverExecutable;
    std::vector<std::string> paths;
    // Empty when --companion was not given: a remote plotfile to show beside
    // the one path, once its slices are up.
    std::string companion;
};

struct SshConnectParseResult {
    std::optional<SshConnectArguments> request;
    std::string error;
};

// After the destination and the optional --server PATH, every token is a
// remote path -- except "--companion PATH", and one that starts with '-',
// which is an option this program does not have (a mistyped --server, say)
// and is refused rather than sent to the server as a path. A path that
// really starts with '-' goes after a "--" token.
inline SshConnectParseResult parseSshConnectArguments(std::span<const std::string_view> arguments) {
    constexpr std::string_view usage = "usage: amrexplorer --ssh SSH_DESTINATION [--server PATH] "
                                       "[--companion REMOTE_PATH] [--] [REMOTE_PATH ...]";
    if (arguments.empty() || arguments.front().empty()) {
        return {{}, std::string(usage)};
    }
    const auto destination = arguments.front();
    if (destination.front() == '-' ||
        destination.find_first_of(" \t\r\n") != std::string_view::npos) {
        return {{}, "invalid SSH destination"};
    }
    SshConnectArguments request;
    request.destination = destination;
    std::size_t pathBegin = 1;
    if (arguments.size() >= 2 && arguments[1] == "--server") {
        if (arguments.size() < 3 || arguments[2].empty()) {
            return {{}, std::string(usage)};
        }
        request.serverExecutable = arguments[2];
        pathBegin = 3;
    }
    request.paths.reserve(arguments.size() - pathBegin);
    bool optionsEnded = false;
    const auto rest = arguments.subspan(pathBegin);
    for (std::size_t index = 0; index < rest.size(); ++index) {
        const auto path = rest[index];
        if (path.empty()) {
            return {{}, "remote paths must not be empty"};
        }
        if (!optionsEnded && path == "--") {
            optionsEnded = true;
            continue;
        }
        if (!optionsEnded && path == "--companion") {
            if (index + 1 >= rest.size() || rest[index + 1].empty()) {
                return {{}, "--companion requires a remote plotfile path\n" + std::string(usage)};
            }
            if (!request.companion.empty()) {
                return {{}, "--companion given twice\n" + std::string(usage)};
            }
            request.companion = rest[++index];
            continue;
        }
        if (!optionsEnded && path.front() == '-') {
            return {{}, "unknown option: " + std::string(path) + "\n" + std::string(usage)};
        }
        request.paths.emplace_back(path);
    }
    if (!request.companion.empty() && request.paths.size() != 1) {
        return {{}, "--companion needs exactly one remote plotfile to pair with\n" + std::string(usage)};
    }
    return {std::move(request), {}};
}

} // namespace amrvis::qt
