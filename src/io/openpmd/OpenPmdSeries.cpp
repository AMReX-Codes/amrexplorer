#include "OpenPmdSeries.hpp"

#include <map>
#include <string>

namespace amrvis {

// A staggered vector's components have different extents (E_x has one more
// sample along x than E_y), which openPMD-api refuses to read by default.
// Iterations are parsed when a source opens one rather than all at once, so
// opening a series of many iterations costs the one it shows.
OpenPmdSeries::OpenPmdSeries(const std::filesystem::path& path)
    : series(path.string(), openPMD::Access::READ_ONLY,
          R"({"verify_homogeneous_extents": false,)"
          R"( "defer_iteration_parsing": true})")
{
}

std::shared_ptr<OpenPmdSeries> openSeries(const std::filesystem::path& path)
{
    // "series.bp/" and "series.bp" are the same series.
    auto key = path.lexically_normal();
    if (key.filename().empty()) {
        key = key.parent_path();
    }
    static std::mutex registryMutex;
    static std::map<std::string, std::weak_ptr<OpenPmdSeries>> registry;
    // Held across the open, so two frames of one series asking at once share
    // a single open rather than racing to make two.
    std::scoped_lock lock(registryMutex);
    std::erase_if(registry,
        [](const auto& entry) { return entry.second.expired(); });
    if (auto open = registry[key.string()].lock()) {
        return open;
    }
    auto opened = std::make_shared<OpenPmdSeries>(path);
    registry[key.string()] = opened;
    return opened;
}

} // namespace amrvis
