#include <amrexplorer/remote/Connection.hpp>

#include "Codec.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace amrvis::remote {
namespace {

using NativeEnvelope = codec::NativeEnvelope;

std::runtime_error disconnectedError(const std::string& reason)
{
    return std::runtime_error(
        reason.empty() ? "remote connection is closed" : reason);
}

} // namespace

class Connection::Impl {
public:
    Impl(std::string host, std::uint16_t port, ConnectionOptions options)
        : m_socket(connectTo(host, port))
        , m_maximumFrameBytes(options.maximumFrameBytes)
        , m_receiver([this] { receiveLoop(); })
    {
        try {
            HelloRequestData hello;
            hello.clientName = std::move(options.clientName);
            hello.softwareVersion = std::move(options.softwareVersion);
            hello.minimumMinor = 0;
            hello.maximumMinor = protocolMinor;
            hello.maximumFrameBytes = options.maximumFrameBytes;
            const auto response = transact(codec::toWire(hello),
                PayloadKind::HelloResponse, {});
            const auto* payload = response->payload.AsHelloResponse();
            if (payload == nullptr) {
                throw std::runtime_error(
                    "server returned the wrong hello payload");
            }
            m_serverInfo = codec::fromWire(*payload);
            if (m_serverInfo.selectedMinor > protocolMinor
                || m_serverInfo.maximumFrameBytes == 0) {
                throw std::runtime_error(
                    "server negotiated invalid capabilities");
            }
            m_maximumFrameBytes = std::min(
                m_maximumFrameBytes.load(),
                m_serverInfo.maximumFrameBytes);
        } catch (...) {
            close();
            throw;
        }
    }

    ~Impl()
    {
        close();
    }

    template <typename Payload>
    std::unique_ptr<NativeEnvelope> transact(Payload payload,
        PayloadKind expected, StopToken cancellation)
    {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto requestId = nextRequestId();
        auto pending = std::make_shared<Pending>();
        pending->expected = expected;
        auto future = pending->promise.get_future();
        {
            std::scoped_lock lock(m_stateMutex);
            ensureConnected();
            if (m_pending.size() >= m_serverInfo.maximumOutstandingRequests
                && m_serverInfo.maximumOutstandingRequests != 0) {
                throw RemoteError(ErrorCode::ResourceLimitExceeded,
                    "connection has too many outstanding requests");
            }
            m_pending.emplace(requestId, pending);
        }
        try {
            auto bytes = codec::encode(requestId, std::move(payload));
            send(bytes);
        } catch (...) {
            erasePending(requestId);
            throw;
        }

        bool cancellationSent = false;
        while (future.wait_for(std::chrono::milliseconds(10))
            != std::future_status::ready) {
            if (cancellation.stop_requested() && !cancellationSent) {
                cancellationSent = true;
                sendCancellation(requestId);
            }
        }
        auto response = future.get();
        if (const auto* error = response->payload.AsErrorResponse()) {
            const auto decoded = codec::fromWire(*error);
            if (decoded.code == ErrorCode::Cancelled) {
                throw ReadCancelled();
            }
            if (decoded.code == ErrorCode::CacheBudgetExceeded) {
                throw CacheBudgetExceeded(decoded.message);
            }
            throw RemoteError(decoded.code, decoded.message);
        }
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        return response;
    }

    OpenedDataset openDataset(const std::string& path,
        std::uint64_t cacheBudgetBytes, StopToken cancellation)
    {
        const auto response = transact(codec::toWire(
            OpenDatasetData{path, cacheBudgetBytes}),
            PayloadKind::DatasetOpened, cancellation);
        const auto* payload = response->payload.AsDatasetOpened();
        if (payload == nullptr) {
            throw std::runtime_error("server omitted dataset-open payload");
        }
        auto opened = codec::fromWire(*payload);
        updateCache(opened.id, opened.cache);
        return opened;
    }

