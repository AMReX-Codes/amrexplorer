#include <amrexplorer/io/StandaloneMetadataReader.hpp>
#include <amrexplorer/io/FabCatalog.hpp>
#include <amrexplorer/io/detail/VisMfIndex.hpp>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amrvis {
namespace {

std::vector<int> parseIntegers(const std::string& text)
{
    std::string numbers = text;
    std::replace_if(numbers.begin(), numbers.end(), [](char character) {
        return !(character >= '0' && character <= '9')
            && character != '-' && character != '+';
    }, ' ');
    std::istringstream input(numbers);
    std::vector<int> result;
    int value = 0;
    while (input >> value) {
        result.push_back(value);
    }
    return result;
}

int inferBoxDimension(const std::string& boxText)
{
    const auto innerStart = boxText.find('(', 1);
    const auto innerEnd = boxText.find(')', innerStart);
    if (innerStart == std::string::npos || innerEnd == std::string::npos) {
        throw MetadataReadError("cannot infer the standalone data dimension");
    }
    const auto lower = parseIntegers(
        boxText.substr(innerStart, innerEnd - innerStart + 1));
    if (lower.empty() || lower.size() > 3) {
        throw MetadataReadError("standalone data dimension must be between 1 and 3");
    }
    return static_cast<int>(lower.size());
}

int inferMultiFabDimension(const std::filesystem::path& headerPath)
{
    std::ifstream input(headerPath);
    if (!input) {
        throw MetadataReadError("cannot open MultiFab header '" + headerPath.string() + "'");
    }
    std::string line;
    while (std::getline(input, line)) {
        const auto boxStart = line.find("((");
        if (boxStart != std::string::npos) {
            return inferBoxDimension(line.substr(boxStart));
        }
    }
    throw MetadataReadError("MultiFab header contains no BoxArray entries");
}

std::shared_ptr<DatasetMetadata> makeSingleLevelMetadata(
    int dimension, const IntBox& domain, int components, std::string_view fieldPrefix)
{
    auto metadata = std::make_shared<DatasetMetadata>();
    metadata->dimension = dimension;
    metadata->finestLevel = 0;
    metadata->hasPhysicalGeometry = false;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto index = static_cast<std::size_t>(axis);
        metadata->physicalDomain.lower[index] = static_cast<double>(domain.lower[index]);
        metadata->physicalDomain.upper[index] = static_cast<double>(domain.upper[index]) + 1.0;
    }
    metadata->fields.reserve(static_cast<std::size_t>(components));
    for (int component = 0; component < components; ++component) {
        auto name = std::string(fieldPrefix) + std::to_string(component);
        metadata->fields.push_back({
            name, 1, centeringFromIndexType(domain.centering, dimension), {name}});
    }
    metadata->levels.resize(1);
    metadata->levels.front().domain = domain;
    metadata->levels.front().storedComponents = components;
    return metadata;
}

void validateStandalone(const DatasetMetadata& metadata)
{
    const auto issues = validateMetadata(metadata);
    if (!issues.empty()) {
        throw MetadataReadError("invalid standalone metadata at "
            + issues.front().path + ": " + issues.front().message);
    }
}

} // namespace

PlotfileMetadataResult StandaloneMetadataReader::readFab(
    const std::filesystem::path& fabPath, std::uint64_t offset) const
{
    const auto record = inspectFabRecord(fabPath, offset);

    auto metadata = makeSingleLevelMetadata(
        record.dimension, record.storedBox, record.components, "Fab_");
    metadata->isFab = true;
    auto& level = metadata->levels.front();
    level.boxes.push_back(record.storedBox);
    level.dataPath = fabPath.filename().generic_string();
    level.visMfHeaderVersion = record.visMfHeaderVersion;
    level.realDescriptor = record.realDescriptor;
    level.blocks.push_back({record.storedBox,
        fabPath.filename().generic_string(), offset, std::nullopt});
    metadata->physicalDomain = sampleBounds(
        level, level.domain, record.dimension);
    validateStandalone(*metadata);

    const auto headerBytes = record.payloadOffset - record.headerOffset;
    return {
        std::shared_ptr<const DatasetMetadata>(std::move(metadata)),
        MetadataReadMetrics{1, headerBytes, 0, 0},
        "FAB"
    };
}

