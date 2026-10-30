#include "CacheConfig.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using amrvis::qt::defaultInitialCacheBudget;
    using amrvis::qt::initialCacheBudget;

    require(initialCacheBudget() == 768ULL * 1024ULL * 1024ULL,
        "cache size environment variable was not read");

    require(initialCacheBudget(nullptr) == defaultInitialCacheBudget,
        "unset cache size did not use the default");
    require(initialCacheBudget("2048") == 2048ULL * 1024ULL * 1024ULL,
        "valid cache size was not parsed");
    require(initialCacheBudget("1") == 1024ULL * 1024ULL,
        "minimum positive cache size was not accepted");
    require(initialCacheBudget("0") == defaultInitialCacheBudget,
        "zero cache size did not use the default");
    require(initialCacheBudget("-1") == defaultInitialCacheBudget,
        "negative cache size did not use the default");
    require(initialCacheBudget("1 MiB") == defaultInitialCacheBudget,
        "cache size with trailing text did not use the default");
    require(initialCacheBudget("") == defaultInitialCacheBudget,
        "empty cache size did not use the default");

    const auto maximumMegabytes
        = std::numeric_limits<std::uint64_t>::max() / (1024ULL * 1024ULL);
    const auto maximum = std::to_string(maximumMegabytes);
    require(initialCacheBudget(maximum.c_str())
            == maximumMegabytes * 1024ULL * 1024ULL,
        "maximum cache size in megabytes was not accepted");
    const auto overflow = std::to_string(maximumMegabytes + 1);
    require(initialCacheBudget(overflow.c_str()) == defaultInitialCacheBudget,
        "overflowing cache size did not use the default");

    // The strict parse the analysis binding uses: the same accepted values,
    // and no default for the rejected ones -- it raises instead.
    using amrvis::parseCacheSizeMegabytes;
    require(parseCacheSizeMegabytes("2048") == 2048ULL * 1024ULL * 1024ULL,
        "strict parse rejected a valid cache size");
    require(!parseCacheSizeMegabytes("0"), "strict parse accepted zero");
    require(!parseCacheSizeMegabytes(""), "strict parse accepted empty text");
    require(!parseCacheSizeMegabytes("12x"),
        "strict parse accepted trailing text");
    require(!parseCacheSizeMegabytes(overflow), "strict parse accepted overflow");
    require(amrvis::cacheSizeEnvironmentValue() == std::string_view("768"),
        "the environment value was not read");
}
