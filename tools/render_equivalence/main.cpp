#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>
#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/Frame.hpp>
#include <amrexplorer/remote/RemoteDatasetSession.hpp>
#include <amrexplorer/render2d/Palette.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t cacheBudgetBytes = 1024ULL * 1024ULL * 1024ULL;

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
};

struct Options {
    Endpoint endpoint;
    std::filesystem::path localPath;
    std::string remotePath;
    std::optional<std::string> fieldName;
    bool allFields = false;
    std::array<int, 2> viewport{800, 600};
};

struct ViewState {
    std::string_view name;
    double lowerFraction = 0.0;
    double upperFraction = 1.0;
};

constexpr std::array<ViewState, 7> viewStates{{
    {"full", 0.0, 1.0},
    {"zoom-2x-center", 0.25, 0.75},
    {"zoom-2x-pan-low", 0.0, 0.5},
    {"zoom-2x-pan-high", 0.5, 1.0},
    {"zoom-4x-center", 0.375, 0.625},
    {"zoom-4x-pan-low", 0.125, 0.375},
    {"zoom-4x-pan-high", 0.625, 0.875},
}};

[[noreturn]] void usageError(const std::string& message)
{
    throw std::invalid_argument(message + "\nusage: "
        "amrexplorer-render-equivalence --connect HOST:PORT --token-stdin "
        "--remote-path REMOTE_PLOTFILE [--field NAME | --all-fields] "
        "[--viewport WIDTHxHEIGHT] LOCAL_PLOTFILE");
}

Endpoint parseEndpoint(std::string_view text)
{
    std::string_view host;
    std::string_view portText;
    if (!text.empty() && text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string_view::npos || close + 2 > text.size()
            || text[close + 1] != ':') {
            usageError("invalid bracketed endpoint '" + std::string(text) + "'");
        }
        host = text.substr(1, close - 1);
        portText = text.substr(close + 2);
    } else {
        const auto separator = text.rfind(':');
        if (separator == std::string_view::npos
            || separator != text.find(':')) {
            usageError("endpoint must be HOST:PORT (bracket IPv6 addresses)");
        }
        host = text.substr(0, separator);
        portText = text.substr(separator + 1);
    }
    unsigned int port = 0;
    const auto [end, error] = std::from_chars(
        portText.data(), portText.data() + portText.size(), port);
    if (host.empty() || error != std::errc{}
        || end != portText.data() + portText.size()
        || port == 0 || port > 65535) {
        usageError("invalid endpoint '" + std::string(text) + "'");
    }
    if (!amrvis::remote::isNumericAddress(std::string(host))) {
        usageError("endpoint host must be a numeric loopback address");
    }
    return {std::string(host), static_cast<std::uint16_t>(port)};
}

std::array<int, 2> parseViewport(std::string_view text)
{
    const auto separator = text.find_first_of("xX");
    if (separator == std::string_view::npos) {
        usageError("viewport must be WIDTHxHEIGHT");
    }
    std::array<int, 2> viewport{};
    const std::array parts{text.substr(0, separator), text.substr(separator + 1)};
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto [end, error] = std::from_chars(parts[index].data(),
            parts[index].data() + parts[index].size(), viewport[index]);
        if (error != std::errc{}
            || end != parts[index].data() + parts[index].size()
            || viewport[index] < 1
            || viewport[index] > amrvis::maxViewOutputDimension) {
            usageError("viewport dimensions must be between 1 and 4096");
        }
    }
    return viewport;
}

Options parseArguments(int argc, char* argv[])
{
    Options options;
    bool tokenStdin = false;
    bool haveEndpoint = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto valueAfter = [&](std::string_view option) -> std::string_view {
            if (++index >= argc) {
                usageError("missing value after " + std::string(option));
            }
            return argv[index];
        };
        if (argument == "--connect") {
            options.endpoint = parseEndpoint(valueAfter(argument));
            haveEndpoint = true;
        } else if (argument == "--token-stdin") {
            tokenStdin = true;
        } else if (argument == "--remote-path") {
            options.remotePath = valueAfter(argument);
        } else if (argument == "--field") {
            options.fieldName = std::string(valueAfter(argument));
        } else if (argument == "--all-fields") {
            options.allFields = true;
        } else if (argument == "--viewport") {
            options.viewport = parseViewport(valueAfter(argument));
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "usage: amrexplorer-render-equivalence --connect "
                "HOST:PORT --token-stdin --remote-path REMOTE_PLOTFILE "
                "[--field NAME | --all-fields] [--viewport WIDTHxHEIGHT] "
                "LOCAL_PLOTFILE\n";
            std::exit(0);
        } else if (!argument.empty() && argument.front() == '-') {
            usageError("unknown option '" + std::string(argument) + "'");
        } else if (options.localPath.empty()) {
            options.localPath = argument;
        } else {
            usageError("unexpected argument '" + std::string(argument) + "'");
        }
    }
    if (!haveEndpoint || !tokenStdin || options.remotePath.empty()
        || options.localPath.empty()) {
        usageError("connect endpoint, token stdin, remote path, and local path are required");
    }
    if (options.fieldName && options.allFields) {
        usageError("--field and --all-fields are mutually exclusive");
    }
    return options;
}

