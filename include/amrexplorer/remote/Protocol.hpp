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

enum class PayloadKind : std::uint8_t {
    None,
    HelloRequest,
    HelloResponse,
    OpenDatasetRequest,
    DatasetOpened,
    CloseDatasetRequest,
    DatasetClosed,
    SliceViewRequest,
    SliceViewResponse,
    LineViewRequest,
    LineViewResponse,
    DatasetPageRequest,
    DatasetPageResponse,
    ParticleSampleRequest,
    ParticleSampleResponse,
    RangeRequest,
    RangeResponse,
    ClearCacheRequest,
    SetCacheBudgetRequest,
    CacheResponse,
    CancelRequest,
    CancelAcknowledged,
    PingRequest,
    PongResponse,
    ErrorResponse
};

enum class ErrorCode : std::uint16_t {
    UnsupportedProtocol,
    InvalidRequest,
    UnknownDataset,
    DatasetOpenFailure,
    Cancelled,
    CacheBudgetExceeded,
    ResourceLimitExceeded,
    OperationFailure,
    InternalServerError,
    Disconnected
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
};

struct HelloResponseData {
    std::string serverName;
    std::string softwareVersion;
    std::uint16_t selectedMinor = 0;
    std::uint32_t maximumFrameBytes = 0;
    std::uint32_t maximumDatasets = 0;
    std::uint32_t maximumOutstandingRequests = 0;
    std::uint32_t workerCount = 0;
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
