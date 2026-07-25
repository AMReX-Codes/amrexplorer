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
        // A different dataset with an identical region and normal keeps the
        // geometry-aware default (same pixel-to-data mapping).
        require(DisplayCoordinator::rasterTransformPolicy(
                true, cached, makeRequest(2, 1, 0.0), true)
                == ImageTransformPolicy::GeometryAware,
            "a same-region dataset swap should stay geometry-aware");
    }

    return 0;
}
