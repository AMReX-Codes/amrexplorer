#include <amrexplorer/data/ViewData.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace amrvis {

LineQueryResult boundLineToViewport(LineQueryResult result, int outputWidth)
{
    if (outputWidth < 1) {
        throw std::invalid_argument("line viewport width must be positive");
    }
    const auto size = result.line.values.size();
    if (result.line.positions.size() != size
        || result.line.valid.size() != size
        || result.line.sourceLevel.size() != size) {
        throw std::invalid_argument("line result vectors have inconsistent sizes");
    }
    const auto maximumSamples = static_cast<std::size_t>(outputWidth) * 2;
    if (size <= maximumSamples) {
        return result;
    }

    LineResult bounded;
    bounded.axis = result.line.axis;
    bounded.positionsAreIndices = result.line.positionsAreIndices;
    bounded.positions.reserve(maximumSamples);
    bounded.values.reserve(maximumSamples);
    bounded.valid.reserve(maximumSamples);
    bounded.sourceLevel.reserve(maximumSamples);

    const auto append = [&](std::size_t index) {
        bounded.positions.push_back(result.line.positions[index]);
        bounded.values.push_back(result.line.values[index]);
        bounded.valid.push_back(result.line.valid[index]);
        bounded.sourceLevel.push_back(result.line.sourceLevel[index]);
    };

    for (int pixel = 0; pixel < outputWidth; ++pixel) {
        const auto begin = size * static_cast<std::size_t>(pixel)
            / static_cast<std::size_t>(outputWidth);
        const auto end = size * static_cast<std::size_t>(pixel + 1)
            / static_cast<std::size_t>(outputWidth);
        if (begin >= end) {
            continue;
        }
        const auto invalid = std::find(result.line.valid.begin()
                + static_cast<std::ptrdiff_t>(begin),
            result.line.valid.begin() + static_cast<std::ptrdiff_t>(end), 0);
        if (invalid != result.line.valid.begin()
                + static_cast<std::ptrdiff_t>(end)) {
            append(static_cast<std::size_t>(
                std::distance(result.line.valid.begin(), invalid)));
            continue;
        }
        auto minimum = begin;
        auto maximum = begin;
        for (auto index = begin + 1; index < end; ++index) {
            if (result.line.values[index] < result.line.values[minimum]) {
                minimum = index;
            }
            if (result.line.values[index] > result.line.values[maximum]) {
                maximum = index;
            }
        }
        if (minimum == maximum) {
            append(minimum);
        } else if (minimum < maximum) {
            append(minimum);
            append(maximum);
        } else {
            append(maximum);
            append(minimum);
        }
    }
    result.line = std::move(bounded);
    return result;
}

} // namespace amrvis
