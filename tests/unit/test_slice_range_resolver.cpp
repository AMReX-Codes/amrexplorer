#include <amrexplorer/pipeline/SliceRangeResolver.hpp>

#include <amrexplorer/core/ValueMapping.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

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
    return std::fabs(a - b) <= tolerance * std::max({1.0, std::fabs(a), std::fabs(b)});
}

amrvis::ScalarPlane makePlane(std::initializer_list<double> values)
{
    amrvis::ScalarPlane plane;
    plane.width = static_cast<int>(values.size());
    plane.height = 1;
    plane.values.assign(values);
    plane.valid.assign(values.size(), 1);
    return plane;
}

// Two levels, one scalar field; per-block statistics with the given ranges.
// A block whose range is {NaN, NaN} is stored without statistics.
amrvis::DatasetMetadata makeMetadata(
    std::pair<double, double> level0, std::pair<double, double> level1)
{
    amrvis::DatasetMetadata metadata;
    metadata.dimension = 2;
    metadata.finestLevel = 1;
    metadata.fields.push_back({"density", amrvis::Centering::Cell, {}});
    for (const auto& range : {level0, level1}) {
        amrvis::LevelMetadata level;
        level.level = static_cast<int>(metadata.levels.size());
        amrvis::BlockMetadata block;
        if (std::isfinite(range.first)) {
            block.statistics = amrvis::BlockStatistics{
                {range.first}, {range.second}};
        }
        level.blocks.push_back(std::move(block));
        metadata.levels.push_back(std::move(level));
    }
    return metadata;
}

constexpr auto kNoStats = std::numeric_limits<double>::quiet_NaN();

} // namespace

