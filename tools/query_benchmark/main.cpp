#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

int parseNonnegative(const char* value, const char* name)
{
    try {
        const auto parsed = std::stoi(value);
        if (parsed < 0) {
            throw std::invalid_argument("negative");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string(name) + " must be a non-negative integer");
    }
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 5) {
        std::cerr << "usage: query_benchmark PLOTFILE LEVEL GRID FIELD\n";
        return 2;
    }

    try {
        const std::filesystem::path plotfile(argv[1]);
        const auto levelIndex = parseNonnegative(argv[2], "level");
        const auto gridIndex = parseNonnegative(argv[3], "grid");
        const std::string fieldName(argv[4]);

        const auto metadataStart = std::chrono::steady_clock::now();
        const auto metadataResult = amrvis::PlotfileMetadataReader{}.read(plotfile);
        const auto metadataEnd = std::chrono::steady_clock::now();
        const auto field = std::find_if(
            metadataResult.metadata->fields.begin(), metadataResult.metadata->fields.end(),
            [&fieldName](const amrvis::FieldMetadata& candidate) {
                return candidate.name == fieldName;
            });
        if (field == metadataResult.metadata->fields.end()) {
            throw std::invalid_argument("field is not present in the dataset");
        }
        const auto fieldIndex = static_cast<std::size_t>(
            std::distance(metadataResult.metadata->fields.begin(), field));
        if (fieldIndex > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("field index exceeds request representation");
        }

        amrvis::BlockRequest request;
        request.dataset.value = 1;
        request.level = levelIndex;
        request.gridIndex = gridIndex;
        request.field.value = static_cast<std::uint32_t>(fieldIndex);

        const auto readStart = std::chrono::steady_clock::now();
        const auto result = amrvis::PlotfileBlockReader(
            plotfile, metadataResult.metadata).readBlock(request);
        const auto readEnd = std::chrono::steady_clock::now();

        bool haveValue = false;
        double minimum = 0.0;
        double maximum = 0.0;
        std::uint64_t nanCount = 0;
        for (std::size_t index = 0; index < result.block->values.size(); ++index) {
            const auto value = result.block->values[index];
            if (std::isnan(value)) {
                ++nanCount;
                continue;
            }
            if (!haveValue) {
                minimum = value;
                maximum = value;
                haveValue = true;
            } else {
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        }

        const auto metadataMicros = std::chrono::duration_cast<std::chrono::microseconds>(
            metadataEnd - metadataStart).count();
        const auto readMicros = std::chrono::duration_cast<std::chrono::microseconds>(
            readEnd - readStart).count();
        const auto& level = metadataResult.metadata->levels[static_cast<std::size_t>(levelIndex)];
        const auto& indexedBlock = level.blocks[static_cast<std::size_t>(gridIndex)];
        const auto payloadFileBytes = std::filesystem::file_size(
            plotfile / indexedBlock.filePath);

        std::cout << "field: " << fieldName << '\n'
                  << "level: " << levelIndex << '\n'
                  << "grid: " << gridIndex << '\n'
                  << "values_read: " << result.metrics.valuesRead << '\n'
                  << "payload_files_read: " << result.metrics.filesRead << '\n'
                  << "payload_bytes_read: " << result.metrics.bytesRead << '\n'
                  << "payload_file_bytes: " << payloadFileBytes << '\n'
                  << "metadata_files_read: " << metadataResult.metrics.filesRead << '\n'
                  << "metadata_bytes_read: " << metadataResult.metrics.bytesRead << '\n'
                  << "metadata_microseconds: " << metadataMicros << '\n'
                  << "block_read_microseconds: " << readMicros << '\n'
                  << "minimum: " << minimum << '\n'
                  << "maximum: " << maximum << '\n'
                  << "nan_values: " << nanCount << '\n';

        if (indexedBlock.statistics) {
            const auto component = static_cast<std::size_t>(request.field.value);
            std::cout << "indexed_minimum: " << indexedBlock.statistics->minimum[component] << '\n'
                      << "indexed_maximum: " << indexedBlock.statistics->maximum[component] << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "query_benchmark: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
