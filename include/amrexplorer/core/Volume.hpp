#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/OrthoProjection.hpp>
#include <amrexplorer/core/Request.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace amrvis {

// Direct volume rendering of a 3-D dataset: the field is sampled into a
// bounded uniform grid and ray-cast with an orthographic camera and a
// transfer function (a colour and an opacity per lookup entry). The request
// is what a session renders, locally or on the remote server; the frame is
// the viewport-sized image that comes back. Field data never travels: the
// server renders and returns pixels.

// The colour and opacity lookup the renderer maps values through, entry 0
// for the bottom of the range and the last entry for the top; built by the
// client from its palette and opacity controls, so the server needs neither.
struct VolumeTransferFunction {
    std::vector<std::uint32_t> colors;   // 0x00RRGGBB per entry
    std::vector<float> opacities;        // [0, 1] per entry, per voxel
    friend bool operator==(const VolumeTransferFunction&,
        const VolumeTransferFunction&) = default;
};

// The value range the transfer function spans, linear or logarithmic.
struct VolumeRange {
    double minimum = 0.0;
    double maximum = 1.0;
    bool logarithmic = false;
    friend bool operator==(const VolumeRange&, const VolumeRange&) = default;
};

inline constexpr int maxVolumeOutputDimension = 4096;
inline constexpr std::size_t maxVolumeTransferEntries = 1024;
inline constexpr int maxVolumeSamplesPerVoxel = 8;
inline constexpr double minVolumeZoom = 0.01;
inline constexpr double maxVolumeZoom = 100.0;
// The sampled grid's voxel budget: the default (256^3, 64 MiB of floats) and
// the cap either side enforces (512^3).
inline constexpr std::uint64_t defaultVolumeVoxelBudget
    = 256ULL * 256ULL * 256ULL;
inline constexpr std::uint64_t maxVolumeVoxelBudget = 512ULL * 512ULL * 512ULL;

struct VolumeRenderRequest {
    DatasetId dataset;
    FieldId field;
    int component = 0;
    // The finest level to sample and how the levels compose (as slices).
    int maximumLevel = 0;
    CompositionPolicy composition = CompositionPolicy::FinestAvailable;
    // The physical sub-box to sample; the whole domain to start with.
    RealBox region;
    OrthoCamera camera;
    std::array<int, 2> outputSize{0, 0};   // width, height in pixels
    // The range the colours span; nullopt asks the renderer to use the
    // sampled grid's finite extrema (the "Visible" range) and report them
    // back, with `logarithmic` as the requested mapping (falling back to
    // linear when the data is not strictly positive). `logarithmic` is
    // ignored when `range` is set: the range carries its own mapping.
    std::optional<VolumeRange> range;
    bool logarithmic = false;
    VolumeTransferFunction transfer;
    // Ray samples per voxel along the ray (the step is the smallest voxel
    // pitch divided by this).
    int samplesPerVoxel = 2;
    std::uint64_t maximumVoxels = defaultVolumeVoxelBudget;
    friend bool operator==(const VolumeRenderRequest&,
        const VolumeRenderRequest&) = default;
};

// The field sampled onto a uniform grid over `region`: voxel (i, j, k) is
// centred at lower + (i + 0.5) * pitch per axis, x fastest; NaN marks a voxel
// no level covers (or a non-finite value), which the renderer treats as
// transparent.
struct VolumeGrid {
    std::array<int, 3> dims{0, 0, 0};
    RealBox region;
    std::vector<float> values;
    std::uint64_t coveredVoxels = 0;
    int maximumLevel = 0;   // the finest level that contributed
};

struct VolumeRenderMetrics {
    std::array<int, 3> gridDims{0, 0, 0};
    std::uint64_t coveredVoxels = 0;
    int sampledMaximumLevel = 0;
    bool gridFromCache = false;
    std::uint64_t sampleMilliseconds = 0;
    std::uint64_t renderMilliseconds = 0;
    std::uint64_t candidateBlocks = 0;
    std::uint64_t blocksRead = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t payloadBytesRead = 0;
    friend bool operator==(const VolumeRenderMetrics&,
        const VolumeRenderMetrics&) = default;
};

// The rendered image: premultiplied 0xAARRGGBB pixels, row 0 at the top,
// alpha 0 where no ray sample landed, so it composites over any background.
struct VolumeFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;
    VolumeRange usedRange;   // the range the colours were mapped with
    VolumeRenderMetrics metrics;
    // A cache-pressure fallback to a coarser composite level, as slices
    // report it; -1 when none happened.
    int cacheFallbackFromLevel = -1;
    int cacheFallbackToLevel = -1;
    friend bool operator==(const VolumeFrame&, const VolumeFrame&) = default;
};

// Structural validity of a request, before a session checks it against its
// dataset: every field bounded and finite. Empty when valid.
[[nodiscard]] std::vector<std::string> validateVolumeTransferFunction(
    const VolumeTransferFunction& transfer);
[[nodiscard]] std::vector<std::string> validateVolumeRenderRequest(
    const VolumeRenderRequest& request, int datasetDimension);

} // namespace amrvis
