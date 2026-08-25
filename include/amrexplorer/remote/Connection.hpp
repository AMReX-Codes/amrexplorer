#pragma once

#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/data/DatasetPage.hpp>
#include <amrexplorer/data/DatasetSession.hpp>
#include <amrexplorer/data/ViewData.hpp>
#include <amrexplorer/remote/Frame.hpp>
#include <amrexplorer/remote/Protocol.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace amrvis::remote {

struct ConnectionOptions {
    std::string clientName = "AMReXplorer";
    std::string softwareVersion = "unknown";
    std::uint32_t maximumFrameBytes = defaultMaximumFrameBytes;
    std::chrono::milliseconds connectionTimeout{10000};
    // Bounds request writes, lightweight control responses, and cancellation
    // acknowledgement. Plotfile-operation responses have no wall-clock
    // deadline and remain governed by cancellation or disconnect instead.
    std::chrono::milliseconds requestTimeout{30000};
    // Access token printed by the server at startup. Required: a server always
    // enforces a token, so an empty value here is rejected at the handshake.
    std::string sessionToken;
};

// Thrown by both layers when the negotiated protocol predates volume
// rendering; one copy, because two would drift on the first protocol bump or
// reworded hint and give the same condition different words depending on
// which layer the caller entered through.
inline constexpr const char* volumeRenderingUnsupportedMessage
    = "the remote server predates volume rendering (protocol 1.2); install a "
      "current amrexplorer-server";
// Its narrower sibling: the server renders volumes but always sampled
// nearest, so smooth sampling is the one thing it cannot be asked for.
inline constexpr const char* volumeSamplingUnsupportedMessage
    = "the remote server predates smooth volume sampling (protocol 1.3) and "
      "samples each ray at the nearest voxel; install a current "
      "amrexplorer-server";
// And for derived fields, which a 1.3 server would not read off an open
// request at all -- so it would answer a catalog of stored fields while the
// client believed the definitions had been installed.
inline constexpr const char* derivedFieldsUnsupportedMessage
    = "the remote server predates derived fields (protocol 1.4); install a "
      "current amrexplorer-server";

class Connection : public std::enable_shared_from_this<Connection> {
public:
    // Connects to a loopback listener (tests and tools on one host).
    Connection(std::string host, std::uint16_t port,
        ConnectionOptions options = {}, StopToken cancellation = {});
    // Speaks the protocol over an already-connected channel, such as the
    // client's end of the stdio stream of an ssh-launched server. The
    // connection timeout then bounds only the handshake.
    Connection(std::unique_ptr<Channel> channel,
        ConnectionOptions options = {}, StopToken cancellation = {});
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] const HelloResponseData& serverInfo() const noexcept;
    [[nodiscard]] bool connected() const;
    [[nodiscard]] std::string disconnectReason() const;

    // `derivedFields` are installed by the server on the session it opens
    // (protocol 1.4). Ask supportsDerivedFields() first: a non-empty list
    // against an older peer throws, because a peer that cannot read the field
    // would answer a catalog of stored fields alone.
    [[nodiscard]] OpenedDataset openDataset(const std::string& path,
        std::uint64_t cacheBudgetBytes, StopToken cancellation = {},
        std::vector<DerivedFieldDefinition> derivedFields = {});
    // Lists the subdirectories of a server-side directory (protocol 1.1);
    // an empty path is the server's home. Throws when the server negotiated
    // protocol 1.0, i.e. predates browsing.
    [[nodiscard]] RemoteDirectoryListing listDirectory(
        const std::string& path, StopToken cancellation = {});
    // Whether the negotiated protocol carries volume rendering (1.2), asked
    // before the call so a caller can refuse the capability without the
    // failure looking like a misbehaving peer.
    [[nodiscard]] bool supportsVolumeRendering() const noexcept;
    // Whether the negotiated protocol carries the volume march's sampling
    // policy (1.3). A 1.2 peer renders volumes but always sampled nearest.
    [[nodiscard]] bool supportsVolumeSampling() const noexcept;
    // Whether the negotiated protocol carries derived-field definitions on an
    // open request (1.4). A 1.3 peer opens datasets perfectly well; it just
    // cannot be asked to compute a field.
    [[nodiscard]] bool supportsDerivedFields() const noexcept;
    // Renders a volume on the server and returns the frame (protocol 1.2).
    // Ask supportsVolumeRendering() first: this throws when the server
    // negotiated an older protocol.
    [[nodiscard]] VolumeFrame renderVolume(
        const VolumeRenderRequest& request, StopToken cancellation = {});
    void closeDataset(DatasetId dataset, StopToken cancellation = {});
    void closeDatasetBestEffort(DatasetId dataset) noexcept;
    [[nodiscard]] ViewDataResult requestView(
        const ViewDataRequest& request, StopToken cancellation = {});
    [[nodiscard]] DatasetPage requestDatasetPage(
        const DatasetPageRequest& request, StopToken cancellation = {});
    [[nodiscard]] std::optional<ValueRange> requestRange(DatasetId dataset,
        const RangeRequest& request, StopToken cancellation = {});
    [[nodiscard]] ParticleSample requestParticleSample(DatasetId dataset,
        const std::string& species, double fraction, std::uint64_t seed,
        StopToken cancellation = {});
    [[nodiscard]] CacheMetrics clearCache(
        DatasetId dataset, StopToken cancellation = {});
    [[nodiscard]] CacheMetrics setCacheBudget(DatasetId dataset,
        std::uint64_t bytes, StopToken cancellation = {});
    [[nodiscard]] CacheMetrics latestCache(DatasetId dataset) const;
    // Request/response pairs this connection has started, monotonically. The
    // number itself is not meaningful; the difference across an operation is,
    // which is how a test proves that a repeated query costs no round trip.
    [[nodiscard]] std::uint64_t transactionCount() const noexcept;
    void ping(StopToken cancellation = {});

    void close() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace amrvis::remote
