#include "ParticleColoring.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

amrvis::ParticleSample sample(std::vector<double> values, bool withAttribute = true)
{
    amrvis::ParticleSample result;
    if (withAttribute) {
        result.attribute = amrvis::ParticleAttribute{};
    }
    for (const auto value : values) {
        amrvis::ParticlePoint point;
        point.value = value;
        result.points.push_back(point);
    }
    return result;
}

} // namespace

int main()
{
    using amrvis::qt::ParticleColorRange;
    using amrvis::qt::particleValueRange;
    constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
    constexpr auto infinity = std::numeric_limits<double>::infinity();

    // The range spans the placeable values of the samples with an attribute.
    const std::vector samples{sample({3.0, -2.0, nan, infinity}),
        sample({100.0}, false), sample({0.5})};
    require(particleValueRange(samples, false) == ParticleColorRange{-2.0, 3.0},
        "the linear range is not the span of the finite values");
    require(particleValueRange(samples, true) == ParticleColorRange{0.5, 3.0},
        "the log range kept a non-positive value");
    require(!particleValueRange(std::vector{sample({nan})}, false)
            && !particleValueRange(std::vector{sample({1.0}, false)}, false),
        "a range came from nothing placeable");

    // Slots through the slices' mapping: truncated, the maximum in the last
    // slot, the middle for a flat range, none for what cannot be placed. A
    // range whose span overflows a double still maps.
    constexpr int slots = 253;
    const amrvis::qt::ParticleColorScale linear(ParticleColorRange{0.0, 10.0}, false, slots);
    require(linear.slot(2.5) == 63 && linear.slot(-5.0) == 0 && linear.slot(10.0) == 252
            && linear.slot(50.0) == 252,
        "a linear value is in the wrong slot");
    const amrvis::qt::ParticleColorScale decades(ParticleColorRange{1.0, 100.0}, true, slots);
    require(decades.slot(10.0) == 126, "a log value is in the wrong slot");
    require(amrvis::qt::ParticleColorScale(ParticleColorRange{7.0, 7.0}, false, slots).slot(7.0)
            == 126,
        "a flat range does not place its value in the middle");
    const amrvis::qt::ParticleColorScale extreme(
        ParticleColorRange{-1.0e308, 1.0e308}, false, slots);
    require(extreme.slot(0.0) == 126 && extreme.slot(1.0e308) == 252
            && extreme.slot(-1.0e308) == 0,
        "a range spanning past the largest double is misplaced");
    require(!linear.slot(nan) && !linear.slot(infinity) && !decades.slot(0.0)
            && !decades.slot(-1.0),
        "an unplaceable value was placed");

    std::cout << "particle coloring tests passed\n";
    return 0;
}