PlotfileMetadataResult makeSelectedFabMetadata(
    const DatasetMetadata& source, int levelIndex, std::size_t blockIndex,
    const std::filesystem::path& dataRoot)
{
    if (levelIndex < 0
        || static_cast<std::size_t>(levelIndex) >= source.levels.size()) {
        throw MetadataReadError("selected FAB level is unavailable");
    }
    const auto& sourceLevel = source.levels[static_cast<std::size_t>(levelIndex)];
    if (blockIndex >= sourceLevel.blocks.size()) {
        throw MetadataReadError("selected FAB index is unavailable");
    }
    const auto& sourceBlock = sourceLevel.blocks[blockIndex];
    auto storedBox = sourceBlock.box;
    for (int axis = 0; axis < source.dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        storedBox.lower[i] -= sourceLevel.ghostWidth[i];
        storedBox.upper[i] += sourceLevel.ghostWidth[i];
    }
    if (sourceLevel.visMfHeaderVersion == 1) {
        storedBox = inspectFabRecord(
            dataRoot / sourceBlock.filePath, sourceBlock.fileOffset).storedBox;
    }

    auto metadata = std::make_shared<DatasetMetadata>();
    metadata->dimension = source.dimension;
    metadata->finestLevel = 0;
    metadata->isFab = true;
    metadata->hasPhysicalGeometry = false;
    metadata->time = source.time;
    metadata->coordinateSystem = source.coordinateSystem;
    metadata->fields = source.fields;
    metadata->levels.resize(1);
    auto& level = metadata->levels.front();
    level.domain = storedBox;
    level.indexOrigin = sourceLevel.indexOrigin;
    level.cellSize = sourceLevel.cellSize;
    level.storedComponents = sourceLevel.storedComponents;
    level.visMfHeaderVersion = sourceLevel.visMfHeaderVersion;
    level.realDescriptor = sourceLevel.realDescriptor;
    level.dataPath = sourceLevel.dataPath;
    level.boxes.push_back(storedBox);
    level.blocks.emplace_back();
    auto& block = level.blocks.back();
    block.box = storedBox;
    block.filePath = sourceBlock.filePath;
    block.fileOffset = sourceBlock.fileOffset;
    metadata->physicalDomain = sampleBounds(level, storedBox, source.dimension);
    validateStandalone(*metadata);
    return {std::shared_ptr<const DatasetMetadata>(std::move(metadata)),
        MetadataReadMetrics{}, "FAB"};
}

PlotfileMetadataResult StandaloneMetadataReader::readMultiFab(
    const std::filesystem::path& prefixOrHeader) const
{
    auto prefix = prefixOrHeader;
    if (prefix.string().ends_with("_H")) {
        auto value = prefix.string();
        value.resize(value.size() - 2);
        prefix = value;
    }
    const auto headerPath = std::filesystem::path(prefix.string() + "_H");
    const auto dimension = inferMultiFabDimension(headerPath);
    const auto index = detail::readVisMfIndex(headerPath, dimension);
    if (index.boxes.empty() || index.components <= 0) {
        throw MetadataReadError("standalone MultiFab is empty");
    }

    auto domain = index.boxes.front();
    for (const auto& box : index.boxes) {
        for (int axis = 0; axis < dimension; ++axis) {
            const auto coordinate = static_cast<std::size_t>(axis);
            domain.lower[coordinate] = std::min(domain.lower[coordinate], box.lower[coordinate]);
            domain.upper[coordinate] = std::max(domain.upper[coordinate], box.upper[coordinate]);
        }
    }
    auto metadata = makeSingleLevelMetadata(
        dimension, domain, index.components, "MultiFab_");
    auto& level = metadata->levels.front();
    level.boxes = index.boxes;
    level.ghostWidth = index.ghostWidth;
    level.dataPath = prefix.filename().generic_string();
    level.visMfHeaderVersion = index.version;
    level.realDescriptor = index.realDescriptor;
    metadata->physicalDomain = sampleBounds(level, level.domain, dimension);
    level.blocks.reserve(index.boxes.size());
    for (std::size_t block = 0; block < index.boxes.size(); ++block) {
        BlockMetadata metadataBlock;
        metadataBlock.box = index.boxes[block];
        metadataBlock.filePath = index.fileNames[block];
        metadataBlock.fileOffset = index.fileOffsets[block];
        if (index.hasPerBlockStatistics
            && index.minimum.size() == index.boxes.size()
            && index.maximum.size() == index.boxes.size()) {
            metadataBlock.statistics = BlockStatistics{
                index.minimum[block], index.maximum[block]};
        }
        level.blocks.push_back(std::move(metadataBlock));
    }
    validateStandalone(*metadata);
    return {
        std::shared_ptr<const DatasetMetadata>(std::move(metadata)),
        MetadataReadMetrics{1, index.bytesRead, 0, 0},
        "VisMF-" + std::to_string(index.version)
    };
}

PlotfileMetadataResult readDatasetMetadata(const std::filesystem::path& path)
{
    if (std::filesystem::is_directory(path)
        && std::filesystem::is_regular_file(path / "Header")) {
        return PlotfileMetadataReader{}.read(path);
    }
    if (path.string().ends_with("_H")
        || std::filesystem::is_regular_file(
            std::filesystem::path(path.string() + "_H"))) {
        return StandaloneMetadataReader{}.readMultiFab(path);
    }
    return StandaloneMetadataReader{}.readFab(path);
}

} // namespace amrvis