int main()
{
    using amrvis::CompositionPolicy;
    using amrvis::FieldId;
    using amrvis::RangeMode;

    // resolveRange/resolveDisplayRange only consult the dataset for the
    // Level/File statistics branch; the User and Visible paths are pure. The
    // null dataset below locks in that contract.
    const std::shared_ptr<amrvis::DatasetSession> noDataset;

    // --- finiteRange -----------------------------------------------------
    {
        auto plane = makePlane({2.0F, -1.0F, 5.0F, 9.0F});
        plane.valid[3] = 0;                       // masked: 9 must not count
        plane.values[0] = std::nanf("");          // non-finite: skipped
        const auto range = amrvis::finiteRange(plane);
        require(range.has_value(), "finiteRange dropped a finite plane");
        require(nearlyEqual(range->first, -1.0) && nearlyEqual(range->second, 5.0),
            "finiteRange did not skip masked and non-finite samples");
    }
    {
        auto plane = makePlane({3.0F, 4.0F});
        plane.valid.assign(2, 0);
        require(!amrvis::finiteRange(plane).has_value(),
            "finiteRange accepted an all-masked plane");
    }
    {
        const auto range = amrvis::finiteRange(makePlane({7.0F, 7.0F}));
        require(range.has_value() && range->first < 7.0 && range->second > 7.0,
            "finiteRange did not pad a constant plane to positive extent");
    }

    // --- selectedMetadataRange / effectiveRangeMode ----------------------
    {
        const auto metadata = makeMetadata({1.0, 4.0}, {2.0, 9.0});
        const auto file = amrvis::selectedMetadataRange(metadata, FieldId{0},
            1, CompositionPolicy::FinestAvailable, RangeMode::File);
        require(file && nearlyEqual(file->minimum, 1.0)
                && nearlyEqual(file->maximum, 9.0),
            "File range is not the union across levels");

        const auto composite = amrvis::selectedMetadataRange(metadata, FieldId{0},
            1, CompositionPolicy::FinestAvailable, RangeMode::Level);
        require(composite && nearlyEqual(composite->minimum, 1.0)
                && nearlyEqual(composite->maximum, 9.0),
            "composite Level range is not the union of levels 0..max");

        const auto exact = amrvis::selectedMetadataRange(metadata, FieldId{0},
            1, CompositionPolicy::ExactLevel, RangeMode::Level);
        require(exact && nearlyEqual(exact->minimum, 2.0)
                && nearlyEqual(exact->maximum, 9.0),
            "ExactLevel range did not use the selected level alone");

        require(!amrvis::selectedMetadataRange(metadata, FieldId{0},
                1, CompositionPolicy::FinestAvailable, RangeMode::Visible),
            "Visible mode returned a metadata range");

        require(amrvis::effectiveRangeMode(metadata, FieldId{0}, 1,
                CompositionPolicy::FinestAvailable, RangeMode::File)
                == RangeMode::File,
            "File mode downgraded despite available statistics");
    }
    {
        // A block without statistics makes File/Level unavailable, so the
        // requested mode downgrades to Visible.
        const auto metadata = makeMetadata({1.0, 4.0}, {kNoStats, kNoStats});
        require(!amrvis::selectedMetadataRange(metadata, FieldId{0},
                1, CompositionPolicy::FinestAvailable, RangeMode::File),
            "File range ignored a statistics-free block");
        require(amrvis::effectiveRangeMode(metadata, FieldId{0}, 1,
                CompositionPolicy::FinestAvailable, RangeMode::File)
                == RangeMode::Visible,
            "File mode did not downgrade to Visible without statistics");
        require(amrvis::effectiveRangeMode(metadata, FieldId{0}, 0,
                CompositionPolicy::FinestAvailable, RangeMode::Level)
                == RangeMode::Level,
            "Level 0 mode downgraded although level 0 has statistics");
    }

    // --- resolveRange ------------------------------------------------------
    {
        const auto plane = makePlane({0.5F, 8.0F});
        const auto [minimum, maximum] = amrvis::resolveRange(noDataset,
            FieldId{0}, 0, CompositionPolicy::FinestAvailable, RangeMode::User,
            std::pair{2.0, 6.0}, false, plane);
        require(nearlyEqual(minimum, 2.0) && nearlyEqual(maximum, 6.0),
            "User range was not passed through");
    }
    {
        const auto plane = makePlane({0.5F, 8.0F});
        const auto [minimum, maximum] = amrvis::resolveRange(noDataset,
            FieldId{0}, 0, CompositionPolicy::FinestAvailable,
            RangeMode::Visible, std::nullopt, false, plane);
        require(nearlyEqual(minimum, 0.5) && nearlyEqual(maximum, 8.0),
            "Visible range is not the plane extrema");
    }
    {
        // No finite samples: neutral fallbacks, linear and logarithmic.
        auto plane = makePlane({1.0F});
        plane.valid.assign(1, 0);
        const auto linear = amrvis::resolveRange(noDataset, FieldId{0}, 0,
            CompositionPolicy::FinestAvailable, RangeMode::Visible,
            std::nullopt, false, plane);
        require(nearlyEqual(linear.first, 0.0) && nearlyEqual(linear.second, 1.0),
            "empty-plane linear fallback is not [0, 1]");
        const auto log = amrvis::resolveRange(noDataset, FieldId{0}, 0,
            CompositionPolicy::FinestAvailable, RangeMode::Visible,
            std::nullopt, true, plane);
        require(nearlyEqual(log.first, 1.0) && nearlyEqual(log.second, 10.0),
            "empty-plane logarithmic fallback is not [1, 10]");
    }
    {
        // Degenerate user range: padded additively (linear) or by ratio (log).
        const auto plane = makePlane({1.0F});
        const auto linear = amrvis::resolveRange(noDataset, FieldId{0}, 0,
            CompositionPolicy::FinestAvailable, RangeMode::User,
            std::pair{5.0, 5.0}, false, plane);
        require(linear.first < 5.0 && linear.second > 5.0,
            "degenerate linear range was not padded");
        const auto log = amrvis::resolveRange(noDataset, FieldId{0}, 0,
            CompositionPolicy::FinestAvailable, RangeMode::User,
            std::pair{5.0, 5.0}, true, plane);
        require(log.first < 5.0 && log.second > 5.0 && log.first > 0.0,
            "degenerate logarithmic range was not ratio-padded");
        // Zero has no magnitude for the padding to be relative to. Falling back
        // to the smallest-normal backstop there gives a range of +/-2.2e-308,
        // which is what the color bar and the seeded User-range spin boxes then
        // show for a uniform plane of zeros -- an ordinary case. The padding
        // has to stay the absolute constant.
        const auto zero = amrvis::resolveRange(noDataset, FieldId{0}, 0,
            CompositionPolicy::FinestAvailable, RangeMode::User,
            std::pair{0.0, 0.0}, false, plane);
        require(nearlyEqual(zero.first, -1.0e-6)
                && nearlyEqual(zero.second, 1.0e-6),
            "a degenerate range at zero was not padded by the constant");
    }
    {
        bool threw = false;
        try {
            (void)amrvis::resolveRange(noDataset, FieldId{0}, 0,
                CompositionPolicy::FinestAvailable, RangeMode::User,
                std::pair{6.0, 2.0}, false, makePlane({1.0F}));
        } catch (const std::exception&) {
            threw = true;
        }
        require(threw, "an inverted user range did not throw");

        threw = false;
        try {
            (void)amrvis::resolveRange(noDataset, FieldId{0}, 0,
                CompositionPolicy::FinestAvailable, RangeMode::User,
                std::pair{-1.0, 2.0}, true, makePlane({1.0F}));
        } catch (const std::exception&) {
            threw = true;
        }
        require(threw, "a non-positive logarithmic range did not throw");
    }

    // --- logarithmicRangeViable --------------------------------------------
    // The one predicate behind every "keep Log or degrade to linear" decision
    // -- the slice resolver below, the 3-D shared range, and the arrival
    // realignment -- so it is pinned once here rather than three times.
    {
        require(amrvis::logarithmicRangeViable(1.0, 100.0),
            "an ordinary positive range was called unviable");
        require(!amrvis::logarithmicRangeViable(0.0, 10.0),
            "a range reaching zero was called viable");
        require(!amrvis::logarithmicRangeViable(-1.0, 2.0),
            "a range crossing zero was called viable");
        require(!amrvis::logarithmicRangeViable(5.0, 5.0),
            "a degenerate range was called viable");
        require(!amrvis::logarithmicRangeViable(10.0, 9.0),
            "an inverted range was called viable");
        require(!amrvis::logarithmicRangeViable(
                    std::numeric_limits<double>::quiet_NaN(), 10.0),
            "a NaN bound was called viable");
        require(!amrvis::logarithmicRangeViable(
                    1.0, std::numeric_limits<double>::infinity()),
            "an infinite bound was called viable");
        // The case positivity cannot see: ordered, positive, same logarithm.
        require(!amrvis::logarithmicRangeViable(
                    10.0, std::nextafter(10.0, 11.0)),
            "bounds sharing a logarithm were called viable");
    }

    // --- resolveDisplayRange ----------------------------------------------
    {
        // A logarithmic request over a non-positive range falls back to
        // linear and reports logarithmic=false instead of failing the slice.
        const auto plane = makePlane({1.0F});
        const auto fallback = amrvis::resolveDisplayRange(noDataset, FieldId{0},
            0, CompositionPolicy::FinestAvailable, RangeMode::User,
            std::pair{-1.0, 2.0}, true, plane);
        require(!fallback.logarithmic
                && nearlyEqual(fallback.minimum, -1.0)
                && nearlyEqual(fallback.maximum, 2.0),
            "non-positive logarithmic request did not fall back to linear");

        const auto log = amrvis::resolveDisplayRange(noDataset, FieldId{0},
            0, CompositionPolicy::FinestAvailable, RangeMode::User,
            std::pair{1.0, 100.0}, true, plane);
        require(log.logarithmic && nearlyEqual(log.minimum, 1.0)
                && nearlyEqual(log.maximum, 100.0),
            "a positive logarithmic request did not stay logarithmic");
    }
    {
        // Positive and strictly ordered, and still unusable as a log range:
        // adjacent doubles a decade up are too far apart for the degenerate
        // padding and too close for log() to separate. Reachable only since
        // the plane stopped narrowing its samples to float, which used to
        // collapse the pair and trigger the padding instead.
        constexpr double low = 10.0;
        const double high = std::nextafter(10.0, 11.0);
        require(low < high && std::log(low) == std::log(high),
            "the fixture pair no longer shares a logarithm");
        const auto plane = makePlane({low, high});

        bool threw = false;
        try {
            (void)amrvis::resolveRange(noDataset, FieldId{0}, 0,
                CompositionPolicy::FinestAvailable, RangeMode::Visible,
                std::nullopt, true, plane);
        } catch (const std::exception&) {
            threw = true;
        }
        require(threw, "a range that collapses under log did not throw");

        // The display path turns that into linear rather than failing the
        // slice, exactly as it does for a non-positive range.
        const auto fallback = amrvis::resolveDisplayRange(noDataset,
            FieldId{0}, 0, CompositionPolicy::FinestAvailable,
            RangeMode::Visible, std::nullopt, true, plane);
        require(!fallback.logarithmic && fallback.minimum == low
                && fallback.maximum == high,
            "a log-collapsing range did not fall back to linear");
    }

    return 0;
}
