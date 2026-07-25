#pragma once

#include <cstdint>

namespace amrvis {

struct CacheMetrics {
    std::uint64_t budgetBytes = 0;
    std::uint64_t residentBytes = 0;
    std::uint64_t pinnedBytes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    // Capacity-driven evictions only (an entry dropped to make room). Entries
    // removed by an explicit erase()/clearUnpinned() are counted in `clears`
    // instead, so the eviction diagnostic is not inflated by a user-initiated
    // cache clear (e.g. the cache-pressure fallback).
    std::uint64_t evictions = 0;
    std::uint64_t clears = 0;

    [[nodiscard]] double hitRate() const noexcept;
    [[nodiscard]] bool withinBudget() const noexcept;
};

} // namespace amrvis

