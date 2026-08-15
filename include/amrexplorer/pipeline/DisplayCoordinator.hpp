#pragma once

#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/pipeline/ImageTransformPolicy.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <array>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Owns derived display state that the GUI's slice paths must keep mutually
// consistent across transitions, and the pure decisions that were previously
// re-implemented inline at each call site. Step 2 of the MainWindow
// extraction (see agent-notes/issues/mainwindow-needs-extraction.md): today
// this holds the full-domain range cache, the shared-Visible-range union,
// and the raster transform-policy decision; the per-panel reconcile step
// grows onto it next.

namespace amrvis {

class DisplayCoordinator {
public:
    // Physical facts that determine whether an existing view transform still
    // denotes the same data-space window after a raster replacement. Dataset
    // ids are deliberately absent: sequence frames receive fresh ids even
    // when their physical geometry is identical.
    struct RasterGeometry {
        RealBox physicalDomain;
        int dimension = 0;
        int coordinateSystem = 0;
        int normalDirection = 0;
        SphericalDisplay sphericalDisplay = SphericalDisplay::RZ;

        friend bool operator==(const RasterGeometry&, const RasterGeometry&)
            = default;
    };

    // What the cached full-domain range is valid for. Sequence frames load
    // fresh datasets with new ids, so keying on the dataset invalidates the
    // cache across frames (see sequence-frame-range-cache-goes-stale).
    struct RangeKey {
        DatasetId dataset;
        FieldId field;
        int maximumLevel = -1;
        CompositionPolicy composition = CompositionPolicy::FinestAvailable;

        friend bool operator==(const RangeKey&, const RangeKey&) = default;
    };

    // The cached full-domain Visible range, if it was stored for exactly
    // this key; reused for zoomed (subregion) slices so the color bar stays
    // stable during pan and zoom.
    [[nodiscard]] std::optional<std::pair<double, double>>
    cachedFullDomainRange(const RangeKey& key) const;

    void storeFullDomainRange(
        const RangeKey& key, std::pair<double, double> range);

    // Drops the cached range (dataset change, slice-position move, range
    // state reset).
    void invalidateRangeCache();

    // The shared Visible range across panels: the union of finite valid
    // samples, padded to positive extent (ratio-padded when logarithmic over
    // a positive value, additively otherwise). Empty planes are skipped;
    // nullopt when no panel has a finite sample. One definition for the two
    // previously drift-prone copies (executeFrameLoad's shared-range block
    // and syncVisibleRanges).
    [[nodiscard]] static std::optional<std::pair<double, double>>
    sharedVisibleRange(
        std::span<const ScalarPlane* const> planes, bool logarithmic);

    // How the view should treat its transform when `incoming` replaces the
    // raster produced by `cached`. Compatible physical geometry preserves the
    // ImageView mode across ordinary refreshes and sequence-frame ids. A
    // physical-domain, coordinate-system, dimensional, normal-direction, or
    // displayed-orientation change refits deterministically.
    [[nodiscard]] static ImageTransformPolicy rasterTransformPolicy(
        const std::optional<RasterGeometry>& cached,
        const RasterGeometry& incoming);

    // Realigns an arrived result to a replacement display range (the reused
    // full-domain range): overwrites minimum/maximum and, when
    // realignRasterAndContours is set, re-renders the raster against it
    // (unless the raster was intentionally left untouched) and re-extracts
    // the contour polylines. The 3-D shared-range sync realigns every panel
    // afterwards, so its caller passes false there to avoid rendering each
    // panel twice; 2-D has no later sync and passes true.
    static void realignArrivalToRange(SliceDisplayResult& result,
        std::pair<double, double> range, const Palette& palette,
        bool realignRasterAndContours);

    // One panel's inputs to / outputs of the shared-range sync below.
    struct PanelSyncInput {
        const ScalarPlane* plane = nullptr;        // display plane
        const ScalarPlane* contourPlane = nullptr; // contour modes only
        std::array<int, 2> outputSize{0, 0};  // display size for contours
    };
    struct PanelSyncUpdate {
        bool applies = false;          // false: empty plane, leave untouched
        ImageBuffer image;             // rendered against the shared range
        bool contoursRecomputed = false;
        std::vector<ContourPolyline> contourPolylines;
    };
    struct SharedRangeSync {
        std::pair<double, double> range;
        // One log flag for the whole panel set: the requested log mapping,
        // kept only when the shared range minimum is positive (renderScalarPlane
        // rejects a non-positive log minimum). Callers apply it to every panel
        // and to the shared color bar so all three agree with the raster.
        bool logarithmic = false;
        std::vector<PanelSyncUpdate> panels;  // parallel to the input span
    };

    // The 3-D Visible-range synchronization: resolve the shared range (the
    // cached full-domain range for `key` when current, else the union of the
    // panels' finite extrema) and produce, per panel, the raster re-rendered
    // against it and — in contour mode — the polylines re-extracted against
    // it, so every panel matches the one shared color bar. Returns nullopt
    // when there is neither a cached range nor any finite sample, in which
    // case the caller leaves the panels untouched.
    [[nodiscard]] std::optional<SharedRangeSync> syncPanelsToSharedRange(
        const RangeKey& key, std::span<const PanelSyncInput> panels,
        bool logarithmic, bool contourMode, int contourCount,
        const Palette& palette) const;

    // The pure, coordinator-state-free half of syncPanelsToSharedRange: takes
    // the already-resolved cached range (or nullopt to fall back to the
    // panels' finite-extrema union) and does the heavy work — extrema scans,
    // contour re-extraction, and up to one full raster render per panel.
    // Static so the GUI can run it on a worker thread over immutable plane
    // snapshots while the coordinator itself stays confined to the GUI
    // thread (see heavy-work-on-gui-thread, part D).
    [[nodiscard]] static std::optional<SharedRangeSync>
    renderPanelsToSharedRange(
        std::optional<std::pair<double, double>> sharedRange,
        std::span<const PanelSyncInput> panels, bool logarithmic,
        bool contourMode, int contourCount, const Palette& palette);

    // True when the two planes map pixels to physical units at different
    // densities on the given in-plane axes — the condition under which
    // preserving a scene transform would misframe the replacement raster
    // (a capped full-domain raster replaced by a finer subregion crop, see
    // issue #45). Degenerate planes or extents compare as not-different, so
    // callers fall back to the plain Preserve behavior.
    [[nodiscard]] static bool planeDensitiesDiffer(
        const ScalarPlane& before, const ScalarPlane& after,
        std::array<int, 2> axes);

private:
    std::optional<RangeKey> m_rangeKey;
    std::pair<double, double> m_range{0.0, 0.0};
};

} // namespace amrvis
