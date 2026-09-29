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
    using amrvis::qt::particleColorFraction;
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

    // Placement: linear and log, clamped, the middle of a flat range, none
    // for what cannot be placed.
    const ParticleColorRange linear{0.0, 10.0};
    require(particleColorFraction(2.5, linear, false) == 0.25
            && particleColorFraction(-5.0, linear, false) == 0.0
            && particleColorFraction(50.0, linear, false) == 1.0,
        "a linear value is misplaced");
    const ParticleColorRange decades{1.0, 100.0};
    require(particleColorFraction(10.0, decades, true) == 0.5,
        "a log value is misplaced");
    require(particleColorFraction(7.0, ParticleColorRange{7.0, 7.0}, false) == 0.5,
        "a flat range does not place its value in the middle");
    require(!particleColorFraction(nan, linear, false)
            && !particleColorFraction(infinity, linear, false)
            && !particleColorFraction(0.0, decades, true)
            && !particleColorFraction(-1.0, decades, true),
        "an unplaceable value was placed");

    std::cout << "particle coloring tests passed\n";
    return 0;
}
