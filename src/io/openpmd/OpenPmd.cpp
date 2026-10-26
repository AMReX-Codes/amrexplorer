#include <amrexplorer/io/OpenPmd.hpp>

#if AMREXPLORER_HAS_OPENPMD
#include "OpenPmdSource.hpp"
#endif

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace amrvis {

bool isOpenPmdSeriesPath(const std::filesystem::path& path)
{
    // "series.bp/" names the ADIOS2 directory by its parent.
    auto name = path.filename();
    if (name.empty()) {
        name = path.parent_path().filename();
    }
    if (name.string().find("%T") != std::string::npos) {
        return true;
    }
    // The extensions openPMD-api picks a backend from.
    static constexpr std::array<std::string_view, 6> extensions{
        ".h5", ".bp", ".bp4", ".bp5", ".json", ".toml"};
    const auto extension = name.extension().string();
    return std::find(extensions.begin(), extensions.end(), extension)
        != extensions.end();
}

bool openPmdSupported() noexcept
{
#if AMREXPLORER_HAS_OPENPMD
    return true;
#else
    return false;
#endif
}

PlotfileMetadataResult readOpenPmdMetadata(
    const DatasetLocation& location, StopToken cancellation)
{
#if AMREXPLORER_HAS_OPENPMD
    auto source = std::make_shared<const OpenPmdSource>(
        location.path, location.iteration, cancellation);
    PlotfileMetadataResult result;
    result.metadata = source->metadata();
    result.metrics = source->metadataReadMetrics();
    result.fileVersion = source->fileVersion();
    result.iterations = source->iterations();
    result.source = std::move(source);
    return result;
#else
    static_cast<void>(cancellation);
    throw MetadataReadError("'" + location.path.string()
        + "' is an openPMD series, which this build cannot read "
          "(configure with -DAMREXPLORER_ENABLE_OPENPMD=ON)");
#endif
}

} // namespace amrvis
