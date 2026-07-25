#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <system_error>

namespace amrvis::qt {

inline constexpr std::uint64_t defaultInitialCacheBudget
    = 1ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::string_view cacheSizeEnvironmentVariable
    = "AMREXPLORER_CACHE_SIZE_MB";
inline constexpr std::uint64_t bytesPerMegabyte = 1024ULL * 1024ULL;

inline std::uint64_t initialCacheBudget(const char* configuredValue) noexcept
{
    if (configuredValue == nullptr) {
        return defaultInitialCacheBudget;
    }

    const std::string_view text(configuredValue);
    std::uint64_t megabytes = 0;
    const auto [end, error]
        = std::from_chars(text.data(), text.data() + text.size(), megabytes);
    if (error != std::errc{} || end != text.data() + text.size()
        || megabytes == 0
        || megabytes > std::numeric_limits<std::uint64_t>::max() / bytesPerMegabyte) {
        return defaultInitialCacheBudget;
    }
    return megabytes * bytesPerMegabyte;
}

inline std::uint64_t initialCacheBudget() noexcept
{
#if defined(_MSC_VER)
    char* configuredValue = nullptr;
    std::size_t configuredValueSize = 0;
    const auto readError = ::_dupenv_s(&configuredValue,
        &configuredValueSize, cacheSizeEnvironmentVariable.data());
    const auto budget = readError == 0
        ? initialCacheBudget(configuredValue) : defaultInitialCacheBudget;
    std::free(configuredValue);
    return budget;
#else
    return initialCacheBudget(
        std::getenv(cacheSizeEnvironmentVariable.data()));
#endif
}

} // namespace amrvis::qt
