#pragma once

#include <amrexplorer/core/Request.hpp>

#include <cstddef>
#include <functional>

namespace amrvis {

struct BlockKey {
    DatasetId dataset;
    int timestep = 0;
    int level = 0;
    int grid = 0;
    FieldId field;
    int firstComponent = 0;
    int componentCount = 1;
    int ghostWidth = 0;

    friend bool operator==(const BlockKey&, const BlockKey&) = default;
};

struct BlockKeyHash {
    [[nodiscard]] std::size_t operator()(const BlockKey& key) const noexcept
    {
        std::size_t seed = std::hash<std::uint64_t>{}(key.dataset.value);
        const auto combine = [&seed](std::size_t value) {
            seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        };
        combine(std::hash<int>{}(key.timestep));
        combine(std::hash<int>{}(key.level));
        combine(std::hash<int>{}(key.grid));
        combine(std::hash<std::uint32_t>{}(key.field.value));
        combine(std::hash<int>{}(key.firstComponent));
        combine(std::hash<int>{}(key.componentCount));
        combine(std::hash<int>{}(key.ghostWidth));
        return seed;
    }
};

[[nodiscard]] inline BlockKey makeBlockKey(const BlockRequest& request)
{
    return {
        request.dataset,
        request.timestep,
        request.level,
        request.gridIndex,
        request.field,
        request.firstComponent,
        request.componentCount,
        request.ghostWidth
    };
}

} // namespace amrvis

