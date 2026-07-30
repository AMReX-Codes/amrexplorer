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
    const auto separator = text.rfind(':');
    if (separator == std::string_view::npos || separator == 0
        || separator + 1 == text.size()) {
        return std::nullopt;
    }
    auto host = text.substr(0, separator);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host.remove_prefix(1);
        host.remove_suffix(1);
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
