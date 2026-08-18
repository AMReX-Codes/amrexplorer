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
};

struct SshConnectParseResult {
    std::optional<SshConnectArguments> request;
    std::string error;
};

// After the destination and the optional --server PATH, every token is a
// remote path -- except one that starts with '-', which is an option this
// program does not have (a mistyped --server, say) and is refused rather
// than sent to the server as a path. A path that really starts with '-'
// goes after a "--" token.
inline SshConnectParseResult parseSshConnectArguments(std::span<const std::string_view> arguments) {
    constexpr std::string_view usage = "usage: amrexplorer --ssh SSH_DESTINATION [--server PATH] "
                                       "[--] [REMOTE_PATH ...]";
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
    for (const auto path : arguments.subspan(pathBegin)) {
        if (path.empty()) {
            return {{}, "remote paths must not be empty"};
        }
        if (!optionsEnded && path == "--") {
            optionsEnded = true;
            continue;
        }
        if (!optionsEnded && path.front() == '-') {
            return {{}, "unknown option: " + std::string(path) + "\n" + std::string(usage)};
        }
        request.paths.emplace_back(path);
    }
    return {std::move(request), {}};
}

} // namespace amrvis::qt
