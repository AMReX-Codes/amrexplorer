#include <amrexplorer/data/ViewData.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

amrvis::LineQueryResult lineResult(std::vector<float> values,
    std::vector<std::uint8_t> valid)
{
    amrvis::LineQueryResult result;
    result.line.positions.reserve(values.size());
    result.line.sourceLevel.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        result.line.positions.push_back(static_cast<double>(index));
        result.line.sourceLevel.push_back(valid[index] != 0 ? 0 : -1);
    }
    result.line.values = std::move(values);
    result.line.valid = std::move(valid);
    return result;
}

} // namespace

int main()
{
    auto gapped = amrvis::boundLineToViewport(
        lineResult({1.0F, 0.0F, 3.0F, 2.0F}, {1, 0, 1, 1}), 1);
    require(gapped.line.values.size() <= 2,
        "gapped bucket exceeded the viewport sample bound");
    require(std::ranges::any_of(gapped.line.valid,
                [](std::uint8_t valid) { return valid == 0; }),
        "downsampling discarded an AMR coverage gap");

    const auto extrema = amrvis::boundLineToViewport(
        lineResult({2.0F, -4.0F, 7.0F, 1.0F}, {1, 1, 1, 1}), 1);
    require(extrema.line.values == std::vector<float>({-4.0F, 7.0F}),
        "valid bucket did not preserve its ordered extrema");
    return 0;
}
