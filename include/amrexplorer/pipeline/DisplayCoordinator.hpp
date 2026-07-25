#pragma once

#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/pipeline/ImageTransformPolicy.hpp>

#include <optional>
#include <span>
#include <utility>

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
    // raster produced by `cached`. A zoomed panel-local refresh (same
    // dataset and orientation) preserves; a replacement whose region or
    // orientation differs refits even if the dimensions coincide; everything
    // else refits only on a dimension change. Moved verbatim from the GUI's
    // showSlice (see the raster-colorbar and rubber-band issues).
    [[nodiscard]] static ImageTransformPolicy rasterTransformPolicy(
        bool hasCachedRequest, const SliceRequest& cached,
        const SliceRequest& incoming, bool zoomed);

private:
    std::optional<RangeKey> m_rangeKey;
    std::pair<double, double> m_range{0.0, 0.0};
};

} // namespace amrvis
