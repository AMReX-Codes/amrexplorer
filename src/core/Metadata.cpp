#include <amrexplorer/core/Metadata.hpp>

#include <cstddef>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace amrvis {

bool isNodal(const IntBox& box, int axis) noexcept
{
    return box.centering[static_cast<std::size_t>(axis)] != 0;
}

Centering centeringFromIndexType(const Int3& type, int dimension) noexcept
{
    const auto x = type[0] != 0;
    const auto y = dimension >= 2 && type[1] != 0;
    const auto z = dimension >= 3 && type[2] != 0;
    if (!x && !y && !z) return Centering::Cell;
    if (x && (dimension < 2 || y) && (dimension < 3 || z)) return Centering::Node;
    if (x && !y && !z) return Centering::FaceX;
    if (!x && y && !z) return Centering::FaceY;
    if (!x && !y && z) return Centering::FaceZ;
    if (!x && y && z) return Centering::EdgeX;
    if (x && !y && z) return Centering::EdgeY;
    if (x && y && !z) return Centering::EdgeZ;
    return Centering::Mixed;
}

double samplePosition(
    const LevelMetadata& level, int axis, int index) noexcept
{
    const auto i = static_cast<std::size_t>(axis);
    const auto half = isNodal(level.domain, axis) ? 0.0 : 0.5;
    return level.indexOrigin[i]
        + (static_cast<double>(index) + half) * level.cellSize[i];
}

RealBox sampleBounds(
    const LevelMetadata& level, const IntBox& box, int dimension) noexcept
{
    RealBox result;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto nodalHalf = isNodal(box, axis) ? 0.5 : 0.0;
        result.lower[i] = level.indexOrigin[i]
            + (static_cast<double>(box.lower[i]) - nodalHalf)
                * level.cellSize[i];
        result.upper[i] = level.indexOrigin[i]
            + (static_cast<double>(box.upper[i]) + 1.0 - nodalHalf)
                * level.cellSize[i];
    }
    return result;
}

RealBox datasetSampleBounds(const DatasetMetadata& metadata) noexcept
{
    if (metadata.levels.empty()) {
        return metadata.physicalDomain;
    }
    const auto& level = metadata.levels.back();
    return sampleBounds(level, level.domain, metadata.dimension);
}

int sampleIndex(const LevelMetadata& level, int axis, double position)
{
    const auto i = static_cast<std::size_t>(axis);
    const auto phase = isNodal(level.domain, axis) ? 0.5 : 0.0;
    const auto offset = std::floor(
        (position - level.indexOrigin[i]) / level.cellSize[i] + phase);
    if (!std::isfinite(offset)
        || offset < static_cast<double>(std::numeric_limits<int>::min())
        || offset > static_cast<double>(std::numeric_limits<int>::max())) {
        throw std::out_of_range("physical coordinate exceeds sample index range");
    }
    return static_cast<int>(offset);
}

std::vector<MetadataIssue> validateMetadata(const DatasetMetadata& metadata)
{
    std::vector<MetadataIssue> issues;
    const auto add = [&issues](std::string path, std::string message) {
        issues.push_back({std::move(path), std::move(message)});
    };

    if (metadata.dimension < 1 || metadata.dimension > 3) {
        add("dimension", "must be between 1 and 3");
    }
    if (metadata.finestLevel < 0) {
        add("finestLevel", "must be non-negative");
    }
    if (!metadata.physicalDomain.valid(metadata.dimension)) {
        add("physicalDomain", "must have finite positive extent in every active direction");
    }

    const auto expectedLevels = metadata.finestLevel >= 0
        ? static_cast<std::size_t>(metadata.finestLevel + 1)
        : std::size_t{0};
    if (metadata.levels.size() != expectedLevels) {
        add("levels", "size must equal finestLevel + 1");
    }

    std::unordered_set<std::string> fieldNames;
    for (std::size_t fieldIndex = 0; fieldIndex < metadata.fields.size(); ++fieldIndex) {
        const auto& field = metadata.fields[fieldIndex];
        const auto path = "fields[" + std::to_string(fieldIndex) + "]";
        if (field.name.empty()) {
            add(path + ".name", "must not be empty");
        } else if (!fieldNames.insert(field.name).second) {
            add(path + ".name", "must be unique");
        }
        if (field.componentCount <= 0) {
            add(path + ".componentCount", "must be positive");
        }
        if (!field.componentNames.empty()
            && field.componentNames.size() != static_cast<std::size_t>(field.componentCount)) {
            add(path + ".componentNames", "size must equal componentCount when provided");
        }
    }

    for (std::size_t levelIndex = 0; levelIndex < metadata.levels.size(); ++levelIndex) {
        const auto& level = metadata.levels[levelIndex];
        const auto path = "levels[" + std::to_string(levelIndex) + "]";
        if (level.level != static_cast<int>(levelIndex)) {
            add(path + ".level", "must match its index");
        }
        if (!level.domain.valid(metadata.dimension)) {
            add(path + ".domain", "must be a valid index-space box");
        }
        for (int axis = 0; axis < metadata.dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            if (level.domain.centering[i] != 0
                && level.domain.centering[i] != 1) {
                add(path + ".domain.centering",
                    "must contain only cell (0) or node (1) index types");
                break;
            }
            if (!(level.cellSize[i] > 0.0)) {
                add(path + ".cellSize", "must be positive in every active direction");
                break;
            }
            if (!std::isfinite(level.indexOrigin[i])) {
                add(path + ".indexOrigin",
                    "must be finite in every active direction");
                break;
            }
            if (levelIndex + 1 < metadata.levels.size()
                && level.refinementRatioToNext[i] <= 0) {
                add(path + ".refinementRatioToNext",
                    "must be positive in every active direction");
                break;
            }
        }
        for (std::size_t boxIndex = 0; boxIndex < level.boxes.size(); ++boxIndex) {
            const auto& box = level.boxes[boxIndex];
            const auto boxPath = path + ".boxes[" + std::to_string(boxIndex) + "]";
            if (!box.valid(metadata.dimension)) {
                add(boxPath, "must be a valid index-space box");
                continue;
            }
            for (int axis = 0; axis < metadata.dimension; ++axis) {
                const auto i = static_cast<std::size_t>(axis);
                if (box.lower[i] < level.domain.lower[i]
                    || box.upper[i] > level.domain.upper[i]) {
                    add(boxPath, "must lie within the level domain");
                    break;
                }
                if (box.centering[i] != level.domain.centering[i]) {
                    add(boxPath + ".centering", "must match the level domain");
                    break;
                }
            }
        }
        if (!level.blocks.empty() && level.blocks.size() != level.boxes.size()) {
            add(path + ".blocks", "size must match the level box array");
        }
        for (std::size_t blockIndex = 0; blockIndex < level.blocks.size(); ++blockIndex) {
            const auto& block = level.blocks[blockIndex];
            const auto blockPath = path + ".blocks[" + std::to_string(blockIndex) + "]";
            if (!(block.box == level.boxes[blockIndex])) {
                add(blockPath + ".box", "must match the corresponding level box");
            }
            if (block.filePath.empty()) {
                add(blockPath + ".filePath", "must not be empty");
            }
            if (block.statistics) {
                const auto expectedComponents = static_cast<std::size_t>(level.storedComponents);
                if (block.statistics->minimum.size() != expectedComponents
                    || block.statistics->maximum.size() != expectedComponents) {
                    add(blockPath + ".statistics",
                        "component count must match the stored data");
                }
            }
        }
    }

    return issues;
}

} // namespace amrvis