void requireMatchingMetadata(
    const amrvis::DatasetMetadata& local, const amrvis::DatasetMetadata& remote)
{
    if (local.dimension != remote.dimension
        || local.finestLevel != remote.finestLevel
        || local.coordinateSystem != remote.coordinateSystem
        || local.physicalDomain != remote.physicalDomain
        || local.levels.size() != remote.levels.size()
        || local.fields.size() != remote.fields.size()) {
        throw std::runtime_error(
            "local and remote plotfile metadata differ; they are not the same dataset");
    }
    for (std::size_t index = 0; index < local.fields.size(); ++index) {
        if (local.fields[index].name != remote.fields[index].name
            || local.fields[index].centering != remote.fields[index].centering
            || local.fields[index].componentNames
                != remote.fields[index].componentNames) {
            throw std::runtime_error(
                "local and remote field metadata differ at field "
                + std::to_string(index));
        }
    }
    for (std::size_t index = 0; index < local.levels.size(); ++index) {
        if (local.levels[index].domain != remote.levels[index].domain
            || local.levels[index].cellSize != remote.levels[index].cellSize
            || local.levels[index].indexOrigin != remote.levels[index].indexOrigin) {
            throw std::runtime_error(
                "local and remote level geometry differ at level "
                + std::to_string(index));
        }
    }
}

std::vector<std::size_t> selectedFields(
    const Options& options, const amrvis::DatasetMetadata& metadata)
{
    if (metadata.fields.empty()) {
        throw std::runtime_error("plotfile contains no fields");
    }
    if (options.allFields) {
        std::vector<std::size_t> fields(metadata.fields.size());
        for (std::size_t index = 0; index < fields.size(); ++index) {
            fields[index] = index;
        }
        return fields;
    }
    if (options.fieldName) {
        const auto found = std::find_if(metadata.fields.begin(),
            metadata.fields.end(), [&](const auto& field) {
                return field.name == *options.fieldName;
            });
        if (found == metadata.fields.end()) {
            throw std::runtime_error(
                "field '" + *options.fieldName + "' is not present locally");
        }
        return {static_cast<std::size_t>(found - metadata.fields.begin())};
    }
    return {0};
}

amrvis::RealBox regionFor(const amrvis::RealBox& full, int dimension,
    int normal, const ViewState& state)
{
    auto region = full;
    const auto axes = amrvis::slicePlaneAxes(dimension, normal);
    for (const auto axis : axes) {
        const auto index = static_cast<std::size_t>(axis);
        const auto extent = full.upper[index] - full.lower[index];
        region.lower[index] = full.lower[index] + state.lowerFraction * extent;
        region.upper[index] = full.lower[index] + state.upperFraction * extent;
    }
    return region;
}

std::string planeDifference(
    const amrvis::ScalarPlane& local, const amrvis::ScalarPlane& remote)
{
    if (local.width != remote.width || local.height != remote.height) {
        return "plane dimensions differ";
    }
    if (local.physicalRegion != remote.physicalRegion) {
        return "plane physical regions differ";
    }
    if (local.values.size() != remote.values.size()) {
        return "sample value counts differ";
    }
    if (local.valid.size() != remote.valid.size()) {
        return "validity mask sizes differ";
    }
    if (local.sourceLevel.size() != remote.sourceLevel.size()) {
        return "source-level counts differ";
    }
    const auto firstDifference = [](const auto& lhs, const auto& rhs) {
        return std::mismatch(lhs.begin(), lhs.end(), rhs.begin(), rhs.end()).first;
    };
    if (local.values != remote.values) {
        const auto found = firstDifference(local.values, remote.values);
        const auto index = static_cast<std::size_t>(found - local.values.begin());
        std::ostringstream message;
        message << "sample values differ at pixel " << index
                << " (local=" << std::setprecision(9) << local.values[index]
                << ", remote=" << remote.values[index] << ')';
        return message.str();
    }
    if (local.valid != remote.valid) {
        const auto found = firstDifference(local.valid, remote.valid);
        return "validity masks differ at pixel "
            + std::to_string(found - local.valid.begin());
    }
    if (local.sourceLevel != remote.sourceLevel) {
        const auto found = firstDifference(local.sourceLevel, remote.sourceLevel);
        return "source levels differ at pixel "
            + std::to_string(found - local.sourceLevel.begin());
    }
    return {};
}

