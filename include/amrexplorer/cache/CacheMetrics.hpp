#pragma once

#include <cstdint>

namespace amrvis {

struct CacheMetrics {
    std::uint64_t budgetBytes = 0;
    std::uint64_t residentBytes = 0;
    std::uint64_t pinnedBytes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;

    [[nodiscard]] double hitRate() const noexcept;
    [[nodiscard]] bool withinBudget() const noexcept;
};

} // namespace amrvis

