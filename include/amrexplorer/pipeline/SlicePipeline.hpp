#pragma once

#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/data/DatasetSession.hpp>
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
#include <span>
#include <string>
#include <utility>
#include <vector>

// The worker-side slice pipeline: everything needed to turn a slice request
// into a displayable result (raster image, contour polylines, vector glyphs,
// resolved range) off the GUI thread, with no Qt dependency. MainWindow
// orchestrates these from its request/completion handlers.

namespace amrvis {

struct SliceDisplayResult {
    // The request that produced everything below; the GUI keeps it as the
    // cache key for the re-render-from-cache path.
    SliceRequest request;
    SliceQueryResult slice;
    ImageBuffer image;
    // Re-render-from-cache fast path: the view's existing display plane is
    // immutable and unchanged, so refreshCachedSlice hands it back by
    // shared_ptr instead of deep-copying it into slice.plane (which is up to
    // ~110 MB). When set, displayPlane() and showSlice read through it and
    // adopt it directly; empty on the executeSlice path, which produces a
    // fresh slice.plane. Note this makes a cache-path arrival *reuse* the same
    // pointer the view already holds — pointer identity is no longer a proxy
    // for "changed", so identity-keyed staleness guards must gate on something
    // else (see PlaneViewState::plane in MainWindow.hpp).
    //
    // NOT covered by this fast path: the contour-mode companion plane
    // (contourPlane below) is still deep-copied by value, ~14 MB per view. It
    // retires with the wider "stop round-tripping planes through
    // SliceDisplayResult" cleanup, which also removes this dual
    // reusedPlane/slice.plane slot.
    std::shared_ptr<const ScalarPlane> reusedPlane;
    // Coordinate system of the dataset (AMReX Header code). 2 (spherical) warps
    // `image` into physical (R, Z); all others leave it in logical space.
    int coordinateSystem = 0;
    // 2-D spherical layout used to produce `image` (R-Z warp / r-theta /
    // theta-r); ignored for non-spherical data.
    SphericalDisplay sphericalDisplay = SphericalDisplay::RZ;
    // Physical bounds of `image` in display space: slice.plane.physicalRegion
    // for non-spherical data, the (R, Z) sector bounding box for 2-D spherical
    // R-Z, or the (possibly axis-swapped) logical bounds for r-theta / theta-r.
    // Overlays and the probe map through this.
    RealBox displayRegion;
    std::vector<VectorSegment> vectors;
    // Contour modes only: the plane the contours were traced on (at contour
    // resolution, which since #56 removed supersampling is the plane the
    // contours are extracted from directly), and the polylines extracted from
    // it, already mapped to display-plane pixel space (empty otherwise).
    ScalarPlane contourPlane;
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