    void closeDataset(DatasetId dataset, StopToken cancellation)
    {
        codec::fb::CloseDatasetRequestT request;
        request.dataset_id = dataset.value;
        static_cast<void>(transact(std::move(request),
            PayloadKind::DatasetClosed, cancellation));
        std::scoped_lock lock(m_cacheMutex);
        m_cache.erase(dataset.value);
    }

    ViewDataResult requestView(
        const ViewDataRequest& request, StopToken cancellation)
    {
        return std::visit(
            [&](const auto& typed) -> ViewDataResult {
                using Request = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Request, SliceRequest>) {
                    const auto response = transact(codec::toWire(typed),
                        PayloadKind::SliceViewResponse, cancellation);
                    const auto* payload
                        = response->payload.AsSliceViewResponse();
                    if (payload == nullptr) {
                        throw std::runtime_error(
                            "server omitted slice-view payload");
                    }
                    updateCache(typed.dataset,
                        codec::fromWire(payload->cache.get()));
                    return codec::fromWire(*payload);
                } else {
                    const auto response = transact(codec::toWire(typed),
                        PayloadKind::LineViewResponse, cancellation);
                    const auto* payload
                        = response->payload.AsLineViewResponse();
                    if (payload == nullptr) {
                        throw std::runtime_error(
                            "server omitted line-view payload");
                    }
                    updateCache(typed.query.dataset,
                        codec::fromWire(payload->cache.get()));
                    return codec::fromWire(*payload);
                }
            },
            request);
    }

    DatasetPage requestDatasetPage(
        const DatasetPageRequest& request, StopToken cancellation)
    {
        const auto response = transact(codec::toWire(request),
            PayloadKind::DatasetPageResponse, cancellation);
        const auto* payload = response->payload.AsDatasetPageResponse();
        if (payload == nullptr) {
            throw std::runtime_error("server omitted dataset-page payload");
        }
        updateCache(
            request.dataset, codec::fromWire(payload->cache.get()));
        return codec::fromWire(*payload);
    }

    std::optional<ValueRange> requestRange(DatasetId dataset,
        const RangeRequest& request, StopToken cancellation)
    {
        const auto response = transact(codec::toWire(dataset, request),
            PayloadKind::RangeResponse, cancellation);
        const auto* payload = response->payload.AsRangeResponse();
        if (payload == nullptr) {
            throw std::runtime_error("server omitted range payload");
        }
        updateCache(dataset, codec::fromWire(payload->cache.get()));
        return codec::fromWire(*payload);
    }

    ParticleSample requestParticleSample(DatasetId dataset,
        const std::string& species, double fraction, std::uint64_t seed,
        StopToken cancellation)
    {
        const auto response = transact(
            codec::toWire(dataset, species, fraction, seed),
            PayloadKind::ParticleSampleResponse, cancellation);
        const auto* payload = response->payload.AsParticleSampleResponse();
        if (payload == nullptr) {
            throw std::runtime_error(
                "server omitted particle-sample payload");
        }
        updateCache(dataset, codec::fromWire(payload->cache.get()));
        return codec::fromWire(*payload);
    }

    CacheMetrics clearCache(DatasetId dataset, StopToken cancellation)
    {
        codec::fb::ClearCacheRequestT request;
        request.dataset_id = dataset.value;
        return cacheRequest(
            dataset, std::move(request), cancellation);
    }

    CacheMetrics setCacheBudget(DatasetId dataset,
        std::uint64_t bytes, StopToken cancellation)
    {
        codec::fb::SetCacheBudgetRequestT request;
        request.dataset_id = dataset.value;
        request.budget_bytes = bytes;
        return cacheRequest(
            dataset, std::move(request), cancellation);
    }

    CacheMetrics latestCache(DatasetId dataset) const
    {
        std::scoped_lock lock(m_cacheMutex);
        const auto found = m_cache.find(dataset.value);
        return found == m_cache.end() ? CacheMetrics{} : found->second;
    }

    void ping(StopToken cancellation)
    {
        codec::fb::PingRequestT request;
        request.nonce = nextRequestId();
        static_cast<void>(transact(
            std::move(request), PayloadKind::PongResponse, cancellation));
    }

    const HelloResponseData& serverInfo() const noexcept
    {
        return m_serverInfo;
    }

    bool connected() const noexcept
    {
        std::scoped_lock lock(m_stateMutex);
        return m_connected;
    }

    std::string disconnectReason() const
    {
        std::scoped_lock lock(m_stateMutex);
        return m_disconnectReason;
    }

    void close() noexcept
    {
        {
            std::scoped_lock lock(m_stateMutex);
            if (!m_connected && !m_socket.valid()) {
                return;
            }
            m_connected = false;
        }
        m_socket.shutdown();
        if (m_receiver.joinable()
            && m_receiver.get_id() != std::this_thread::get_id()) {
            m_receiver.join();
        }
        m_socket.close();
        failPending("remote connection closed");
    }

