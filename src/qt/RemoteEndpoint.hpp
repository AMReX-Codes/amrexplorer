#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace amrvis::qt {

struct RemoteEndpoint {
    std::string host;
    std::uint16_t port = 0;
    // Access token, empty when the input carried none (the caller then prompts
    // for it separately). An inline token may be appended as HOST:PORT#TOKEN.
    std::string token;
};

inline std::optional<RemoteEndpoint> parseRemoteEndpoint(std::string_view text)
{
    // Peel an optional inline token off the end first, so a token that happens
    // to contain ':' cannot confuse the host/port split below.
    std::string token;
    std::string_view hostPort = text;
    if (const auto hash = text.find('#'); hash != std::string_view::npos) {
        auto tokenView = text.substr(hash + 1);
        while (!tokenView.empty()
            && (tokenView.front() == ' ' || tokenView.front() == '\t')) {
            tokenView.remove_prefix(1);
        }
        while (!tokenView.empty()
            && (tokenView.back() == ' ' || tokenView.back() == '\t')) {
            tokenView.remove_suffix(1);
        }
        token = std::string(tokenView);
        hostPort = text.substr(0, hash);
    }
    while (!hostPort.empty()
        && (hostPort.back() == ' ' || hostPort.back() == '\t')) {
        hostPort.remove_suffix(1);
    }
    while (!hostPort.empty()
        && (hostPort.front() == ' ' || hostPort.front() == '\t')) {
        hostPort.remove_prefix(1);
    }

    std::size_t separator = std::string_view::npos;
    std::string_view host;
    if (!hostPort.empty() && hostPort.front() == '[') {
        const auto close = hostPort.find(']');
        if (close == std::string_view::npos || close == 1
            || close + 1 >= hostPort.size() || hostPort[close + 1] != ':') {
            return std::nullopt;
        }
        host = hostPort.substr(1, close - 1);
        separator = close + 1;
    } else {
        separator = hostPort.find(':');
        if (separator != hostPort.rfind(':')
            || hostPort.find_first_of("[]") != std::string_view::npos) {
            // Bare IPv6 is ambiguous with the port separator, and brackets
            // are valid only around a complete IPv6 host.
            return std::nullopt;
        }
        host = hostPort.substr(0, separator);
    }
    if (separator == std::string_view::npos || separator == 0
        || separator + 1 == hostPort.size()) {
        return std::nullopt;
    }
    if (host.empty()) {
        return std::nullopt;
    }
    unsigned int port = 0;
    const auto portText = hostPort.substr(separator + 1);
    const auto [end, error] = std::from_chars(
        portText.data(), portText.data() + portText.size(), port);
    if (error != std::errc{} || end != portText.data() + portText.size()
        || port == 0 || port > 65535) {
        return std::nullopt;
    }
    return RemoteEndpoint{std::string(host),
        static_cast<std::uint16_t>(port), std::move(token)};
}

} // namespace amrvis::qt