std::uint64_t imageHash(const amrvis::ImageBuffer& image)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto pixel : image.rgba) {
        hash ^= pixel;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void compare(const Options& options, const std::string& token)
{
    auto local = std::make_shared<amrvis::LocalDatasetSession>(
        options.localPath, amrvis::DatasetId{1}, cacheBudgetBytes);
    amrvis::remote::ConnectionOptions connectionOptions;
    connectionOptions.sessionToken = token;
    auto connection = std::make_shared<amrvis::remote::Connection>(
        options.endpoint.host, options.endpoint.port,
        std::move(connectionOptions));
    auto remote = amrvis::remote::RemoteDatasetSession::open(
        connection, options.remotePath, cacheBudgetBytes);

    requireMatchingMetadata(local->metadata(), remote->metadata());
    const auto fields = selectedFields(options, local->metadata());
    const auto full = amrvis::datasetSampleBounds(local->metadata());
    const auto dimension = local->metadata().dimension;
    const auto palette = amrvis::builtinPalette(amrvis::BuiltinPalette::Rainbow);
    const std::array<int, 3> normals{0, 1, 2};
    const auto normalCount = dimension == 3 ? normals.size() : std::size_t{1};
    std::size_t comparisons = 0;

    for (const auto field : fields) {
        for (std::size_t normalIndex = 0; normalIndex < normalCount;
             ++normalIndex) {
            const auto normal = dimension == 3 ? normals[normalIndex] : 1;
            for (const auto& state : viewStates) {
                amrvis::SliceRequest localRequest;
                localRequest.dataset = local->id();
                localRequest.field
                    = amrvis::FieldId{static_cast<std::uint32_t>(field)};
                localRequest.normalDirection = normal;
                localRequest.visibleRegion
                    = regionFor(full, dimension, normal, state);
                const auto normalAxis = static_cast<std::size_t>(normal);
                localRequest.physicalPosition = 0.5
                    * (full.lower[normalAxis] + full.upper[normalAxis]);
                localRequest.maximumLevel = local->metadata().finestLevel;
                localRequest.outputSize = amrvis::frameBudgetBoundedOutputSize(
                    amrvis::viewportBoundedOutputSize(local->metadata(),
                        localRequest.visibleRegion, normal, options.viewport),
                    remote->maximumResponseBytes());
                auto remoteRequest = localRequest;
                remoteRequest.dataset = remote->id();

                const auto localResult =
                    amrvis::executeSlice(local, localRequest, amrvis::RangeMode::File, std::nullopt,
                                         {amrvis::ColorScale::Linear}, palette, {});
                const auto remoteResult =
                    amrvis::executeSlice(remote, remoteRequest, amrvis::RangeMode::File,
                                         std::nullopt, {amrvis::ColorScale::Linear}, palette, {});
                const auto difference = planeDifference(
                    localResult.slice.plane, remoteResult.slice.plane);
                if (!difference.empty()) {
                    throw std::runtime_error("field '"
                        + local->metadata().fields[field].name + "', normal "
                        + std::to_string(normal) + ", "
                        + std::string(state.name) + ": " + difference);
                }
                if (localResult.minimum != remoteResult.minimum ||
                    localResult.maximum != remoteResult.maximum ||
                    localResult.scale != remoteResult.scale) {
                    throw std::runtime_error("field '"
                        + local->metadata().fields[field].name + "', normal "
                        + std::to_string(normal) + ", "
                        + std::string(state.name) + ": display ranges differ");
                }
                if (localResult.image.width != remoteResult.image.width
                    || localResult.image.height != remoteResult.image.height
                    || localResult.image.strideBytes
                        != remoteResult.image.strideBytes
                    || localResult.image.rgba != remoteResult.image.rgba) {
                    throw std::runtime_error("field '"
                        + local->metadata().fields[field].name + "', normal "
                        + std::to_string(normal) + ", "
                        + std::string(state.name)
                        + ": final RGBA rasters differ");
                }
                ++comparisons;
                std::cout << "PASS field="
                          << local->metadata().fields[field].name
                          << " normal=" << normal
                          << " view=" << state.name
                          << " size=" << localResult.image.width << 'x'
                          << localResult.image.height
                          << " hash=" << std::hex << imageHash(localResult.image)
                          << std::dec << '\n';
            }
        }
    }
    std::cout << "PASS: " << comparisons
              << " local/remote rendered views are byte-identical\n";
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parseArguments(argc, argv);
        std::string token;
        if (!std::getline(std::cin, token) || token.empty()) {
            throw std::runtime_error("--token-stdin requires a non-empty token line");
        }
        if (!token.empty() && token.back() == '\r') {
            token.pop_back();
        }
        compare(options, token);
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "argument error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "render equivalence FAILED: " << error.what() << '\n';
        return 1;
    }
}