private:
    struct Pending {
        PayloadKind expected = PayloadKind::None;
        std::promise<std::unique_ptr<NativeEnvelope>> promise;
    };

    template <typename Payload>
    CacheMetrics cacheRequest(
        DatasetId dataset, Payload request, StopToken cancellation)
    {
        const auto response = transact(std::move(request),
            PayloadKind::CacheResponse, cancellation);
        const auto* payload = response->payload.AsCacheResponse();
        if (payload == nullptr || payload->dataset_id != dataset.value) {
            throw std::runtime_error("server returned invalid cache state");
        }
        const auto cache = codec::fromWire(payload->cache.get());
        updateCache(dataset, cache);
        return cache;
    }

    void receiveLoop() noexcept
    {
        try {
            for (;;) {
                const auto frame
                    = readFrame(m_socket, m_maximumFrameBytes.load());
                if (!frame) {
                    throw std::runtime_error("remote server closed connection");
                }
                auto envelope = codec::decode(*frame);
                const auto info = codec::inspect(*envelope);
                if (info.protocolMajor != protocolMajor) {
                    throw std::runtime_error(
                        "remote server changed protocol major version");
                }
                std::shared_ptr<Pending> pending;
                {
                    std::scoped_lock lock(m_stateMutex);
                    const auto found = m_pending.find(info.requestId);
                    if (found == m_pending.end()) {
                        throw std::runtime_error(
                            "remote response has an unknown request ID");
                    }
                    pending = found->second;
                    m_pending.erase(found);
                }
                if (info.payload != pending->expected
                    && info.payload != PayloadKind::ErrorResponse) {
                    throw std::runtime_error(
                        "remote response has an unexpected payload type");
                }
                pending->promise.set_value(std::move(envelope));
            }
        } catch (const std::exception& error) {
            {
                std::scoped_lock lock(m_stateMutex);
                m_connected = false;
                if (m_disconnectReason.empty()) {
                    m_disconnectReason = error.what();
                }
            }
            m_socket.shutdown();
            failPending(error.what());
        }
    }

    void send(const codec::Bytes& bytes)
    {
        std::scoped_lock lock(m_sendMutex);
        {
            std::scoped_lock stateLock(m_stateMutex);
            ensureConnected();
        }
        writeFrame(m_socket, bytes, m_maximumFrameBytes.load());
    }

    void sendCancellation(std::uint64_t target)
    {
        const auto cancelId = nextRequestId();
        auto pending = std::make_shared<Pending>();
        pending->expected = PayloadKind::CancelAcknowledged;
        {
            std::scoped_lock lock(m_stateMutex);
            if (!m_connected) {
                return;
            }
            m_pending.emplace(cancelId, pending);
        }
        codec::fb::CancelRequestT request;
        request.target_request_id = target;
        try {
            auto bytes = codec::encode(cancelId, std::move(request));
            send(bytes);
        } catch (...) {
            erasePending(cancelId);
        }
    }

    void erasePending(std::uint64_t requestId)
    {
        std::scoped_lock lock(m_stateMutex);
        m_pending.erase(requestId);
    }

    void failPending(const std::string& reason) noexcept
    {
        std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> pending;
        {
            std::scoped_lock lock(m_stateMutex);
            pending.swap(m_pending);
        }
        for (auto& [id, operation] : pending) {
            static_cast<void>(id);
            try {
                operation->promise.set_exception(
                    std::make_exception_ptr(disconnectedError(reason)));
            } catch (...) {
            }
        }
    }

    void ensureConnected() const
    {
        if (!m_connected) {
            throw disconnectedError(m_disconnectReason);
        }
    }

    std::uint64_t nextRequestId()
    {
        return m_nextRequestId.fetch_add(1);
    }

    void updateCache(DatasetId dataset, const CacheMetrics& cache)
    {
        std::scoped_lock lock(m_cacheMutex);
        m_cache[dataset.value] = cache;
    }

    Socket m_socket;
    std::atomic<std::uint32_t> m_maximumFrameBytes;
    std::atomic<std::uint64_t> m_nextRequestId{1};
    mutable std::mutex m_stateMutex;
    bool m_connected = true;
    std::string m_disconnectReason;
    std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> m_pending;
    std::mutex m_sendMutex;
    mutable std::mutex m_cacheMutex;
    std::unordered_map<std::uint64_t, CacheMetrics> m_cache;
    HelloResponseData m_serverInfo;
    std::thread m_receiver;
};

