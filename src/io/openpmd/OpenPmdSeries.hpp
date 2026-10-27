#pragma once

// Private to amrexplorer_io, like OpenPmdSource.hpp.

#include <openPMD/openPMD.hpp>

#include <filesystem>
#include <memory>
#include <mutex>

namespace amrvis {

// One open openPMD series, shared by every OpenPmdSource reading it: each
// frame of a series sequence is a source of its own, and without this each
// would open (and parse) the whole series again. Every use of `series` holds
// `mutex`, since openPMD-api is not thread-safe and a frame being prefetched
// reads it while the one on screen does.
struct OpenPmdSeries {
    explicit OpenPmdSeries(const std::filesystem::path& path);

    std::mutex mutex;
    openPMD::Series series;
};

// The series at `path`: the one already open while any source still holds
// it, else a newly opened one. Held weakly here, so a series nobody reads is
// closed rather than kept -- the next open sees the files as they are then,
// which matters for a series a running simulation is still writing.
[[nodiscard]] std::shared_ptr<OpenPmdSeries> openSeries(
    const std::filesystem::path& path);

} // namespace amrvis
