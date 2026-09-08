#pragma once

#include <amrexplorer/cache/CacheMetrics.hpp>
#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/data/DatasetPage.hpp>
#include <amrexplorer/data/DatasetSession.hpp>
#include <amrexplorer/data/ViewData.hpp>
#include <amrexplorer/io/ParticleReader.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace amrvis::remote {

inline constexpr std::uint16_t protocolMajor = 1;
// 1.1 added directory browsing; 1.2 adds volume rendering (RenderedFrame*);
// 1.3 adds the volume march's sampling policy (RenderedFrameRequest.sampling);
// 1.4 adds derived fields (OpenDatasetRequest.derived_fields, and the derived
// count and skip reasons on DatasetOpened).
//
// 1.3 is a version rather than a bare appended field because the field changes
// the picture. A 1.2 server ignores what it cannot read and renders nearest,
// so a client that simply sent it would offer a control that silently did
// nothing there -- the defect this codebase keeps designing against. The
// version lets the client ask first and say so instead.
//
// 1.4 is a version for the same reason, one step worse: a 1.3 server would
// answer a plain catalog, so the client would show an Expression Editor whose
// Apply reported success while every slice kept coming back from the stored
// fields alone.
inline constexpr std::uint16_t protocolMinorVersion = 5;

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
    ErrorResponse = 24,
    // Protocol 1.1: directory browsing.
    ListDirectoryRequest = 25,
    DirectoryListing = 26,
    // Protocol 1.2: volume rendering -- the server renders a viewport-sized
    // frame; volume field data never travels.
    RenderedFrameRequest = 27,
    RenderedFrameResponse = 28
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
    std::uint16_t protocolMinorVersion = 0;
    std::uint64_t requestId = 0;
    PayloadKind payload = PayloadKind::None;
};

struct HelloRequestData {
    std::string clientName;
    std::string softwareVersion;
    std::uint16_t minimumMinorVersion = 0;
    std::uint16_t maximumMinorVersion = 0;
    std::uint32_t maximumFrameBytes = 0;
    std::string sessionToken;
    std::vector<std::uint64_t> capabilities;
};

struct HelloResponseData {
    std::string serverName;
    std::string softwareVersion;
    std::uint16_t selectedMinorVersion = 0;
    std::uint32_t maximumFrameBytes = 0;
    std::uint32_t maximumDatasets = 0;
    std::uint32_t maximumOutstandingRequests = 0;
    std::uint32_t workerCount = 0;
    std::vector<std::uint64_t> capabilities;
};

struct OpenDatasetData {
    std::string path;
    std::uint64_t cacheBudgetBytes = 0;
    // Protocol 1.4. The fields the session should compute as well as read; the
    // server installs them under DerivedFieldPolicy::Skip, so one it cannot
    // resolve comes back in OpenedDataset::derivedFieldSkips rather than
    // failing the open.
    std::vector<DerivedFieldDefinition> derivedFields;
};

// One subdirectory of a listed directory. Only directories are listed:
// plotfiles are directories, and files are not navigable. `path` is the
// server-side absolute path of the entry.
struct RemoteDirectoryEntry {
    std::string name;
    std::string path;
    bool isPlotfile = false;
};

// The most subdirectories one DirectoryListing carries: the first this many
// in name order, with `truncated` set when more exist. A bound both sides
// enforce, so a listing cannot grow past what a dialog can show. It is part
// of protocol 1.1: a 1.1 client rejects a larger listing, so it cannot simply
// be raised -- a larger cap needs a new minor version and a server that sends
// at most this many to a peer that negotiated 1.1 (nothing does that yet).
inline constexpr std::size_t maximumDirectoryEntries = 4096;

// A directory as the server sees it, with `path` resolved and normalized the
// same way dataset paths are (see resolveDatasetPath in Server.cpp). At the
// root `parentPath` equals `path`. `truncated` is set when the listing was
// cut at maximumDirectoryEntries; the entries present are the first ones in
// name order.
struct RemoteDirectoryListing {
    std::string path;
    std::string parentPath;
    std::vector<RemoteDirectoryEntry> entries;
    bool truncated = false;
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
    // Protocol 1.4. How many of catalog.fields are computed rather than stored
    // (the last ones, in definition order), and the definitions the server
    // could not install. Zero and empty for a server that was sent none -- and
    // for one too old to have been sent any.
    std::uint32_t derivedFieldCount = 0;
    std::vector<DerivedFieldSkip> derivedFieldSkips;
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
