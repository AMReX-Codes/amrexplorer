#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/OrthoProjection.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/ValueMapping.hpp>

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
    ColorScaleConfig scale;
    constexpr VolumeRange() = default;
    constexpr VolumeRange(double minimumValue, double maximumValue,
        bool useLogarithmic, ColorScaleConfig scaleConfig = {})
        : minimum(minimumValue), maximum(maximumValue),
          logarithmic(useLogarithmic), scale(scaleConfig)
    {
    }
    friend bool operator==(const VolumeRange& lhs, const VolumeRange& rhs)
    {
        const auto left = effectiveColorScale(lhs.logarithmic, lhs.scale);
        const auto right = effectiveColorScale(rhs.logarithmic, rhs.scale);
        return lhs.minimum == rhs.minimum && lhs.maximum == rhs.maximum
            && left.scale == right.scale
            && (left.scale != ColorScale::SymLogarithmic
                || left.linearThreshold == right.linearThreshold);
    }
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
// A sensible sampled-grid cache budget for a caller that has to name one --
// the server's --volume-cache-mib default, say. It holds four grids of the
// default voxel budget. A session does not use it as its own default: it
// takes the budget the dataset was opened with. This is for whoever chooses
// that number, and it sits here so all three volume budgets read together.
inline constexpr std::uint64_t defaultVolumeGridCacheBytes
    = 256ULL * 1024ULL * 1024ULL;
// The most that budget may be raised to: an operational ceiling, not an
// eviction threshold. The cache evicts correctly at any budget -- distinct
// grid keys fill it and the least recently used ones go. What a budget past
// this buys is the room to fill it: 64 GiB is already 128 grids at the
// largest voxel budget (512^3 voxels, four bytes each), so a larger number
// stops describing memory any host will lend and becomes a way to be killed
// by the allocator instead of bounded by the setting. A ceiling on a budget
// is a property of the budget, so it reads here beside it rather than in
// whichever layer happens to expose the knob.
inline constexpr std::uint64_t maximumVolumeGridCacheBytes
    = 64ULL * 1024ULL * 1024ULL * 1024ULL;
static_assert(defaultVolumeGridCacheBytes <= maximumVolumeGridCacheBytes);
// What a rendered frame costs on the wire beyond its pixels: the response
// tables, the metrics and the cache snapshot around them. Both ends reserve
// it -- the client to size the frame it asks for, the server to refuse one
// that cannot fit before rendering it -- so it lives here rather than beside
// either of them, as sliceResponseOverheadBytes does for slices.
inline constexpr std::uint64_t volumeResponseOverheadBytes = 4096;

// What the sampler needs: the sub-box to sample, the levels to compose, and
// the budget bounding the grid. A field-for-field subset of the render
// request below, so both validate against the same rules.
struct VolumeSampleRequest {
    DatasetId dataset;
    FieldId field;
    int component = 0;
    int maximumLevel = 0;
    CompositionPolicy composition = CompositionPolicy::FinestAvailable;
    RealBox region;
    std::uint64_t maximumVoxels = defaultVolumeVoxelBudget;
    friend bool operator==(const VolumeSampleRequest&,
        const VolumeSampleRequest&) = default;
};

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
    ColorScaleConfig scale;
    VolumeTransferFunction transfer;
    // Ray samples per voxel along the ray (the step is the mean distance a
    // view ray spends crossing one voxel divided by this).
    int samplesPerVoxel = 2;
    // How each sample reads the sampled grid: Linear interpolates over the
    // eight voxel centres around it, Nearest and PiecewiseConstant take the
    // voxel it lands in. A march property, not a sampling one -- it does not
    // change the grid, so it is deliberately absent from VolumeSampleRequest
    // and from the grid cache key, and one cached grid serves both.
    SamplingPolicy sampling = SamplingPolicy::Linear;
    std::uint64_t maximumVoxels = defaultVolumeVoxelBudget;
    friend bool operator==(const VolumeRenderRequest&,
        const VolumeRenderRequest&) = default;
};

// The field sampled onto a uniform grid over `region`: voxel (i, j, k) is
// centred at lower + (i + 0.5) * pitch per axis, x fastest; NaN marks a voxel
// no level covers, and one whose value is not a finite float -- non-finite in
// the data, or past the range float can represent -- which the renderer treats
// as transparent. A region reaching past the domain is allowed and comes
// back NaN there, as the same region does on a slice.
struct VolumeGrid {
    std::array<int, 3> dims{0, 0, 0};
    RealBox region;
    std::vector<float> values;
    // Voxels holding a value the renderer can show, which is not quite the
    // same as voxels a level covered: a covered voxel whose source data is
    // itself NaN counts here as uncovered, because nothing downstream can
    // tell the two apart from the grid alone. Separating them would cost a
    // parallel coverage mask over the whole grid, and no reader branches on
    // the difference.
    std::uint64_t coveredVoxels = 0;
    // The finest level that put a showable value in the grid, or 0 when
    // none did.
    int maximumLevel = 0;
};

struct VolumeRenderMetrics {
    std::array<int, 3> gridDims{0, 0, 0};
    std::uint64_t coveredVoxels = 0;
    int sampledMaximumLevel = 0;
    bool gridFromCache = false;
    // Microseconds, not milliseconds: a small viewport renders in well under
    // one, and a whole-millisecond metric reports those as 0, which reads as
    // "not measured" rather than "fast".
    std::uint64_t sampleMicroseconds = 0;
    std::uint64_t renderMicroseconds = 0;
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

// The voxels a dims triple describes, saturating at the 64-bit maximum: a
// product large enough to wrap would otherwise equal the size of a much
// smaller (or empty) grid and pass a storage or budget check it should fail.
// Callers pass positive dims; the zero test guards the division, not the
// overflow.
[[nodiscard]] std::uint64_t volumeVoxelCount(
    const std::array<int, 3>& dims) noexcept;

// Structural validity of a request, before a session checks it against its
// dataset: every field bounded and finite. Empty when valid.
[[nodiscard]] std::vector<std::string> validateVolumeTransferFunction(
    const VolumeTransferFunction& transfer);
[[nodiscard]] std::vector<std::string> validateVolumeSampleRequest(
    const VolumeSampleRequest& request, int datasetDimension);
[[nodiscard]] std::vector<std::string> validateVolumeRenderRequest(
    const VolumeRenderRequest& request, int datasetDimension);

// The sampling fields of a render request, so the one validator above and the
// sampler itself see the same values.
[[nodiscard]] VolumeSampleRequest volumeSampleRequestOf(
    const VolumeRenderRequest& request);

} // namespace amrvis
