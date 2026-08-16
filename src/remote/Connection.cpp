#include <amrexplorer/remote/Connection.hpp>

#include "Codec.hpp"
#ifdef AMREXPLORER_SERVER_TEST_HOOKS
#include "ServerTestHooks.hpp"
#endif

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

enum class ResponseWait {
    Bounded,
    Indefinite
};

} // namespace

class Connection::Impl {
public:
    static std::unique_ptr<Impl> connectLoopback(const std::string& host,
        std::uint16_t port, ConnectionOptions options, StopToken cancellation)
    {
        const auto deadline
            = deadlineAfter(options.connectionTimeout, "connection timeout");
        auto channel = std::make_unique<Socket>(
            connectTo(host, port, deadline, cancellation));
        return std::make_unique<Impl>(
            std::move(channel), std::move(options), cancellation, deadline);
    }

    static std::unique_ptr<Impl> adopt(std::unique_ptr<Channel> channel,
        ConnectionOptions options, StopToken cancellation)
    {
        if (!channel) {
            throw std::invalid_argument("connection channel must not be null");
        }
        const auto deadline
            = deadlineAfter(options.connectionTimeout, "connection timeout");
        return std::make_unique<Impl>(
            std::move(channel), std::move(options), cancellation, deadline);
    }

    // The channel is already connected; the deadline bounds the handshake.
    Impl(std::unique_ptr<Channel> channel, ConnectionOptions options,
        StopToken cancellation,
        std::chrono::steady_clock::time_point handshakeDeadline)
        : m_connectionDeadline(handshakeDeadline)
        , m_channel(std::move(channel))
        , m_maximumFrameBytes(options.maximumFrameBytes)
        , m_requestTimeout(options.requestTimeout)
        , m_receiver([this] { receiveLoop(); })
    {
        try {
            if (m_requestTimeout <= std::chrono::milliseconds::zero()) {
                throw std::invalid_argument(
                    "request timeout must be greater than zero");
            }
            HelloRequestData hello;
            hello.clientName = std::move(options.clientName);
            hello.softwareVersion = std::move(options.softwareVersion);
            hello.minimumMinorVersion = 0;
            hello.maximumMinorVersion = protocolMinorVersion;
            hello.maximumFrameBytes = options.maximumFrameBytes;
            hello.sessionToken = std::move(options.sessionToken);
            const auto response = transact(codec::toWire(hello),
                PayloadKind::HelloResponse, cancellation,
                m_connectionDeadline);
            const auto* payload = response->payload.AsHelloResponse();
            if (payload == nullptr) {
                throw std::runtime_error(
                    "server returned the wrong hello payload");
            }
            m_serverInfo = codec::fromWire(*payload);
            if (m_serverInfo.selectedMinorVersion > protocolMinorVersion
                || m_serverInfo.maximumFrameBytes == 0) {
                throw std::runtime_error(
                    "server negotiated invalid capabilities");
            }
            m_selectedMinorVersion = m_serverInfo.selectedMinorVersion;
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
        PayloadKind expected, StopToken cancellation,
        ResponseWait responseWait = ResponseWait::Bounded)
    {
        const auto writeDeadline
            = deadlineAfter(m_requestTimeout, "request timeout");
        return transact(std::move(payload), expected, cancellation,
            writeDeadline,
            responseWait == ResponseWait::Indefinite
                ? std::chrono::steady_clock::time_point::max()
                : writeDeadline);
    }

    template <typename Payload>
    std::unique_ptr<NativeEnvelope> transact(Payload payload,
        PayloadKind expected, StopToken cancellation,
        std::chrono::steady_clock::time_point deadline)
    {
        return transact(std::move(payload), expected, cancellation,
            deadline, deadline);
    }

    template <typename Payload>
    std::unique_ptr<NativeEnvelope> transact(Payload payload,
        PayloadKind expected, StopToken cancellation,
        std::chrono::steady_clock::time_point writeDeadline,
        std::chrono::steady_clock::time_point responseDeadline)
    {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        m_transactions.fetch_add(1, std::memory_order_relaxed);
        const auto requestId = nextRequestId();
        auto pending = std::make_shared<Pending>();
        pending->expected = expected;
        auto future = pending->promise.get_future();
        {
            std::scoped_lock lock(m_stateMutex);
            ensureConnected();
            if (m_outstandingRequests
                    >= m_serverInfo.maximumOutstandingRequests
                && m_serverInfo.maximumOutstandingRequests != 0) {
                throw RemoteError(ErrorCode::ResourceLimitExceeded,
                    "connection has too many outstanding requests");
            }
            m_pending.emplace(requestId, pending);
            ++m_outstandingRequests;
        }
        try {
            auto bytes = codec::encode(
                requestId, std::move(payload), m_selectedMinorVersion);
            send(bytes, writeDeadline, cancellation);
        } catch (...) {
            erasePending(requestId);
            // A failed write may have emitted only part of the frame. Retire
            // the connection rather than allowing a later request to append
            // bytes to a corrupted stream.
            close();
            throw;
        }

        bool cancellationSent = false;
        while (future.wait_for(std::chrono::milliseconds(10))
            != std::future_status::ready) {
            if (future.wait_for(std::chrono::milliseconds::zero())
                == std::future_status::ready) {
                break;
            }
            if (cancellation.stop_requested() && !cancellationSent) {
                if (expected == PayloadKind::HelloResponse) {
                    erasePending(requestId);
                    close();
                    throw ReadCancelled();
                }
                cancellationSent = true;
                responseDeadline
                    = deadlineAfter(m_requestTimeout, "cancellation timeout");
                sendCancellation(requestId, responseDeadline);
            }
            if (future.wait_for(std::chrono::milliseconds::zero())
                == std::future_status::ready) {
                break;
            }
            if (std::chrono::steady_clock::now() >= responseDeadline) {
                erasePending(requestId);
                close();
                if (cancellationSent) {
                    throw ReadCancelled();
                }
                throw std::runtime_error(
                    expected == PayloadKind::HelloResponse
                        ? "remote handshake timed out"
                        : "remote request timed out");
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
        return response;
    }

    OpenedDataset openDataset(const std::string& path,
        std::uint64_t cacheBudgetBytes, StopToken cancellation)
    {
        const auto response = transact(codec::toWire(
            OpenDatasetData{path, cacheBudgetBytes}),
            PayloadKind::DatasetOpened, cancellation,
            ResponseWait::Indefinite);
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
                        PayloadKind::SliceViewResponse, cancellation,
                        ResponseWait::Indefinite);
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
                        PayloadKind::LineViewResponse, cancellation,
                        ResponseWait::Indefinite);
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
            PayloadKind::DatasetPageResponse, cancellation,
            ResponseWait::Indefinite);
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
            PayloadKind::RangeResponse, cancellation,
            ResponseWait::Indefinite);
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
            PayloadKind::ParticleSampleResponse, cancellation,
            ResponseWait::Indefinite);
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

    [[nodiscard]] std::uint64_t transactionCount() const noexcept
    {
        return m_transactions.load(std::memory_order_relaxed);
    }

    void ping(StopToken cancellation)
    {
        codec::fb::PingRequestT request;
        request.nonce = m_nextPingNonce.fetch_add(1);
        static_cast<void>(transact(
            std::move(request), PayloadKind::PongResponse, cancellation));
    }

    const HelloResponseData& serverInfo() const noexcept
    {
        return m_serverInfo;
    }

    bool connected() const
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
        std::scoped_lock closeLock(m_closeMutex);
        {
            std::scoped_lock lock(m_stateMutex);
            if (!m_connected && m_channelClosed) {
                return;
            }
            m_connected = false;
        }
        m_lifecycleStop.request_stop();
        if (m_receiver.joinable()
            && m_receiver.get_id() != std::this_thread::get_id()) {
            m_receiver.join();
        }
        {
            std::scoped_lock lock(m_sendMutex);
            m_channel->shutdown();
            m_channel->close();
        }
        {
            std::scoped_lock lock(m_stateMutex);
            m_channelClosed = true;
        }
        failPending("remote connection closed");
    }

