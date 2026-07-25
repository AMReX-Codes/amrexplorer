#include <amrexplorer/cache/CacheMetrics.hpp>

namespace amrvis {

double CacheMetrics::hitRate() const noexcept
{
    const auto accesses = hits + misses;
    return accesses == 0 ? 0.0
                         : static_cast<double>(hits) / static_cast<double>(accesses);
}

bool CacheMetrics::withinBudget() const noexcept
{
    return residentBytes <= budgetBytes;
}

} // namespace amrvis

