#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace amrvis::qt {

inline std::optional<std::pair<std::string, std::uint16_t>>
parseRemoteEndpoint(std::string_view text)
{
    std::size_t separator = std::string_view::npos;
    std::string_view host;
    if (!text.empty() && text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string_view::npos || close == 1
            || close + 1 >= text.size() || text[close + 1] != ':') {
            return std::nullopt;
        }
        host = text.substr(1, close - 1);
        separator = close + 1;
    } else {
        separator = text.find(':');
        if (separator != text.rfind(':')) {
            // A bare IPv6 literal is ambiguous with the port separator.
            return std::nullopt;
        }
        host = text.substr(0, separator);
    }
    if (separator == std::string_view::npos || separator == 0
        || separator + 1 == text.size()) {
        return std::nullopt;
    }
    if (host.empty()) {
        return std::nullopt;
    }
    unsigned int port = 0;
    const auto portText = text.substr(separator + 1);
    const auto [end, error] = std::from_chars(
        portText.data(), portText.data() + portText.size(), port);
    if (error != std::errc{} || end != portText.data() + portText.size()
        || port == 0 || port > 65535) {
        return std::nullopt;
    }
    return std::pair{std::string(host), static_cast<std::uint16_t>(port)};
}

} // namespace amrvis::qt
