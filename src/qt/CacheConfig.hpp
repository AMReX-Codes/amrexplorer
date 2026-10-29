#pragma once

// The viewer's initial cache budget now lives below Qt, shared with the
// analysis binding; these names keep the GUI's spelling of it.
#include <amrexplorer/core/CacheConfig.hpp>

namespace amrvis::qt {

using amrvis::bytesPerMegabyte;
using amrvis::cacheSizeEnvironmentVariable;
using amrvis::defaultInitialCacheBudget;
using amrvis::initialCacheBudget;

} // namespace amrvis::qt