    // The display plane, whether freshly produced (slice.plane) or reused from
    // the cache (reusedPlane). Every reader of the display plane goes through
    // this so the two producers stay interchangeable. The ternary is not a null
    // fallback: reusedPlane is either unset (executeSlice path -> slice.plane)
    // or a valid plane (refreshCachedSlice rejects a null argument), so this
    // never substitutes an empty plane for a missing one.
    [[nodiscard]] const ScalarPlane& displayPlane() const noexcept
    {
        return reusedPlane ? *reusedPlane : slice.plane;
    }
};

struct InitialSliceResult {
    std::shared_ptr<DatasetSession> dataset;
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
    // Nonzero for remote sequence frames. Server-local dataset identifiers can
    // restart after reconnect, so GUI caches also key their lifetime to this
    // client-side connection generation.
    std::uint64_t connectionGeneration = 0;
};

// Everything needed to render one frame's slice(s) off the GUI thread. The
// sequence path builds this from the current UI state so frame switches keep
// the user's field/level/range/log/palette/visible-region settings; empty or
// default entries mean "fall back to the new dataset's defaults" (midpoint
// slice positions, whole domain, finest-native output size).
struct FrameSliceSpec {
    DisplayMode displayMode = DisplayMode::Raster;
    std::uint32_t field = 0;
    // The same selections by name, which is what actually carries them from
    // one frame to the next. An index is only meaningful in the field list it
    // came from: frames need not agree on their stored fields, and a derived
    // definition that one frame cannot resolve is left out of that frame's
    // list (DerivedFieldPolicy::Skip), which compacts every id after it. An
    // index alone then lands on whatever now occupies that slot, silently.
    // Empty means "no name to go on", and the index is used as it was.
    std::string fieldName;
    std::string vectorUFieldName;
    std::string vectorVFieldName;
    std::string vectorWFieldName;
    int levelSelection = -1;  // level combo data: -1 = finest available
    RangeMode rangeMode = RangeMode::File;
    std::optional<std::pair<double, double>> userRange;
    bool logarithmic = false;
    Palette palette;
    std::uint32_t vectorUField = 0;
    std::uint32_t vectorVField = 0;
    std::uint32_t vectorWField = 0;
    int contourCount = 10;
    // 2-D spherical warp resolution carried across frame loads (see
    // SliceRequest::sphericalSupersample).
    int sphericalSupersample = 4;
    // 2-D spherical display layout carried across frame loads.
    SphericalDisplay sphericalDisplay = SphericalDisplay::RZ;
    bool defaultPositions = true;
    std::array<double, 3> slicePositions{0.0, 0.0, 0.0};
    std::vector<std::optional<RealBox>> visibleRegions;  // per view, normal order
    std::vector<std::array<int, 2>> outputSizes;  // per view, viewport pixels
    // When true, outputSizes are viewport bounds rather than exact raster
    // dimensions; the frame loader preserves the physical aspect ratio within
    // each bound after it has opened the frame and knows its geometry.
    bool outputSizesAreViewportBounds = false;
    bool includeGridBoxes = false;
    bool particleSelectionInitialized = false;
    std::vector<std::string> particleSpecies;
    double particleFraction = 1.0;
    std::uint64_t particleSeed = 0;
    // Fields to compute rather than read (core/DerivedField.hpp), installed by
    // the session executeFrameLoad opens. A definition this frame cannot
    // resolve is left out of it rather than failing the load, and the session
    // says which through DatasetSession::skippedDerivedFields().
    //
    // Only executeFrameLoad can act on these, because it is what opens the
    // session. Passing a spec that carries them to executeSessionFrameLoad is
    // not an error -- executeFrameLoad does exactly that, having already
    // installed them -- but for a session opened elsewhere they are inert:
    // its field list was fixed when it opened. Callers with a session in hand
    // gate on DatasetSession::supportsDerivedFields().
    std::vector<DerivedFieldDefinition> derivedFields;
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

// The field a carried selection means in *this* dataset: the one named, if it
// is here, and otherwise the index clamped into range. Names are how a
// selection survives a frame switch -- see FrameSliceSpec::fieldName for why
// an index does not -- and the clamp is what happens when the frame simply
// does not have the field any more. An empty name goes straight to the index,
// which is the behaviour of every caller that has no name to give.
[[nodiscard]] std::uint32_t resolveSpecField(
    const DatasetMetadata& metadata, const std::string& name,
    std::uint32_t index);

// Upper bound on slice output dimensions. One pixel per finest cell is the
// ideal, but a plane costs on the order of 10 bytes per pixel (float value,
// validity mask, source level, RGBA image), so uncapped native resolution on
// huge domains could allocate gigabytes. Once the region edges are snapped
// to cell boundaries (snapToCellBoundaries) the extent is an exact multiple
// of the cell size, so when this cap does engage the sampling pitch only
// ever exceeds the cell size — honest downsampling — and never produces
// duplicated or skipped cells.
inline constexpr int maxSliceOutputDimension = maxViewOutputDimension;

// Native render resolution for a slice: the count of finest-level cells the
// visible region spans along each in-plane axis. At the 1x fixed scale this is
// the resolution legacy Amrvis drew (one pixel per finest cell); the larger
// fixed scales magnify it through the view zoom.
[[nodiscard]] std::array<int, 2> finestNativeOutputSize(
    const DatasetMetadata& metadata, const RealBox& region, int normal);

// Fits a slice region into a pixel bound without distorting its in-plane
// aspect ratio, measured in finest cells (the display's unit: one square
// pixel per cell), so cells that are not square do not squeeze the raster.
[[nodiscard]] std::array<int, 2> viewportBoundedOutputSize(
    const DatasetMetadata& metadata, const RealBox& region, int normal,
    std::array<int, 2> viewportSize);
// Viewport-bounded remote raster size that never invents more samples than
// the region's finest-native raster. This preserves Fit's bounded transport
// without making a small native image behave as though it had been enlarged.
[[nodiscard]] std::array<int, 2> nativeBoundedViewportOutputSize(
    const DatasetMetadata& metadata, const RealBox& region, int normal,
    std::array<int, 2> viewportSize);
[[nodiscard]] std::array<int, 2> frameBudgetBoundedOutputSize(
    std::array<int, 2> outputSize,
    std::optional<std::uint32_t> maximumResponseBytes);

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
    const std::shared_ptr<DatasetSession>& dataset, const SliceRequest& request,
    RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const Palette& palette, StopToken cancellation);

// Vector mode queries the U- and V-component planes independently and
// derives arrow glyphs from them. Both slices share the raster request's
// region, level, and output size so the planes line up sample for sample.
void appendVectorGlyphs(const std::shared_ptr<DatasetSession>& dataset,
    SliceRequest request, FieldId uField, FieldId vField, int count,
    StopToken cancellation, SliceDisplayResult& result);

// The whole non-cached slice worker: executeSlice plus the display-mode
// extras (contours or vector glyphs), with the same cache-pressure level
// fallback as executeFrameLoad — a composite (Finest Available) request
// whose multi-level working set overflows the budget is retried at a lower
// composite maximum level, with the fallback recorded on the result; an
// exact level (or level 0) cannot shed resolution and reports an actionable
// error instead (plain untranslated text; the GUI wraps failures in its own
// translated message).
[[nodiscard]] SliceDisplayResult executeSliceWithFallback(
    const std::shared_ptr<DatasetSession>& dataset, SliceRequest request,
    RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const Palette& palette, DisplayMode displayMode,
    std::uint32_t vectorUField, std::uint32_t vectorVField, int contourCount,
    StopToken cancellation);

// Extracts contour polylines for the request at data resolution and maps
// them to display-plane pixel space; caches the contour plane on the result so
// range and contour-count changes can re-extract without a new SliceQuery.
void appendContours(const std::shared_ptr<DatasetSession>& dataset,
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
    const std::shared_ptr<DatasetSession>& dataset,
    const SliceRequest& request,
    std::shared_ptr<const ScalarPlane> displayPlanePtr,
    ScalarPlane contourPlane, std::vector<VectorSegment> vectors,
    RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const Palette& palette, DisplayMode displayMode,
    std::uint32_t vectorUField, std::uint32_t vectorVField,
    int contourCount, bool rasterDirty, StopToken cancellation = {});

// Re-extract contour polylines from an already-populated contour plane after
// the display range is replaced downstream of appendContours/refreshCachedSlice
// — full-domain-range reuse, the 3-D shared Visible range, or syncVisibleRanges.
// Cheap: the contour plane is cached, so no SliceQuery runs. Returns empty when
// the range is unusable (non-positive log range, zero extent), so a bad range
// clears the overlay rather than throwing. See the
// contours-stale-after-visible-range-sync issue.
[[nodiscard]] std::vector<ContourPolyline> recomputeContourPolylines(
    const ScalarPlane& plane, double minimum,
    double maximum, bool logarithmic, int contourCount,
    int displayWidth, int displayHeight);

// SliceDisplayResult overload: regenerate the polylines to match the result's
// current (possibly replaced) minimum/maximum. No-op outside contour modes.
void recomputeContourPolylines(SliceDisplayResult& result);

// Loads the selected particle species in dataset discovery order. Unknown
// names are ignored, matching the behavior needed when a plotfile sequence
// frame does not contain every species selected on another frame.
[[nodiscard]] std::vector<ParticleSample> loadParticleSamples(
    DatasetSession& dataset,
    std::span<const std::string> selectedSpecies, double fraction,
    std::uint64_t seed, StopToken cancellation = {});

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

// Renders an already-open session using the same initial/frame pipeline.
[[nodiscard]] InitialSliceResult executeSessionFrameLoad(
    std::shared_ptr<DatasetSession> dataset, const FrameSliceSpec& spec,
    StopToken cancellation);

} // namespace amrvis
