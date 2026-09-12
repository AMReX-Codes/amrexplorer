#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace amrvis::remote {

// Injectable OS inputs let Linux hierarchy handling be tested on every host.
struct MemoryLimitInputs {
    std::string cgroupMembership;
    std::string mountInfo;
    std::function<std::optional<std::string>(const std::string&)> readFile;
    std::function<std::optional<std::string>(const std::string&)> environment;
    std::optional<std::uint64_t> physicalBytes;
};
struct MemoryLimit {
    std::uint64_t bytes;
    std::string source;
};
[[nodiscard]] std::optional<MemoryLimit> detectMemoryLimit(const MemoryLimitInputs& inputs);
[[nodiscard]] std::optional<MemoryLimit> detectMemoryLimit();
[[nodiscard]] double parseCacheMemoryFraction(const std::string& text);
[[nodiscard]] std::uint64_t cacheBytesForFraction(std::uint64_t bytes, double fraction);

} // namespace amrvis::remote
