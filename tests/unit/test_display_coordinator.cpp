#include <amrexplorer/pipeline/DisplayCoordinator.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool nearlyEqual(double a, double b, double tolerance = 1.0e-12)
{
    return std::fabs(a - b)
        <= tolerance * std::max({1.0, std::fabs(a), std::fabs(b)});
}

amrvis::ScalarPlane makePlane(std::initializer_list<float> values)
{
    amrvis::ScalarPlane plane;
    plane.width = static_cast<int>(values.size());
    plane.height = 1;
    plane.values.assign(values);
    plane.valid.assign(values.size(), 1);
    return plane;
}

amrvis::SliceRequest makeRequest(
    std::uint64_t dataset, int normal, double regionLow)
{
    amrvis::SliceRequest request;
    request.dataset = amrvis::DatasetId{dataset};
    request.normalDirection = normal;
    request.visibleRegion.lower = {{regionLow, 0.0, 0.0}};
    request.visibleRegion.upper = {{regionLow + 1.0, 1.0, 1.0}};
    return request;
}

} // namespace

int main()
{
    using amrvis::DisplayCoordinator;
    using amrvis::ImageTransformPolicy;
    using Key = amrvis::DisplayCoordinator::RangeKey;

    // --- full-domain range cache ------------------------------------------
    {
        DisplayCoordinator coordinator;
        const Key key{amrvis::DatasetId{7}, amrvis::FieldId{2}, 3,
            amrvis::CompositionPolicy::FinestAvailable};
        require(!coordinator.cachedFullDomainRange(key).has_value(),
            "an empty cache returned a range");

        coordinator.storeFullDomainRange(key, {1.5, 9.5});
        const auto hit = coordinator.cachedFullDomainRange(key);
        require(hit && nearlyEqual(hit->first, 1.5)
                && nearlyEqual(hit->second, 9.5),
            "an exact key did not hit the cache");

        // Every key component must participate: a mismatch in any one is a
        // miss. The dataset component is the sequence-frame invalidation
        // (see sequence-frame-range-cache-goes-stale).
        auto other = key;
        other.dataset = amrvis::DatasetId{8};
        require(!coordinator.cachedFullDomainRange(other).has_value(),
            "a different dataset hit the cache");
        other = key;
        other.field = amrvis::FieldId{3};
        require(!coordinator.cachedFullDomainRange(other).has_value(),
            "a different field hit the cache");
        other = key;
        other.maximumLevel = 2;
        require(!coordinator.cachedFullDomainRange(other).has_value(),
            "a different maximum level hit the cache");
        other = key;
        other.composition = amrvis::CompositionPolicy::ExactLevel;
        require(!coordinator.cachedFullDomainRange(other).has_value(),
            "a different composition hit the cache");

        // Storing overwrites; invalidation clears.
        coordinator.storeFullDomainRange(key, {2.0, 4.0});
        require(nearlyEqual(coordinator.cachedFullDomainRange(key)->second, 4.0),
            "a second store did not overwrite the range");
        coordinator.invalidateRangeCache();
        require(!coordinator.cachedFullDomainRange(key).has_value(),
            "invalidation left the cached range behind");
    }

    // --- sharedVisibleRange -------------------------------------------------
    {
        const auto a = makePlane({2.0F, 5.0F});
        const auto b = makePlane({-1.0F, 3.0F});
        const std::array<const amrvis::ScalarPlane*, 2> planes{&a, &b};
        const auto shared = DisplayCoordinator::sharedVisibleRange(
            planes, false);
        require(shared && nearlyEqual(shared->first, -1.0)
                && nearlyEqual(shared->second, 5.0),
            "shared range is not the union across panels");
    }
    {
        // Masked and non-finite samples are skipped; empty planes and null
        // entries are tolerated.
        auto a = makePlane({2.0F, 50.0F});
        a.valid[1] = 0;
        auto b = makePlane({7.0F});
        b.values[0] = std::nanf("");
        const amrvis::ScalarPlane empty;
        const std::array<const amrvis::ScalarPlane*, 4> planes{
            &a, &b, &empty, nullptr};
        const auto shared = DisplayCoordinator::sharedVisibleRange(
            planes, false);
        // Only the 2.0 sample survives, so the degenerate union is padded
        // around it; the masked 50 and the NaN must not have widened it.
        require(shared && shared->first < 2.0 && shared->second > 2.0
                && nearlyEqual(shared->first, 2.0, 1.0e-5)
                && nearlyEqual(shared->second, 2.0, 1.0e-5),
            "masked/non-finite samples leaked into the shared range");
    }
    {
        const amrvis::ScalarPlane empty;
        const std::array<const amrvis::ScalarPlane*, 1> planes{&empty};
        require(!DisplayCoordinator::sharedVisibleRange(planes, false),
            "an all-empty panel set produced a range");
    }
    {
        // Degenerate union: additive pad in linear mode, ratio pad (staying
        // positive) in logarithmic mode.
        const auto constant = makePlane({5.0F, 5.0F});
        const std::array<const amrvis::ScalarPlane*, 1> planes{&constant};
        const auto linear = DisplayCoordinator::sharedVisibleRange(
            planes, false);
        require(linear && linear->first < 5.0 && linear->second > 5.0,
            "a constant plane was not padded in linear mode");
        const auto log = DisplayCoordinator::sharedVisibleRange(planes, true);
        require(log && log->first < 5.0 && log->second > 5.0
                && log->first > 0.0,
            "a constant plane was not ratio-padded in logarithmic mode");
    }

    // --- realignArrivalToRange ----------------------------------------------
    {
        amrvis::SliceDisplayResult result;
        result.slice.plane = makePlane({1.0F, 3.0F});
        result.minimum = 1.0;
        result.maximum = 3.0;
        result.mode = amrvis::DisplayMode::RasterContours;
        result.contourCount = 2;
        result.contourFinePlane = makePlane({1.0F, 3.0F});
        result.request.outputSize = {2, 1};
        const amrvis::Palette palette;

        // Realigning replaces the range, re-renders the raster, and
        // re-extracts the contours against the new range.
        DisplayCoordinator::realignArrivalToRange(
            result, {0.0, 10.0}, palette, true);
        require(nearlyEqual(result.minimum, 0.0)
                && nearlyEqual(result.maximum, 10.0),
            "realign did not replace the range");
        require(result.image.valid() && result.image.width > 0,
            "realign did not render the raster");

        // rasterUnchanged suppresses the raster render but not the range.
        amrvis::SliceDisplayResult untouched;
        untouched.slice.plane = makePlane({1.0F, 3.0F});
        untouched.rasterUnchanged = true;
        DisplayCoordinator::realignArrivalToRange(
            untouched, {0.0, 10.0}, palette, true);
        // A default (empty) ImageBuffer counts as valid, so probe emptiness.
        require(untouched.image.width == 0,
            "realign rendered a raster it promised to leave untouched");
        require(nearlyEqual(untouched.maximum, 10.0),
            "realign skipped the range on an unchanged raster");

        // realignRasterAndContours=false (the 3-D case, where the shared
        // sync realigns afterwards) only replaces the range.
        amrvis::SliceDisplayResult rangeOnly;
        rangeOnly.slice.plane = makePlane({1.0F, 3.0F});
        DisplayCoordinator::realignArrivalToRange(
            rangeOnly, {0.0, 10.0}, palette, false);
        require(rangeOnly.image.width == 0 && nearlyEqual(rangeOnly.maximum, 10.0),
            "range-only realign touched the raster");
    }

    // --- syncPanelsToSharedRange --------------------------------------------
    {
        DisplayCoordinator coordinator;
        const Key key{amrvis::DatasetId{1}, amrvis::FieldId{0}, 0,
            amrvis::CompositionPolicy::FinestAvailable};
        const amrvis::Palette palette;

        const auto a = makePlane({2.0F, 6.0F});
        const auto b = makePlane({-4.0F, 1.0F});
        const auto fine = makePlane({2.0F, 6.0F});
        const amrvis::ScalarPlane empty;
        const std::array<DisplayCoordinator::PanelSyncInput, 3> inputs{{
            {&a, &fine, 1, {2, 1}},
            {&b, nullptr, 1, {2, 1}},
            {&empty, nullptr, 1, {0, 0}},
        }};

        // No cached range: the union across panels drives every update.
        auto sync = coordinator.syncPanelsToSharedRange(
            key, inputs, false, true, 3, palette);
        require(sync.has_value(), "panel sync found no shared range");
        require(nearlyEqual(sync->range.first, -4.0)
                && nearlyEqual(sync->range.second, 6.0),
            "panel sync range is not the union");
        require(sync->panels[0].applies && sync->panels[1].applies
                && !sync->panels[2].applies,
            "panel sync applied to the wrong panels");
        require(sync->panels[0].image.width > 0
                && sync->panels[1].image.width > 0,
            "panel sync did not render the panel rasters");
        require(sync->panels[0].contoursRecomputed
                && !sync->panels[1].contoursRecomputed,
            "contours were not recomputed exactly where a fine plane exists");

        // A stored full-domain range takes precedence over the union.
        coordinator.storeFullDomainRange(key, {-100.0, 100.0});
        sync = coordinator.syncPanelsToSharedRange(
            key, inputs, false, false, 3, palette);
        require(sync && nearlyEqual(sync->range.first, -100.0)
                && nearlyEqual(sync->range.second, 100.0),
            "the cached full-domain range did not take precedence");

        // Neither cache nor finite samples: nothing to synchronize to.
        coordinator.invalidateRangeCache();
        const std::array<DisplayCoordinator::PanelSyncInput, 1> emptyOnly{{
            {&empty, nullptr, 1, {0, 0}}}};
        require(!coordinator.syncPanelsToSharedRange(
                key, emptyOnly, false, false, 3, palette).has_value(),
            "an empty panel set produced a sync");
    }

    // --- shared log degrades when the union crosses zero --------------------
    {
        // Regression for shared-log-range-render-throw-fails-load: with log
        // requested and one panel all-positive but the union crossing zero,
        // rendering each panel logarithmically against the negative shared
        // minimum used to throw and fail the whole load.
        const amrvis::Palette palette;
        const auto positive = makePlane({2.0F, 6.0F});   // all-positive panel
        const auto crossing = makePlane({-4.0F, 1.0F});  // crosses zero
        const auto fine = makePlane({2.0F, 6.0F});
        const std::array<DisplayCoordinator::PanelSyncInput, 2> mixed{{
            {&positive, &fine, 1, {2, 1}},
            {&crossing, nullptr, 1, {2, 1}},
        }};
        const auto sync = DisplayCoordinator::renderPanelsToSharedRange(
            std::nullopt, mixed, true, true, 3, palette);
        require(sync.has_value(), "mixed-sign log sync found no range");
        require(nearlyEqual(sync->range.first, -4.0)
                && nearlyEqual(sync->range.second, 6.0),
            "mixed-sign log sync range is not the union");
        require(!sync->logarithmic,
            "log was not degraded for a union that crosses zero");
        require(sync->panels[0].image.width > 0
                && sync->panels[1].image.width > 0,
            "mixed-sign log sync did not render every panel");

        // An all-positive union keeps the requested log mapping.
        const auto other = makePlane({3.0F, 5.0F});
        const std::array<DisplayCoordinator::PanelSyncInput, 2> allPositive{{
            {&positive, nullptr, 1, {2, 1}},
            {&other, nullptr, 1, {2, 1}},
        }};
        const auto positiveSync = DisplayCoordinator::renderPanelsToSharedRange(
            std::nullopt, allPositive, true, false, 3, palette);
        require(positiveSync && positiveSync->logarithmic,
            "log was dropped over an all-positive union");

        // realignArrivalToRange degrades identically: a log arrival realigned
        // to a full-domain range that crosses zero must render linear.
        amrvis::SliceDisplayResult arrival;
        arrival.slice.plane = makePlane({2.0F, 6.0F});
        arrival.logarithmic = true;
        DisplayCoordinator::realignArrivalToRange(
            arrival, {-1.0, 10.0}, palette, true);
        require(!arrival.logarithmic,
            "realign kept log against a range that crosses zero");
        require(arrival.image.valid() && arrival.image.width > 0,
            "realign did not render the degraded raster");

        amrvis::SliceDisplayResult positiveArrival;
        positiveArrival.slice.plane = makePlane({2.0F, 6.0F});
        positiveArrival.logarithmic = true;
        DisplayCoordinator::realignArrivalToRange(
            positiveArrival, {0.5, 10.0}, palette, true);
        require(positiveArrival.logarithmic,
            "realign dropped log over a positive range");
    }

    // --- planeDensitiesDiffer -----------------------------------------------
    {
        const std::array<int, 2> axes{0, 1};
        const auto plane = [](int width, double x0, double x1) {
            amrvis::ScalarPlane p;
            p.width = width;
            p.height = 1;
            p.values.assign(static_cast<std::size_t>(width), 1.0F);
            p.valid.assign(static_cast<std::size_t>(width), 1);
            p.physicalRegion.lower = {{x0, 0.0, 0.0}};
            p.physicalRegion.upper = {{x1, 1.0, 1.0}};
            return p;
        };

        // Same density: full domain vs a pan of the same-size window.
        require(!DisplayCoordinator::planeDensitiesDiffer(
                plane(4, 0.0, 4.0), plane(4, 4.0, 8.0), axes),
            "a same-density pan was flagged as a density change");
        // The issue-#45 shape: a capped full-domain raster (density 1/2)
        // replaced by a native-resolution crop (density 1).
        require(DisplayCoordinator::planeDensitiesDiffer(
                plane(4, 0.0, 8.0), plane(4, 0.0, 4.0), axes),
            "the capped-to-native density change was not detected");
        // Degenerate planes and extents compare as not-different.
        require(!DisplayCoordinator::planeDensitiesDiffer(
                amrvis::ScalarPlane{}, plane(4, 0.0, 4.0), axes),
            "an empty plane was flagged as a density change");
        require(!DisplayCoordinator::planeDensitiesDiffer(
                plane(4, 2.0, 2.0), plane(4, 0.0, 4.0), axes),
            "a zero-extent region was flagged as a density change");
    }

    // --- rasterTransformPolicy ----------------------------------------------
    {
        const auto cached = makeRequest(1, 1, 0.0);

        require(DisplayCoordinator::rasterTransformPolicy(
                false, cached, cached, true)
                == ImageTransformPolicy::GeometryAware,
            "no cache should be geometry-aware");

        // Zoomed panel-local refresh (same dataset + normal): preserve, even
        // when the region moved (pan) or shrank (zoom).
        require(DisplayCoordinator::rasterTransformPolicy(
                true, cached, makeRequest(1, 1, 0.5), true)
                == ImageTransformPolicy::Preserve,
            "a zoomed same-panel refresh should preserve");
        require(DisplayCoordinator::rasterTransformPolicy(
                true, cached, cached, false)
                == ImageTransformPolicy::GeometryAware,
            "an unzoomed same-panel refresh should be geometry-aware");

        // A different dataset whose region differs must refit even when the
        // raster dimensions coincide (the equal-size frame-step trap).
        require(DisplayCoordinator::rasterTransformPolicy(
                true, cached, makeRequest(2, 1, 0.5), true)
                == ImageTransformPolicy::Refit,
            "a cross-dataset region change should refit");
        // A different normal must refit regardless of dataset.
        require(DisplayCoordinator::rasterTransformPolicy(
                true, cached, makeRequest(1, 2, 0.0), true)
                == ImageTransformPolicy::Refit,
            "an orientation change should refit");
        // A different dataset always refits: equal raster dimensions do not
        // prove that its physical geometry is compatible.
        require(DisplayCoordinator::rasterTransformPolicy(
                true, cached, makeRequest(2, 1, 0.0), true)
                == ImageTransformPolicy::Refit,
            "a same-region dataset swap should refit");
    }

    return 0;
}