private:
    struct Pending {
        PayloadKind expected = PayloadKind::None;
        bool countsAgainstBudget = true;
        std::promise<std::unique_ptr<NativeEnvelope>> promise;
    };

    static std::chrono::steady_clock::time_point deadlineAfter(
        std::chrono::milliseconds timeout, const char* description)
    {
        if (timeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument(
                std::string(description) + " must be greater than zero");
        }
        return std::chrono::steady_clock::now() + timeout;
    }

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
                    = readFrame(*m_channel, m_maximumFrameBytes.load(),
                          std::chrono::steady_clock::time_point::max(),
                          m_lifecycleStop.get_token());
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
                const char* protocolViolation = nullptr;
                {
                    std::scoped_lock lock(m_stateMutex);
                    const auto found = m_pending.find(info.requestId);
                    if (found == m_pending.end()) {
                        throw std::runtime_error(
                            "remote response has an unknown request ID");
                    }
                    pending = found->second;
                    if (pending->expected != PayloadKind::HelloResponse
                        && info.protocolMinorVersion
                            != m_selectedMinorVersion) {
                        protocolViolation
                            = "remote server changed protocol minor version";
                    } else if (info.payload != pending->expected
                        && info.payload != PayloadKind::ErrorResponse) {
                        protocolViolation
                            = "remote response has an unexpected payload type";
                    }
                    m_pending.erase(found);
                    if (pending->countsAgainstBudget) {
                        --m_outstandingRequests;
                    }
                    if (protocolViolation != nullptr) {
                        // Retire under the same lock that recognizes the
                        // violation, so detection and retirement are atomic.
                        // Setting m_connected below -- even immediately before
                        // waking the caller -- would leave a window in which an
                        // independent thread sharing this connection observes
                        // connected() == true and reuses one that has already
                        // seen a semantically impossible response.
                        m_connected = false;
                        // The catch handler fills the reason only when it is
                        // empty, so this early write wins and has to carry the
                        // real one.
                        if (m_disconnectReason.empty()) {
                            m_disconnectReason = protocolViolation;
                        }
                    }
                }
                if (protocolViolation != nullptr) {
                    pending->promise.set_exception(
                        std::make_exception_ptr(
                            std::runtime_error(protocolViolation)));
#ifdef AMREXPLORER_SERVER_TEST_HOOKS
                    // The caller is awake and this thread has not unwound: the
                    // window the retirement above closes. A test stalls here to
                    // observe it.
                    testing::notifyAfterViolationWake();
#endif
                    // Still throw: the handler below also fails the other
                    // outstanding requests and stops the lifecycle.
                    throw std::runtime_error(protocolViolation);
                }
                pending->promise.set_value(std::move(envelope));
            }
        } catch (const ReadCancelled&) {
            // close() owns pending-request failure and socket teardown after
            // the receiver has cooperatively left the readiness loop.
        } catch (const std::exception& error) {
            {
                std::scoped_lock lock(m_stateMutex);
                m_connected = false;
                if (m_disconnectReason.empty()) {
                    m_disconnectReason = error.what();
                }
            }
            m_lifecycleStop.request_stop();
            failPending(error.what());
        }
    }

    void send(const codec::Bytes& bytes,
        std::chrono::steady_clock::time_point deadline,
        StopToken cancellation = {})
    {
        std::scoped_lock lock(m_sendMutex);
        {
            std::scoped_lock stateLock(m_stateMutex);
            ensureConnected();
        }
        writeFrame(*m_channel, bytes, m_maximumFrameBytes.load(),
            deadline, cancellation, m_lifecycleStop.get_token());
    }

    void sendCancellation(std::uint64_t target,
        std::chrono::steady_clock::time_point deadline)
    {
        const auto cancelId = nextRequestId();
        auto pending = std::make_shared<Pending>();
        pending->expected = PayloadKind::CancelAcknowledged;
        pending->countsAgainstBudget = false;
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
            auto bytes = codec::encode(
                cancelId, std::move(request), m_selectedMinorVersion);
            send(bytes, deadline);
        } catch (...) {
            erasePending(cancelId);
            // Like an ordinary transaction write, a failed cancellation write
            // may have left a partial frame on the stream. No later request
            // can safely reuse this connection.
            close();
        }
    }

    void erasePending(std::uint64_t requestId)
    {
        std::scoped_lock lock(m_stateMutex);
        const auto found = m_pending.find(requestId);
        if (found != m_pending.end()) {
            if (found->second->countsAgainstBudget) {
                --m_outstandingRequests;
            }
            m_pending.erase(found);
        }
    }

    void failPending(const std::string& reason) noexcept
    {
        std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> pending;
        {
            std::scoped_lock lock(m_stateMutex);
            pending.swap(m_pending);
            m_outstandingRequests = 0;
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

    std::chrono::steady_clock::time_point m_connectionDeadline;
    std::unique_ptr<Channel> m_channel;
    std::uint16_t m_selectedMinorVersion = protocolMinorVersion;
    std::atomic<std::uint32_t> m_maximumFrameBytes;
    std::chrono::milliseconds m_requestTimeout;
    StopSource m_lifecycleStop;
    std::atomic<std::uint64_t> m_nextRequestId{1};
    // Every started transaction, including the ones that go on to fail. Kept
    // separately from m_nextRequestId, which is an identifier rather than a
    // count and is not otherwise observable.
    std::atomic<std::uint64_t> m_transactions{0};
    std::atomic<std::uint64_t> m_nextPingNonce{1};
    std::mutex m_closeMutex;
    mutable std::mutex m_stateMutex;
    bool m_connected = true;
    bool m_channelClosed = false;
    std::string m_disconnectReason;
    std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> m_pending;
    std::uint32_t m_outstandingRequests = 0;
    std::mutex m_sendMutex;
    mutable std::mutex m_cacheMutex;
    std::unordered_map<std::uint64_t, CacheMetrics> m_cache;
    HelloResponseData m_serverInfo;
    std::thread m_receiver;
};

Connection::Connection(std::string host, std::uint16_t port,
    ConnectionOptions options, StopToken cancellation)
    : m_impl(Impl::connectLoopback(host, port, std::move(options), cancellation))
{
}

Connection::Connection(std::unique_ptr<Channel> channel,
    ConnectionOptions options, StopToken cancellation)
    : m_impl(Impl::adopt(std::move(channel), std::move(options), cancellation))
{
}

Connection::~Connection() = default;

const HelloResponseData& Connection::serverInfo() const noexcept
{
    return m_impl->serverInfo();
}

bool Connection::connected() const
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

void Connection::closeDatasetBestEffort(DatasetId dataset) noexcept
{
    try {
        auto self = shared_from_this();
        std::thread([self = std::move(self), dataset] {
            try {
                self->closeDataset(dataset);
            } catch (...) {
                // A failed close acknowledgement leaves the server handle's
                // ownership uncertain. Closing the connection makes server
                // session teardown the final cleanup authority.
                self->close();
            }
        }).detach();
    } catch (...) {
        close();
    }
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

std::uint64_t Connection::transactionCount() const noexcept
{
    return m_impl->transactionCount();
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
