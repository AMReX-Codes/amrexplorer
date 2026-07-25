#pragma once

#include <amrexplorer/core/Geometry.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace amrvis {

struct DatasetId {
    std::uint64_t value = 0;
    auto operator<=>(const DatasetId&) const = default;
};

struct FieldId {
    std::uint32_t value = 0;
    auto operator<=>(const FieldId&) const = default;
};

enum class SamplingPolicy : std::uint8_t {
    Nearest,
    PiecewiseConstant,
    Linear
};

enum class CompositionPolicy : std::uint8_t {
    FinestAvailable,
    ExactLevel
};

struct BlockRequest {
    DatasetId dataset;
    int timestep = 0;
    int level = 0;
    int gridIndex = 0;
    FieldId field;
    int firstComponent = 0;
    int componentCount = 1;
    int ghostWidth = 0;
};

struct SliceRequest {
    DatasetId dataset;
    FieldId field;
    int component = 0;
    int normalDirection = 2;
    double physicalPosition = 0.0;
    RealBox visibleRegion;
    int maximumLevel = 0;
    std::array<int, 2> outputSize{0, 0};
    SamplingPolicy sampling = SamplingPolicy::PiecewiseConstant;
    CompositionPolicy composition = CompositionPolicy::FinestAvailable;
};

struct LineRequest {
    DatasetId dataset;
    FieldId field;
    int component = 0;
    int axis = 0;                              // line direction: 0=x, 1=y, 2=z
    std::array<double, 3> fixedCoordinates{};  // physical coords of the other axes
    int maximumLevel = 0;
    CompositionPolicy composition = CompositionPolicy::FinestAvailable;
    // Optional extent along the line axis (from the viewport's visible region).
    // When unset the line spans the full level domain.
    std::optional<RealBox> region;
};

[[nodiscard]] std::vector<std::string> validateBlockRequest(const BlockRequest& request);
[[nodiscard]] std::vector<std::string> validateSliceRequest(
    const SliceRequest& request, int datasetDimension);
[[nodiscard]] std::vector<std::string> validateLineRequest(
    const LineRequest& request, int datasetDimension);

} // namespace amrvis

