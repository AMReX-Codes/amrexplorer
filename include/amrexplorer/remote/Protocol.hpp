#pragma once

#include <amrexplorer/cache/CacheMetrics.hpp>
#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/data/DatasetPage.hpp>
#include <amrexplorer/data/DatasetSession.hpp>
#include <amrexplorer/data/ViewData.hpp>
#include <amrexplorer/io/ParticleReader.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace amrvis::remote {

inline constexpr std::uint16_t protocolMajor = 1;
inline constexpr std::uint16_t protocolMinor = 0;

enum class PayloadKind : std::uint8_t {
    None = 0,
    HelloRequest = 1,
    HelloResponse = 2,
    OpenDatasetRequest = 3,
    DatasetOpened = 4,
    CloseDatasetRequest = 5,
    DatasetClosed = 6,
    SliceViewRequest = 7,
    SliceViewResponse = 8,
    LineViewRequest = 9,
    LineViewResponse = 10,
    DatasetPageRequest = 11,
    DatasetPageResponse = 12,
    ParticleSampleRequest = 13,
    ParticleSampleResponse = 14,
    RangeRequest = 15,
    RangeResponse = 16,
    ClearCacheRequest = 17,
    SetCacheBudgetRequest = 18,
    CacheResponse = 19,
    CancelRequest = 20,
    CancelAcknowledged = 21,
    PingRequest = 22,
    PongResponse = 23,
    ErrorResponse = 24
};

enum class ErrorCode : std::uint16_t {
    UnsupportedProtocol = 0,
    InvalidRequest = 1,
    UnknownDataset = 2,
    DatasetOpenFailure = 3,
    Cancelled = 4,
    CacheBudgetExceeded = 5,
    ResourceLimitExceeded = 6,
    OperationFailure = 7,
    InternalServerError = 8,
    Disconnected = 9,
    Unauthorized = 10
};

struct EnvelopeInfo {
    std::uint16_t protocolMajor = 0;
    std::uint16_t protocolMinor = 0;
    std::uint64_t requestId = 0;
    PayloadKind payload = PayloadKind::None;
};

struct HelloRequestData {
    std::string clientName;
    std::string softwareVersion;
    std::uint16_t minimumMinor = 0;
    std::uint16_t maximumMinor = 0;
    std::uint32_t maximumFrameBytes = 0;
    std::string sessionToken;
    std::vector<std::uint64_t> capabilities;
};

struct HelloResponseData {
    std::string serverName;
    std::string softwareVersion;
    std::uint16_t selectedMinor = 0;
    std::uint32_t maximumFrameBytes = 0;
    std::uint32_t maximumDatasets = 0;
    std::uint32_t maximumOutstandingRequests = 0;
    std::uint32_t workerCount = 0;
    std::vector<std::uint64_t> capabilities;
};

struct OpenDatasetData {
    std::string path;
    std::uint64_t cacheBudgetBytes = 0;
};

struct OpenedDataset {
    DatasetId id;
    DatasetMetadata catalog;
    MetadataReadMetrics metadataMetrics;
    std::string fileVersion;
    std::vector<ParticleSpeciesMetadata> particleSpecies;
    std::vector<std::uint8_t> fileRangeAvailable;
    std::vector<std::uint8_t> levelRangeAvailable;
    CacheMetrics cache;
};

struct ErrorData {
    ErrorCode code = ErrorCode::OperationFailure;
    std::string message;
};

class RemoteError : public std::runtime_error {
public:
    RemoteError(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message))
        , m_code(code)
    {
    }

    [[nodiscard]] ErrorCode code() const noexcept { return m_code; }

private:
    ErrorCode m_code;
};

} // namespace amrvis::remote
