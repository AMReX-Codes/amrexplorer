#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {

std::string jsonEscape(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += character; break;
        }
    }
    return result;
}

void printText(const amrvis::PlotfileMetadataResult& result)
{
    const auto& metadata = *result.metadata;
    std::cout << "format: " << result.fileVersion << '\n'
              << "dimension: " << metadata.dimension << '\n'
              << "time: " << std::setprecision(17) << metadata.time << '\n'
              << "finest_level: " << metadata.finestLevel << '\n'
              << "fields: " << metadata.fields.size() << '\n';
    for (const auto& field : metadata.fields) {
        std::cout << "  - " << field.name << '\n';
    }
    std::cout << "levels: " << metadata.levels.size() << '\n';
    for (const auto& level : metadata.levels) {
        std::cout << "  - level " << level.level << ": " << level.boxes.size()
                  << " grids, data " << level.dataPath << '\n';
    }
    std::cout << "metadata_files_read: " << result.metrics.filesRead << '\n'
              << "metadata_bytes_read: " << result.metrics.bytesRead << '\n'
              << "payload_files_read: " << result.metrics.payloadFilesRead << '\n'
              << "payload_bytes_read: " << result.metrics.payloadBytesRead << '\n';
}

void printJson(const amrvis::PlotfileMetadataResult& result)
{
    const auto& metadata = *result.metadata;
    std::cout << "{\n"
              << "  \"format\": \"" << jsonEscape(result.fileVersion) << "\",\n"
              << "  \"dimension\": " << metadata.dimension << ",\n"
              << "  \"time\": " << std::setprecision(17) << metadata.time << ",\n"
              << "  \"finest_level\": " << metadata.finestLevel << ",\n"
              << "  \"fields\": [";
    for (std::size_t i = 0; i < metadata.fields.size(); ++i) {
        if (i != 0) {
            std::cout << ", ";
        }
        std::cout << '"' << jsonEscape(metadata.fields[i].name) << '"';
    }
    std::cout << "],\n  \"levels\": [\n";
    for (std::size_t i = 0; i < metadata.levels.size(); ++i) {
        const auto& level = metadata.levels[i];
        std::cout << "    {\"level\": " << level.level
                  << ", \"grids\": " << level.boxes.size()
                  << ", \"data_path\": \"" << jsonEscape(level.dataPath) << "\"}";
        std::cout << (i + 1 == metadata.levels.size() ? "\n" : ",\n");
    }
    std::cout << "  ],\n"
              << "  \"metadata_files_read\": " << result.metrics.filesRead << ",\n"
              << "  \"metadata_bytes_read\": " << result.metrics.bytesRead << ",\n"
              << "  \"payload_files_read\": " << result.metrics.payloadFilesRead << ",\n"
              << "  \"payload_bytes_read\": " << result.metrics.payloadBytesRead << "\n"
              << "}\n";
}

} // namespace

int main(int argc, char* argv[])
{
    bool json = false;
    std::filesystem::path plotfile;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--json") {
            json = true;
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "usage: dataset_inspect [--json] DATASET\n";
            return 0;
        } else if (plotfile.empty()) {
            plotfile = argument;
        } else {
            std::cerr << "dataset_inspect: unexpected argument '" << argument << "'\n";
            return 2;
        }
    }
    if (plotfile.empty()) {
        std::cerr << "dataset_inspect: a dataset path is required\n";
        return 2;
    }

    try {
        const auto result = amrvis::readDatasetMetadata(plotfile);
        if (json) {
            printJson(result);
        } else {
            printText(result);
        }
    } catch (const std::exception& error) {
        std::cerr << "dataset_inspect: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
