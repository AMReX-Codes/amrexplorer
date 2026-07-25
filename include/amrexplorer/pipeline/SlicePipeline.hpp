#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/ParticleReader.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/pipeline/DisplayMode.hpp>
#include <amrexplorer/pipeline/SliceRangeResolver.hpp>
#include <amrexplorer/query/SliceQuery.hpp>
#include <amrexplorer/render2d/Contours.hpp>
#include <amrexplorer/render2d/ImageBuffer.hpp>
#include <amrexplorer/render2d/Palette.hpp>
#include <amrexplorer/render2d/VectorGlyphs.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The worker-side slice pipeline: everything needed to turn a slice request
// into a displayable result (raster image, contour polylines, vector glyphs,
// resolved range) off the GUI thread, with no Qt dependency. MainWindow
// orchestrates these from its request/completion handlers.

namespace amrvis {

class PlotfileDataset;

struct SliceDisplayResult {
    // The request that produced everything below; the GUI keeps it as the
    // cache key for the re-render-from-cache path.
    SliceRequest request;
    SliceQueryResult slice;
    ImageBuffer image;
    std::vector<VectorSegment> vectors;
    // Contour modes only: the piecewise plane at data resolution the
    // contours were extracted from (the display plane itself when the data
    // is finer than the display), its bilinear refinement, and the
    // polylines extracted from that refinement, already mapped to
    // display-plane pixel space (empty otherwise).
    ScalarPlane contourPlane;
    ScalarPlane contourFinePlane;
    int contourFineFactor = 1;
    std::vector<ContourPolyline> contourPolylines;
    std::string fieldName;
    double minimum = 0.0;
    double maximum = 1.0;
    bool logarithmic = false;
    DisplayMode mode = DisplayMode::Raster;
    std::uint32_t vectorUField = 0;
    std::uint32_t vectorVField = 0;
    int contourCount = 0;
    // Set when the image was intentionally not re-rendered (contour-only
    // refresh): the GUI keeps the view's current pixmap.
    bool rasterUnchanged = false;
    // Set when a composite (Finest Available) slice exceeded the cache budget
    // and was retried at a lower composite maximum level, mirroring
    // InitialSliceResult (see cache-budget-exceeded-hard-fails-after-load).
    int cacheFallbackFromLevel = -1;
    int cacheFallbackToLevel = -1;
};

struct InitialSliceResult {
    std::shared_ptr<PlotfileDataset> dataset;
    // One entry per displayed view, ordered by normal axis (2-D: one entry).
    std::vector<SliceDisplayResult> displays;
    std::vector<ParticleSample> particles;
    // First line of the plotfile Header when the path is a plotfile
    // directory; empty for standalone datasets.
    std::string fileVersion;
    // Set when a Finest Available load exceeded the cache budget and was
    // retried with a lower composite maximum level.
    int cacheFallbackFromLevel = -1;
    int cacheFallbackToLevel = -1;
};

// Everything needed to render one frame's slice(s) off the GUI thread. The
// sequence path builds this from the current UI state so frame switches keep
// the user's field/level/range/log/palette/visible-region settings; empty or
// default entries mean "fall back to the new dataset's defaults" (midpoint
// slice positions, whole domain, finest-native output size).
struct FrameSliceSpec {
    DisplayMode displayMode = DisplayMode::Raster;
    std::uint32_t field = 0;
    int levelSelection = -1;  // level combo data: -1 = finest available
    RangeMode rangeMode = RangeMode::File;
    std::optional<std::pair<double, double>> userRange;
    bool logarithmic = false;
    Palette palette;
    std::uint32_t vectorUField = 0;
    std::uint32_t vectorVField = 0;
    std::uint32_t vectorWField = 0;
    int contourCount = 10;
    bool defaultPositions = true;
    std::array<double, 3> slicePositions{0.0, 0.0, 0.0};
    std::vector<std::optional<RealBox>> visibleRegions;  // per view, normal order
    bool particleSelectionInitialized = false;
    std::vector<std::string> particleSpecies;
    double particleFraction = 1.0;
    std::uint64_t particleSeed = 0;
};

// Combo data sentinel for "Update to Level N" entries, which composite
// levels 0..N with FinestAvailable. The selected level N is data - 1000.
inline constexpr int kUpdateToLevelOffset = 1000;

struct LevelSelection {
    CompositionPolicy composition = CompositionPolicy::FinestAvailable;
    int maximumLevel = 0;
};

// Decodes the level combo's data encoding: -1 = finest available, N = level N
// only, kUpdateToLevelOffset + N = composite levels 0..N.
[[nodiscard]] LevelSelection decodeLevelData(int data, int finestLevel);

// Upper bound on slice output dimensions. One pixel per finest cell is the
// ideal, but a plane costs on the order of 10 bytes per pixel (float value,
// validity mask, source level, RGBA image), so uncapped native resolution on
// huge domains could allocate gigabytes. Once the region edges are snapped
// to cell boundaries (snapToCellBoundaries) the extent is an exact multiple
// of the cell size, so when this cap does engage the sampling pitch only
// ever exceeds the cell size — honest downsampling — and never produces
// duplicated or skipped cells.
inline constexpr int maxSliceOutputDimension = 4096;

// Native render resolution for a slice: the count of finest-level cells the
// visible region spans along each in-plane axis. At the 1x fixed scale this is
// the resolution legacy Amrvis drew (one pixel per finest cell); the larger
// fixed scales magnify it through the view zoom.
[[nodiscard]] std::array<int, 2> finestNativeOutputSize(
    const DatasetMetadata& metadata, const RealBox& region, int normal);

// The cache-key comparison for a cached slice: everything a cached slice
// depends on. Range, log scale, palette, and contour count are deliberately
// absent — those are recomputed from the cached planes on the cheap path.
[[nodiscard]] bool sameSliceSpec(const SliceRequest& lhs, const SliceRequest& rhs);

// The two in-plane axes of a slice, mirroring SliceQuery's plane axes.
[[nodiscard]] std::array<int, 2> slicePlaneAxes(int dimension, int normalDirection);

// The number of cells of `level` covering [lower, upper] on `axis`, clipped
// to the level's index domain: the data resolution of a slice request.
[[nodiscard]] int coveredCells(const DatasetMetadata& metadata, int level,
    int axis, double lower, double upper);

// How a slice-plane index maps to a physical coordinate. Indices use the
// level's integer box space (domain.lower .. domain.upper), which can be
// negative. Cell-centered data places the value at the center of each cell
// (index i → prob_lo + (i - domain.lower + 0.5)*dx); nodal data places it
// at the node (index i → prob_lo + (i - domain.lower)*dx).
[[nodiscard]] int sliceIndexForPosition(
    const DatasetMetadata& md, int level, int axis, double position);
[[nodiscard]] double positionForSliceIndex(
    const DatasetMetadata& md, int level, int axis, int index);

// Human-readable cache budget ("512 MiB", "1 GiB") for error messages.
[[nodiscard]] std::string cacheBudgetDescription(std::uint64_t bytes);

// Executes the display slice and resolves its range, rendering the raster.
[[nodiscard]] SliceDisplayResult executeSlice(
    const std::shared_ptr<PlotfileDataset>& dataset, const SliceRequest& request,
    RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const Palette& palette, StopToken cancellation);

// Vector mode queries the U- and V-component planes independently and
// derives arrow glyphs from them. Both slices share the raster request's
// region, level, and output size so the planes line up sample for sample.
void appendVectorGlyphs(const std::shared_ptr<PlotfileDataset>& dataset,
    SliceRequest request, FieldId uField, FieldId vField, int count,
    StopToken cancellation, SliceDisplayResult& result);

// Extracts contour polylines for the request at data resolution and maps
// them to display-plane pixel space; caches the fine plane on the result so
// range and contour-count changes can re-extract without a new SliceQuery.
void appendContours(const std::shared_ptr<PlotfileDataset>& dataset,
    const SliceRequest& request, int contourCount, double minimum,
    double maximum, bool logarithmic, StopToken cancellation,
    SliceDisplayResult& result);

// Re-render-from-cache: only palette/log/range/contour-count cosmetics
// changed (the request still matches the view's cache key), so the cached
// planes are re-ranged, re-rendered, and re-contoured without any SliceQuery.
// With rasterDirty false the raster is known unchanged and the image is not
// re-rendered; SliceDisplayResult::rasterUnchanged tells the GUI to keep
// the view's pixmap. Vector glyphs are reused from the cache: they do not
// depend on palette/log/range.
[[nodiscard]] SliceDisplayResult refreshCachedSlice(
    const std::shared_ptr<PlotfileDataset>& dataset,
    const SliceRequest& request, ScalarPlane displayPlane,
    ScalarPlane contourPlane, ScalarPlane contourFinePlane,
    int contourFineFactor, std::vector<VectorSegment> vectors,
    RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const Palette& palette, DisplayMode displayMode,
    std::uint32_t vectorUField, std::uint32_t vectorVField,
    int contourCount, bool rasterDirty);

// Re-extract contour polylines from an already-populated fine plane after the
// display range is replaced downstream of appendContours/refreshCachedSlice —
// full-domain-range reuse, the 3-D shared Visible range, or syncVisibleRanges.
// Cheap: the fine plane is cached, so no SliceQuery runs. Returns empty when
// the range is unusable (non-positive log range, zero extent), so a bad range
// clears the overlay rather than throwing. See the
// contours-stale-after-visible-range-sync issue.
[[nodiscard]] std::vector<ContourPolyline> recomputeContourPolylines(
    const ScalarPlane& finePlane, int fineFactor, double minimum,
    double maximum, bool logarithmic, int contourCount,
    int displayWidth, int displayHeight);

// SliceDisplayResult overload: regenerate the polylines to match the result's
// current (possibly replaced) minimum/maximum. No-op outside contour modes.
void recomputeContourPolylines(SliceDisplayResult& result);

// Opens one plotfile on a worker thread and renders the slice(s) described
// by spec — one per ortho view for 3-D, the single y-normal view for 2-D.
// Shared by the initial open path (default spec) and the sequence path
// (spec preserving the user's UI state across frames). cacheBudgetBytes
// bounds the dataset's block cache (the GUI passes initialCacheBudget()); a
// composite slice that exceeds it is retried at a lower maximum level, with
// the fallback recorded on the result.
[[nodiscard]] InitialSliceResult executeFrameLoad(
    const std::filesystem::path& path, DatasetId datasetId,
    const FrameSliceSpec& spec, std::uint64_t cacheBudgetBytes,
    StopToken cancellation,
    std::optional<PlotfileMetadataResult> preparedMetadata = std::nullopt,
    std::filesystem::path dataRoot = {});

} // namespace amrvis
