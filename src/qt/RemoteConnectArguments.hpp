#pragma once

#include "RemoteEndpoint.hpp"

#include <istream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amrvis::qt {

struct RemoteConnectArguments {
    RemoteEndpoint endpoint;
    std::vector<std::string> paths;
};

struct RemoteConnectParseResult {
    std::optional<RemoteConnectArguments> request;
    std::string error;
};

inline RemoteConnectParseResult parseRemoteConnectArguments(
    std::span<const std::string_view> arguments, std::istream& tokenInput)
{
    constexpr std::string_view usage
        = "usage: amrexplorer --connect HOST:PORT --token-stdin "
          "REMOTE_PATH [REMOTE_PATH ...]";
    if (arguments.size() < 3 || arguments[1] != "--token-stdin") {
        return {{}, std::string(usage)};
    }
    auto endpoint = parseRemoteEndpoint(arguments[0]);
    if (!endpoint) {
        return {{}, "invalid remote endpoint; expected numeric IPv4:PORT or "
                    "[IPv6]:PORT"};
    }
    if (!endpoint->token.empty()) {
        return {{}, "session token must not be supplied in argv; use "
                    "--token-stdin"};
    }
    std::string token;
    if (!std::getline(tokenInput, token)) {
        return {{}, "missing session token on standard input"};
    }
    if (!token.empty() && token.back() == '\r') {
        token.pop_back();
    }
    if (token.empty()) {
        return {{}, "missing session token on standard input"};
    }
    endpoint->token = std::move(token);

    RemoteConnectArguments request;
    request.endpoint = std::move(*endpoint);
    request.paths.reserve(arguments.size() - 2);
    for (const auto path : arguments.subspan(2)) {
        if (path.empty()) {
            return {{}, "remote paths must not be empty"};
        }
        request.paths.emplace_back(path);
    }
    return {std::move(request), {}};
}

} // namespace amrvis::qt
