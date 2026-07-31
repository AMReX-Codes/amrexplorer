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

class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(std::string host, std::uint16_t port,
        ConnectionOptions options = {}, StopToken cancellation = {});
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] const HelloResponseData& serverInfo() const noexcept;
    [[nodiscard]] bool connected() const;
    [[nodiscard]] std::string disconnectReason() const;

    [[nodiscard]] OpenedDataset openDataset(const std::string& path,
        std::uint64_t cacheBudgetBytes, StopToken cancellation = {});
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
    void ping(StopToken cancellation = {});

    void close() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace amrvis::remote
