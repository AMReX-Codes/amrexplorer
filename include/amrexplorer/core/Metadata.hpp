#pragma once

#include <amrexplorer/core/Geometry.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace amrvis {

enum class Centering : std::uint8_t {
    Cell,
    Node,
    FaceX,
    FaceY,
    FaceZ,
    EdgeX,
    EdgeY,
    EdgeZ,
    Mixed,
    Unknown
};

struct FieldMetadata {
    std::string name;
    int componentCount = 1;
    Centering centering = Centering::Cell;
    std::vector<std::string> componentNames;
};

struct BlockStatistics {
    std::vector<double> minimum;
    std::vector<double> maximum;
};

struct BlockMetadata {
    IntBox box;
    std::string filePath;
    std::uint64_t fileOffset = 0;
    std::optional<BlockStatistics> statistics;
};

struct LevelMetadata {
    int level = 0;
    int step = 0;
    IntBox domain;
    Int3 refinementRatioToNext{{1, 1, 1}};
    Real3 cellSize{{1.0, 1.0, 1.0}};
    // Physical coordinate of nodal index zero on each axis. A sample at
    // integer index i is located at indexOrigin+i*dx on nodal axes and at
    // indexOrigin+(i+1/2)*dx on cell-centered axes.
    Real3 indexOrigin;
    Int3 ghostWidth;
    int storedComponents = 0;
    int visMfHeaderVersion = 0;
    std::vector<IntBox> boxes;
    std::vector<BlockMetadata> blocks;
    std::string dataPath;
    std::string realDescriptor;
};

struct DatasetMetadata {
    int dimension = 0;
    int finestLevel = 0;
    // True when the dataset represents one complete stored FAB rather than a
    // MultiFab or plotfile hierarchy.  Its one block is the full data domain.
    bool isFab = false;
    // Standalone FABs and MultiFabs do not carry a physical Geometry. Their
    // synthetic unit geometry supports image-space operations, but coordinate
    // output such as line-plot abscissas must use integer indices instead.
    bool hasPhysicalGeometry = true;
    double time = 0.0;
    int coordinateSystem = 0;
    RealBox physicalDomain;
    std::vector<LevelMetadata> levels;
    std::vector<FieldMetadata> fields;
};

struct MetadataIssue {
    std::string path;
    std::string message;
};

[[nodiscard]] std::vector<MetadataIssue> validateMetadata(const DatasetMetadata& metadata);

[[nodiscard]] bool isNodal(const IntBox& box, int axis) noexcept;
[[nodiscard]] Centering centeringFromIndexType(
    const Int3& indexType, int dimension) noexcept;
[[nodiscard]] double samplePosition(
    const LevelMetadata& level, int axis, int index) noexcept;
[[nodiscard]] RealBox sampleBounds(
    const LevelMetadata& level, const IntBox& box, int dimension) noexcept;
[[nodiscard]] RealBox datasetSampleBounds(const DatasetMetadata& metadata) noexcept;
[[nodiscard]] int sampleIndex(
    const LevelMetadata& level, int axis, double position);

} // namespace amrvis
