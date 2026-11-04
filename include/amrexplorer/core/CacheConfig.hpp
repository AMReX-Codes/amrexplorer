#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

// The block-cache budget a dataset opens with when its caller names none.
// Qt-free so the viewer and the Python binding share one definition: both read
// AMREXPLORER_CACHE_SIZE_MB, and both fall back to the same 1 GiB. They differ
// only in what an unusable value does -- the viewer opens anyway, with the
// default, while an unattended analysis job refuses to run with a budget it
// did not ask for (see parseCacheSizeMegabytes).

namespace amrvis {

inline constexpr std::uint64_t defaultInitialCacheBudget
    = 1ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::string_view cacheSizeEnvironmentVariable
    = "AMREXPLORER_CACHE_SIZE_MB";
inline constexpr std::uint64_t bytesPerMegabyte = 1024ULL * 1024ULL;

// The budget in bytes that a value of AMREXPLORER_CACHE_SIZE_MB names, or
// nullopt when it names none: not a plain decimal count, zero, or too many
// megabytes for a byte count to hold.
[[nodiscard]] inline std::optional<std::uint64_t> parseCacheSizeMegabytes(
    std::string_view text) noexcept
{
    std::uint64_t megabytes = 0;
    const auto [end, error]
        = std::from_chars(text.data(), text.data() + text.size(), megabytes);
    if (error != std::errc{} || end != text.data() + text.size()
        || megabytes == 0
        || megabytes > std::numeric_limits<std::uint64_t>::max() / bytesPerMegabyte) {
        return std::nullopt;
    }
    return megabytes * bytesPerMegabyte;
}

inline std::uint64_t initialCacheBudget(const char* configuredValue) noexcept
{
    if (configuredValue == nullptr) {
        return defaultInitialCacheBudget;
    }
    return parseCacheSizeMegabytes(configuredValue)
        .value_or(defaultInitialCacheBudget);
}

// The environment variable's value, or nullopt when it is unset.
[[nodiscard]] inline std::optional<std::string_view>
cacheSizeEnvironmentValue() noexcept
{
#if defined(_MSC_VER)
    // _dupenv_s allocates; keep the copy for the process's lifetime so the
    // view stays valid. Read once: the variable is configuration, not state.
    static const char* const value = [] {
        char* configuredValue = nullptr;
        std::size_t configuredValueSize = 0;
        if (::_dupenv_s(&configuredValue, &configuredValueSize,
                cacheSizeEnvironmentVariable.data())
            != 0) {
            return static_cast<const char*>(nullptr);
        }
        return static_cast<const char*>(configuredValue);
    }();
#else
    const char* const value = std::getenv(cacheSizeEnvironmentVariable.data());
#endif
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string_view(value);
}

inline std::uint64_t initialCacheBudget() noexcept
{
    const auto configured = cacheSizeEnvironmentValue();
    if (!configured) {
        return defaultInitialCacheBudget;
    }
    return parseCacheSizeMegabytes(*configured)
        .value_or(defaultInitialCacheBudget);
}

} // namespace amrvis