Connection::Connection(std::string host, std::uint16_t port,
    ConnectionOptions options)
    : m_impl(std::make_unique<Impl>(
          std::move(host), port, std::move(options)))
{
}

Connection::~Connection() = default;

const HelloResponseData& Connection::serverInfo() const noexcept
{
    return m_impl->serverInfo();
}

bool Connection::connected() const noexcept
{
    return m_impl->connected();
}

std::string Connection::disconnectReason() const
{
    return m_impl->disconnectReason();
}

OpenedDataset Connection::openDataset(const std::string& path,
    std::uint64_t cacheBudgetBytes, StopToken cancellation)
{
    return m_impl->openDataset(path, cacheBudgetBytes, cancellation);
}

void Connection::closeDataset(DatasetId dataset, StopToken cancellation)
{
    m_impl->closeDataset(dataset, cancellation);
}

ViewDataResult Connection::requestView(
    const ViewDataRequest& request, StopToken cancellation)
{
    return m_impl->requestView(request, cancellation);
}

DatasetPage Connection::requestDatasetPage(
    const DatasetPageRequest& request, StopToken cancellation)
{
    return m_impl->requestDatasetPage(request, cancellation);
}

std::optional<ValueRange> Connection::requestRange(DatasetId dataset,
    const RangeRequest& request, StopToken cancellation)
{
    return m_impl->requestRange(dataset, request, cancellation);
}

ParticleSample Connection::requestParticleSample(DatasetId dataset,
    const std::string& species, double fraction, std::uint64_t seed,
    StopToken cancellation)
{
    return m_impl->requestParticleSample(
        dataset, species, fraction, seed, cancellation);
}

CacheMetrics Connection::clearCache(
    DatasetId dataset, StopToken cancellation)
{
    return m_impl->clearCache(dataset, cancellation);
}

CacheMetrics Connection::setCacheBudget(
    DatasetId dataset, std::uint64_t bytes, StopToken cancellation)
{
    return m_impl->setCacheBudget(dataset, bytes, cancellation);
}

CacheMetrics Connection::latestCache(DatasetId dataset) const
{
    return m_impl->latestCache(dataset);
}

void Connection::ping(StopToken cancellation)
{
    m_impl->ping(cancellation);
}

void Connection::close() noexcept
{
    m_impl->close();
}

} // namespace amrvis::remote
