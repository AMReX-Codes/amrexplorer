#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>

namespace amrvis {

// What an open or a sequence frame names: a plotfile, MultiFab or FAB by its
// path, or one iteration of an openPMD series -- the series' path and the
// iteration within it. A plotfile has no iteration, and a series opened
// without one shows its first.
//
// Converts implicitly from a path, so everything that opens a plotfile by
// path names a location without saying so.
struct DatasetLocation {
    DatasetLocation() = default;
    DatasetLocation(std::filesystem::path where,
        std::optional<std::uint64_t> which = std::nullopt)
        : path(std::move(where))
        , iteration(which)
    {
    }

    std::filesystem::path path;
    std::optional<std::uint64_t> iteration;

    friend bool operator==(const DatasetLocation&, const DatasetLocation&)
        = default;
};

} // namespace amrvis
