#include <amrexplorer/remote/Server.hpp>

#include "Codec.hpp"

#include <amrexplorer/data/LocalDatasetSession.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace amrvis::remote {
namespace {

class JoiningThread {
public:
    template <typename Function>
    explicit JoiningThread(Function&& function)
        : m_thread(std::forward<Function>(function))
    {
    }

    ~JoiningThread()
    {
        join();
    }

    JoiningThread(const JoiningThread&) = delete;
    JoiningThread& operator=(const JoiningThread&) = delete;

    JoiningThread(JoiningThread&&) noexcept = default;

    JoiningThread& operator=(JoiningThread&& other) noexcept
    {
        if (this != &other) {
            join();
            m_thread = std::move(other.m_thread);
        }
        return *this;
    }

private:
    void join() noexcept
    {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    std::thread m_thread;
};

class ThreadPool {
public:
    explicit ThreadPool(unsigned int count)
    {
        count = std::max(1U, count);
        m_threads.reserve(count);
        for (unsigned int index = 0; index < count; ++index) {
            m_threads.emplace_back([this] { worker(); });
        }
    }

    ~ThreadPool()
    {
        {
            std::scoped_lock lock(m_mutex);
            m_stopping = true;
        }
        m_ready.notify_all();
    }

    void submit(std::function<void()> task)
    {
        {
            std::scoped_lock lock(m_mutex);
            if (m_stopping) {
                throw std::runtime_error("server worker pool is stopping");
            }
            m_tasks.push_back(std::move(task));
        }
        m_ready.notify_one();
    }

private:
    void worker() noexcept
    {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(m_mutex);
                m_ready.wait(lock,
                    [&] { return m_stopping || !m_tasks.empty(); });
                if (m_stopping && m_tasks.empty()) {
                    return;
                }
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            try {
                task();
            } catch (...) {
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<std::function<void()>> m_tasks;
    bool m_stopping = false;
    std::vector<JoiningThread> m_threads;
};

ErrorData classifyError(const std::exception& error)
{
    if (const auto* remote = dynamic_cast<const RemoteError*>(&error)) {
        return {remote->code(), remote->what()};
    }
    if (dynamic_cast<const ReadCancelled*>(&error) != nullptr) {
        return {ErrorCode::Cancelled, error.what()};
    }
    if (dynamic_cast<const CacheBudgetExceeded*>(&error) != nullptr) {
        return {ErrorCode::CacheBudgetExceeded, error.what()};
    }
    if (dynamic_cast<const ParticleSampleLimitExceeded*>(&error) != nullptr) {
        return {ErrorCode::ResourceLimitExceeded, error.what()};
    }
    if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr) {
        return {ErrorCode::InvalidRequest, error.what()};
    }
    return {ErrorCode::OperationFailure, error.what()};
}

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(Socket socket, ThreadPool& workers, const ServerOptions& options)
        : m_socket(std::move(socket))
        , m_workers(workers)
        , m_options(options)
        , m_handshakeDeadline(
              std::chrono::steady_clock::now() + options.handshakeTimeout)
        , m_maximumFrameBytes(options.maximumFrameBytes)
    {
    }

    void run() noexcept
    {
        try {
            while (!m_stopping.load()) {
                const auto frame = m_handshakeComplete
                    ? readFrame(m_socket, m_maximumFrameBytes.load())
                    : readFrame(m_socket,
                          std::min(m_options.maximumFrameBytes,
                              m_options.maximumHandshakeFrameBytes),
                          m_handshakeDeadline);
                if (!frame) {
                    break;
                }
                auto envelope = codec::decode(*frame);
                dispatch(std::move(envelope));
            }
        } catch (...) {
        }
        stop();
    }

    void stop() noexcept
    {
        if (m_stopping.exchange(true)) {
            return;
        }
        m_socket.shutdown();
        std::vector<StopSource> stops;
        std::vector<std::shared_ptr<LocalDatasetSession>> datasets;
        {
            std::scoped_lock lock(m_stateMutex);
            for (const auto& [id, active] : m_active) {
                static_cast<void>(id);
                stops.push_back(active.stop);
            }
            datasets.reserve(m_datasets.size());
            for (auto& [id, dataset] : m_datasets) {
                static_cast<void>(id);
                datasets.push_back(std::move(dataset));
            }
            m_datasets.clear();
        }
        for (auto& stop : stops) {
            stop.request_stop();
        }
        for (const auto& dataset : datasets) {
            dataset->close();
        }
    }

    [[nodiscard]] bool stopped() const noexcept
    {
        return m_stopping.load();
    }

private:
    struct ActiveRequest {
        DatasetId dataset;
        StopSource stop;
    };

    void dispatch(std::unique_ptr<codec::NativeEnvelope> envelope)
    {
        const auto info = codec::inspect(*envelope);
        if (info.protocolMajor != protocolMajor) {
            sendError(info.requestId, {ErrorCode::UnsupportedProtocol,
                "unsupported protocol major version"});
            stop();
            return;
        }
        if (!m_handshakeComplete) {
            if (info.payload != PayloadKind::HelloRequest) {
                sendError(info.requestId, {ErrorCode::InvalidRequest,
                    "hello must be the first request"});
                stop();
                return;
            }
            handleHello(*envelope);
            return;
        }
        if (info.protocolMinor != m_selectedMinor) {
            sendError(info.requestId, {ErrorCode::UnsupportedProtocol,
                "message uses a non-negotiated protocol minor version"});
            stop();
            return;
        }
        if (info.payload == PayloadKind::CancelRequest) {
            handleCancel(*envelope);
            return;
        }
        if (info.payload == PayloadKind::PingRequest) {
            const auto* request = envelope->payload.AsPingRequest();
            codec::fb::PongResponseT response;
            response.nonce = request == nullptr ? 0 : request->nonce;
            send(info.requestId, std::move(response));
            return;
        }
        const auto dataset = requestDataset(*envelope);
        StopSource stopSource;
        enum class Rejection {
            None,
            OutstandingLimit,
            Duplicate
        };
        Rejection rejection = Rejection::None;
        {
            std::scoped_lock lock(m_stateMutex);
            if (m_active.contains(info.requestId)) {
                rejection = Rejection::Duplicate;
            } else if (m_active.size()
                >= m_options.maximumOutstandingRequests) {
                rejection = Rejection::OutstandingLimit;
            } else {
                m_active.emplace(info.requestId,
                    ActiveRequest{dataset, stopSource});
            }
        }
        if (rejection == Rejection::OutstandingLimit) {
            sendError(info.requestId, {ErrorCode::ResourceLimitExceeded,
                "too many outstanding requests"});
            return;
        }
        if (rejection == Rejection::Duplicate) {
            sendError(info.requestId, {ErrorCode::InvalidRequest,
                "duplicate live request ID"});
            stop();
            return;
        }
        auto self = shared_from_this();
        auto sharedEnvelope
            = std::shared_ptr<codec::NativeEnvelope>(std::move(envelope));
        m_workers.submit([self, sharedEnvelope, stopSource]() {
            self->handle(sharedEnvelope, stopSource.get_token());
        });
    }

    void handleHello(const codec::NativeEnvelope& envelope)
    {
        const auto* request = envelope.payload.AsHelloRequest();
        if (request == nullptr || request->maximum_frame_bytes == 0
            || request->minimum_minor > protocolMinor
            || request->maximum_minor < request->minimum_minor) {
            sendError(envelope.request_id, {ErrorCode::UnsupportedProtocol,
                "client protocol range is unsupported"});
            stop();
            return;
        }
        m_selectedMinor
            = std::min<std::uint16_t>(protocolMinor, request->maximum_minor);
        m_maximumFrameBytes = std::min(
            m_options.maximumFrameBytes, request->maximum_frame_bytes);
        HelloResponseData response;
        response.serverName = "AMReXplorer server";
        response.softwareVersion = m_options.softwareVersion;
        response.selectedMinor = m_selectedMinor;
        response.maximumFrameBytes = m_maximumFrameBytes.load();
        response.maximumDatasets = m_options.maximumDatasets;
        response.maximumOutstandingRequests
            = m_options.maximumOutstandingRequests;
        response.workerCount = m_options.workerCount;
        send(envelope.request_id, codec::toWire(response));
        m_handshakeComplete = true;
    }

    void handleCancel(const codec::NativeEnvelope& envelope)
    {
        const auto* request = envelope.payload.AsCancelRequest();
        bool accepted = false;
        StopSource activeStop;
        if (request != nullptr) {
            std::scoped_lock lock(m_stateMutex);
            const auto found = m_active.find(request->target_request_id);
            if (found != m_active.end()) {
                activeStop = found->second.stop;
                accepted = true;
            }
        }
        if (accepted) {
            activeStop.request_stop();
        }
        codec::fb::CancelAcknowledgedT response;
        response.target_request_id
            = request == nullptr ? 0 : request->target_request_id;
        response.accepted = accepted;
        send(envelope.request_id, std::move(response));
    }

    void handle(std::shared_ptr<codec::NativeEnvelope> envelope,
        StopToken cancellation) noexcept
    {
        const auto info = codec::inspect(*envelope);
        try {
            switch (info.payload) {
            case PayloadKind::OpenDatasetRequest:
                openDataset(*envelope, cancellation);
                break;
            case PayloadKind::CloseDatasetRequest:
                closeDataset(*envelope);
                break;
            case PayloadKind::SliceViewRequest:
                sliceView(*envelope, cancellation);
                break;
            case PayloadKind::LineViewRequest:
                lineView(*envelope, cancellation);
                break;
            case PayloadKind::DatasetPageRequest:
                datasetPage(*envelope, cancellation);
                break;
            case PayloadKind::ParticleSampleRequest:
                particleSample(*envelope, cancellation);
                break;
            case PayloadKind::RangeRequest:
                range(*envelope, cancellation);
                break;
            case PayloadKind::ClearCacheRequest:
                clearCache(*envelope);
                break;
            case PayloadKind::SetCacheBudgetRequest:
                setCacheBudget(*envelope);
                break;
            default:
                throw std::invalid_argument(
                    "payload is not a supported client request");
            }
        } catch (const std::exception& error) {
            sendError(envelope->request_id, classifyError(error));
        } catch (...) {
            sendError(envelope->request_id,
                {ErrorCode::InternalServerError,
                    "unknown server operation failure"});
        }
        std::scoped_lock lock(m_stateMutex);
        m_active.erase(envelope->request_id);
    }

    void openDataset(
        const codec::NativeEnvelope& envelope, StopToken cancellation)
    {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto* request = envelope.payload.AsOpenDatasetRequest();
        if (request == nullptr || request->path.empty()) {
            throw std::invalid_argument("dataset path is empty");
        }
        {
            std::scoped_lock lock(m_stateMutex);
            if (m_stopping.load() || cancellation.stop_requested()) {
                throw ReadCancelled();
            }
            if (m_datasets.size() + m_datasetReservations
                >= m_options.maximumDatasets) {
                throw RemoteError(ErrorCode::ResourceLimitExceeded,
                    "session dataset limit exceeded");
            }
            ++m_datasetReservations;
        }

        const auto id = DatasetId{m_nextDatasetId.fetch_add(1)};
        std::shared_ptr<LocalDatasetSession> dataset;
        bool reservationActive = true;
        try {
            dataset = std::make_shared<LocalDatasetSession>(
                request->path, id, request->cache_budget_bytes, cancellation);
            OpenedDataset opened;
            opened.id = id;
            opened.catalog = dataset->metadata();
            opened.metadataMetrics = dataset->metadataReadMetrics();
            opened.fileVersion = dataset->fileVersion();
            opened.particleSpecies = dataset->particleSpecies();
            opened.fileRangeAvailable.reserve(opened.catalog.fields.size());
            opened.levelRangeAvailable.reserve(opened.catalog.fields.size()
                * opened.catalog.levels.size());
            for (std::size_t field = 0;
                 field < opened.catalog.fields.size(); ++field) {
                if (cancellation.stop_requested()) {
                    throw ReadCancelled();
                }
                const auto fieldId
                    = FieldId{static_cast<std::uint32_t>(field)};
                opened.fileRangeAvailable.push_back(
                    dataset->rangeAvailable(RangeRequest{fieldId,
                        opened.catalog.finestLevel,
                        CompositionPolicy::FinestAvailable,
                        RangeScope::File}));
                for (int level = 0;
                     level <= opened.catalog.finestLevel; ++level) {
                    opened.levelRangeAvailable.push_back(
                        dataset->rangeAvailable(RangeRequest{fieldId, level,
                            CompositionPolicy::ExactLevel,
                            RangeScope::Level}));
                }
            }
            opened.cache = dataset->cacheMetrics();
            if (codec::encode(envelope.request_id, codec::toWire(opened),
                    m_selectedMinor)
                    .size()
                > m_maximumFrameBytes.load()) {
                throw RemoteError(ErrorCode::ResourceLimitExceeded,
                    "dataset catalog cannot fit in one negotiated frame");
            }

            bool published = false;
            {
                std::scoped_lock lock(m_stateMutex);
                --m_datasetReservations;
                reservationActive = false;
                if (!m_stopping.load() && !cancellation.stop_requested()) {
                    published = m_datasets.emplace(id.value, dataset).second;
                }
            }
            if (!published) {
                throw ReadCancelled();
            }
            send(envelope.request_id, codec::toWire(opened));
            return;
        } catch (const ReadCancelled&) {
            if (reservationActive) {
                std::scoped_lock lock(m_stateMutex);
                --m_datasetReservations;
            }
            if (dataset) {
                dataset->close();
            }
            throw;
        } catch (const RemoteError&) {
            if (reservationActive) {
                std::scoped_lock lock(m_stateMutex);
                --m_datasetReservations;
            }
            if (dataset) {
                dataset->close();
            }
            throw;
        } catch (const std::exception& error) {
            if (reservationActive) {
                std::scoped_lock lock(m_stateMutex);
                --m_datasetReservations;
            }
            if (dataset) {
                dataset->close();
            }
            throw RemoteError(ErrorCode::DatasetOpenFailure, error.what());
        }
    }

    void closeDataset(const codec::NativeEnvelope& envelope)
    {
        const auto* request = envelope.payload.AsCloseDatasetRequest();
        if (request == nullptr) {
            throw std::invalid_argument("close-dataset payload is missing");
        }
        std::shared_ptr<LocalDatasetSession> dataset;
        std::vector<StopSource> stops;
        {
            std::scoped_lock lock(m_stateMutex);
            const auto found = m_datasets.find(request->dataset_id);
            if (found == m_datasets.end()) {
                throw RemoteError(
                    ErrorCode::UnknownDataset, "dataset handle is unknown");
            }
            dataset = std::move(found->second);
            m_datasets.erase(found);
            for (const auto& [id, active] : m_active) {
                static_cast<void>(id);
                if (active.dataset.value == request->dataset_id) {
                    stops.push_back(active.stop);
                }
            }
        }
        for (auto& stop : stops) {
            stop.request_stop();
        }
        dataset->close();
        codec::fb::DatasetClosedT response;
        response.dataset_id = request->dataset_id;
        send(envelope.request_id, std::move(response));
    }

    void sliceView(
        const codec::NativeEnvelope& envelope, StopToken cancellation)
    {
        const auto* payload = envelope.payload.AsSliceViewRequest();
        if (payload == nullptr) {
            throw std::invalid_argument("slice-view payload is missing");
        }
        const auto request = codec::fromWire(*payload);
        validateSliceBound(request);
        const auto dataset = requireDataset(request.dataset);
        const auto result = std::get<SliceQueryResult>(
            dataset->requestView(ViewDataRequest{request}, cancellation));
        send(envelope.request_id,
            codec::toWire(result, dataset->cacheMetrics()));
    }

    void lineView(
        const codec::NativeEnvelope& envelope, StopToken cancellation)
    {
        const auto* payload = envelope.payload.AsLineViewRequest();
        if (payload == nullptr) {
            throw std::invalid_argument("line-view payload is missing");
        }
        const auto request = codec::fromWire(*payload);
        if (request.outputWidth < 1 || request.outputWidth > 16384) {
            throw RemoteError(ErrorCode::ResourceLimitExceeded,
                "line viewport width is outside the server limit");
        }
        constexpr std::uint64_t bytesPerPoint = sizeof(double)
            + sizeof(float) + sizeof(std::uint8_t) + sizeof(std::int16_t);
        if (!fitsResponse(static_cast<std::uint64_t>(request.outputWidth)
                * 2U * bytesPerPoint)) {
            throw RemoteError(ErrorCode::ResourceLimitExceeded,
                "line response cannot fit in one negotiated frame");
        }
        const auto dataset = requireDataset(request.query.dataset);
        const auto result = std::get<LineQueryResult>(
            dataset->requestView(ViewDataRequest{request}, cancellation));
        if (result.line.values.size()
            > static_cast<std::size_t>(request.outputWidth) * 2) {
            throw std::logic_error(
                "line planner exceeded its viewport response bound");
        }
        send(envelope.request_id,
            codec::toWire(result, dataset->cacheMetrics()));
    }

    void datasetPage(
        const codec::NativeEnvelope& envelope, StopToken cancellation)
    {
        const auto* payload = envelope.payload.AsDatasetPageRequest();
        if (payload == nullptr) {
            throw std::invalid_argument("dataset-page payload is missing");
        }
        const auto request = codec::fromWire(*payload);
        if (request.maximumExtent < 1
            || request.maximumExtent > datasetPageMaxExtent) {
            throw RemoteError(ErrorCode::ResourceLimitExceeded,
                "dataset page extent is outside the server limit");
        }
        const auto dataset = requireDataset(request.dataset);
        const auto page
            = dataset->requestDatasetPage(request, cancellation);
        const auto vectorBytes
            = static_cast<std::uint64_t>(page.values.size()) * sizeof(float)
            + static_cast<std::uint64_t>(page.covered.size())
                * sizeof(std::uint8_t);
        if (!fitsResponse(vectorBytes)) {
            throw RemoteError(ErrorCode::ResourceLimitExceeded,
                "dataset page cannot fit in one negotiated frame");
        }
        send(envelope.request_id,
            codec::toWire(page, dataset->cacheMetrics()));
    }

    void range(
        const codec::NativeEnvelope& envelope, StopToken cancellation)
    {
        const auto* payload = envelope.payload.AsRangeRequest();
        if (payload == nullptr) {
            throw std::invalid_argument("range payload is missing");
        }
        const auto [id, request] = codec::fromWire(*payload);
        const auto dataset = requireDataset(id);
        const auto result = dataset->requestRange(request, cancellation);
        send(envelope.request_id,
            codec::toWire(result, dataset->cacheMetrics()));
    }

    void particleSample(
        const codec::NativeEnvelope& envelope, StopToken cancellation)
    {
        const auto* payload = envelope.payload.AsParticleSampleRequest();
        if (payload == nullptr) {
            throw std::invalid_argument(
                "particle-sample payload is missing");
        }
        const auto request = codec::fromWire(*payload);
        const auto dataset = requireDataset(request.dataset);
        constexpr std::uint64_t bytesPerPoint
            = sizeof(std::uint64_t) + 3 * sizeof(double);
        constexpr std::uint64_t particleResponseOverheadBytes = 512;
        const auto frameBytes
            = static_cast<std::uint64_t>(m_maximumFrameBytes.load());
        const auto maximumPoints = static_cast<std::size_t>(
            frameBytes > particleResponseOverheadBytes
                ? (frameBytes - particleResponseOverheadBytes) / bytesPerPoint
                : 0);
        const auto& species = dataset->particleSpecies();
        const auto metadata = std::find_if(species.begin(), species.end(),
            [&](const auto& entry) { return entry.name == request.species; });
        if (metadata == species.end()) {
            throw std::invalid_argument("particle species is unavailable");
        }
        const auto requestedPoints = std::ceil(
            static_cast<long double>(metadata->particleCount)
            * static_cast<long double>(request.fraction));
        if (requestedPoints > static_cast<long double>(maximumPoints)) {
            throw RemoteError(ErrorCode::ResourceLimitExceeded,
                "particle sample cannot fit in one negotiated frame");
        }
        const auto sample = dataset->requestParticleSample(request.species,
            request.fraction, request.seed, maximumPoints, cancellation);
        if (!fitsResponse(static_cast<std::uint64_t>(sample.points.size())
                * bytesPerPoint)) {
            throw RemoteError(ErrorCode::ResourceLimitExceeded,
                "particle sample cannot fit in one negotiated frame");
        }
        send(envelope.request_id,
            codec::toWire(sample, dataset->cacheMetrics()));
    }

    void clearCache(const codec::NativeEnvelope& envelope)
    {
        const auto* request = envelope.payload.AsClearCacheRequest();
        if (request == nullptr) {
            throw std::invalid_argument("clear-cache payload is missing");
        }
        const auto dataset = requireDataset(DatasetId{request->dataset_id});
        dataset->clearUnpinnedCache();
        sendCache(envelope.request_id, *dataset);
    }

    void setCacheBudget(const codec::NativeEnvelope& envelope)
    {
        const auto* request = envelope.payload.AsSetCacheBudgetRequest();
        if (request == nullptr) {
            throw std::invalid_argument("cache-budget payload is missing");
        }
        const auto dataset = requireDataset(DatasetId{request->dataset_id});
        static_cast<void>(dataset->setCacheBudget(request->budget_bytes));
        sendCache(envelope.request_id, *dataset);
    }

    void sendCache(
        std::uint64_t requestId, const LocalDatasetSession& dataset)
    {
        codec::fb::CacheResponseT response;
        response.dataset_id = dataset.id().value;
        response.cache = codec::toWire(dataset.cacheMetrics());
        send(requestId, std::move(response));
    }

    void validateSliceBound(const SliceRequest& request) const
    {
        if (request.outputSize[0] < 1 || request.outputSize[1] < 1
            || request.outputSize[0] > maxViewOutputDimension
            || request.outputSize[1] > maxViewOutputDimension) {
            throw RemoteError(ErrorCode::ResourceLimitExceeded,
                "slice viewport dimensions are outside the server limit");
        }
        const auto cells = static_cast<std::uint64_t>(request.outputSize[0])
            * static_cast<std::uint64_t>(request.outputSize[1]);
        if (!fitsResponse(cells * sliceResponseBytesPerCell)) {
            throw RemoteError(ErrorCode::ResourceLimitExceeded,
                "slice viewport cannot fit in one negotiated frame");
        }
    }

    [[nodiscard]] bool fitsResponse(std::uint64_t vectorBytes) const noexcept
    {
        // Reserve room for the envelope, tables, vector offsets, metrics, and
        // FlatBuffers alignment. send() performs the final exact encoded-size
        // check before touching the socket.
        constexpr std::uint64_t responseOverheadReserveBytes = 512;
        const auto frameBytes
            = static_cast<std::uint64_t>(m_maximumFrameBytes.load());
        return frameBytes >= responseOverheadReserveBytes
            && vectorBytes <= frameBytes - responseOverheadReserveBytes;
    }

    std::shared_ptr<LocalDatasetSession> requireDataset(DatasetId id)
    {
        std::scoped_lock lock(m_stateMutex);
        const auto found = m_datasets.find(id.value);
        if (found == m_datasets.end()) {
            throw RemoteError(
                ErrorCode::UnknownDataset, "dataset handle is unknown");
        }
        return found->second;
    }

    DatasetId requestDataset(const codec::NativeEnvelope& envelope) const
    {
        switch (codec::inspect(envelope).payload) {
        case PayloadKind::CloseDatasetRequest: {
            const auto* value
                = envelope.payload.AsCloseDatasetRequest();
            return DatasetId{value ? value->dataset_id : 0};
        }
        case PayloadKind::SliceViewRequest: {
            const auto* value = envelope.payload.AsSliceViewRequest();
            return DatasetId{value ? value->dataset_id : 0};
        }
        case PayloadKind::LineViewRequest: {
            const auto* value = envelope.payload.AsLineViewRequest();
            return DatasetId{value ? value->dataset_id : 0};
        }
        case PayloadKind::DatasetPageRequest: {
            const auto* value
                = envelope.payload.AsDatasetPageRequest();
            return DatasetId{value ? value->dataset_id : 0};
        }
        case PayloadKind::ParticleSampleRequest: {
            const auto* value
                = envelope.payload.AsParticleSampleRequest();
            return DatasetId{value ? value->dataset_id : 0};
        }
        case PayloadKind::RangeRequest: {
            const auto* value = envelope.payload.AsRangeRequest();
            return DatasetId{value ? value->dataset_id : 0};
        }
        case PayloadKind::ClearCacheRequest: {
            const auto* value = envelope.payload.AsClearCacheRequest();
            return DatasetId{value ? value->dataset_id : 0};
        }
        case PayloadKind::SetCacheBudgetRequest: {
            const auto* value
                = envelope.payload.AsSetCacheBudgetRequest();
            return DatasetId{value ? value->dataset_id : 0};
        }
        default:
            return {};
        }
    }

    template <typename Payload>
    void send(std::uint64_t requestId, Payload payload) noexcept
    {
        try {
            const auto bytes
                = codec::encode(requestId, std::move(payload), m_selectedMinor);
            if (bytes.size() > m_maximumFrameBytes.load()) {
                sendError(requestId, {ErrorCode::ResourceLimitExceeded,
                    "response cannot fit in one negotiated frame"});
                return;
            }
            std::scoped_lock lock(m_writeMutex);
            if (!m_stopping.load()) {
                writeFrame(m_socket, bytes, m_maximumFrameBytes.load(),
                    std::chrono::steady_clock::now()
                        + m_options.responseWriteTimeout);
            }
        } catch (...) {
            stop();
        }
    }

    void sendError(std::uint64_t requestId, ErrorData error) noexcept
    {
        try {
            const auto bytes = codec::encode(
                requestId, codec::toWire(error), m_selectedMinor);
            if (bytes.size() > m_maximumFrameBytes.load()) {
                stop();
                return;
            }
            std::scoped_lock lock(m_writeMutex);
            if (!m_stopping.load()) {
                writeFrame(m_socket, bytes, m_maximumFrameBytes.load(),
                    std::chrono::steady_clock::now()
                        + m_options.responseWriteTimeout);
            }
        } catch (...) {
            stop();
        }
    }

    Socket m_socket;
    ThreadPool& m_workers;
    ServerOptions m_options;
    std::chrono::steady_clock::time_point m_handshakeDeadline;
    std::atomic<std::uint32_t> m_maximumFrameBytes;
    std::atomic_bool m_stopping{false};
    std::uint16_t m_selectedMinor = 0;
    bool m_handshakeComplete = false;
    std::atomic<std::uint64_t> m_nextDatasetId{1};
    std::mutex m_writeMutex;
    mutable std::mutex m_stateMutex;
    std::unordered_map<std::uint64_t,
        std::shared_ptr<LocalDatasetSession>> m_datasets;
    std::size_t m_datasetReservations = 0;
    std::unordered_map<std::uint64_t, ActiveRequest> m_active;
};

} // namespace

class Server::Impl {
public:
    explicit Impl(ServerOptions options)
        : m_options(std::move(options))
        , m_listener(listenOnLoopback(m_options.port))
        , m_workers(resolveWorkerCount(m_options.workerCount))
    {
        if (m_options.maximumConnections == 0
            || m_options.maximumDatasets == 0
            || m_options.maximumOutstandingRequests == 0
            || m_options.maximumFrameBytes == 0
            || m_options.maximumHandshakeFrameBytes == 0
            || m_options.handshakeTimeout
                <= std::chrono::milliseconds::zero()
            || m_options.responseWriteTimeout
                <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument(
                "server resource limits must be greater than zero");
        }
        m_options.workerCount = resolveWorkerCount(m_options.workerCount);
    }

    ~Impl()
    {
        requestStop();
    }

    std::uint16_t port() const noexcept
    {
        return m_listener.port;
    }

    std::string lastError() const
    {
        std::scoped_lock lock(m_errorMutex);
        return m_lastError;
    }

    void run()
    {
        auto retryDelay = std::chrono::milliseconds{10};
        while (!m_stopping.load()) {
            try {
                auto socket = acceptConnection(m_listener.socket);
                auto session = std::make_shared<Session>(
                    std::move(socket), m_workers, m_options);
                std::scoped_lock lock(m_sessionsMutex);
                std::erase_if(m_sessions,
                    [](const auto& worker) {
                        return worker.session->stopped();
                    });
                if (m_stopping.load()) {
                    session->stop();
                    break;
                }
                if (m_sessions.size() >= m_options.maximumConnections) {
                    session->stop();
                    continue;
                }
                m_sessions.push_back(SessionWorker{
                    session, JoiningThread(
                        [session] { session->run(); })});
                retryDelay = std::chrono::milliseconds{10};
            } catch (const std::exception& error) {
                if (m_stopping.load()) {
                    break;
                }
                {
                    std::scoped_lock lock(m_errorMutex);
                    m_lastError = error.what();
                }
                std::this_thread::sleep_for(retryDelay);
                retryDelay = std::min(
                    retryDelay * 2, std::chrono::milliseconds{250});
            } catch (...) {
                if (m_stopping.load()) {
                    break;
                }
                {
                    std::scoped_lock lock(m_errorMutex);
                    m_lastError = "unknown server accept-loop failure";
                }
                std::this_thread::sleep_for(retryDelay);
                retryDelay = std::min(
                    retryDelay * 2, std::chrono::milliseconds{250});
            }
        }
    }

    void requestStop() noexcept
    {
        if (m_stopping.exchange(true)) {
            return;
        }
        m_listener.socket.shutdown();
        m_listener.socket.close();
        std::vector<std::shared_ptr<Session>> sessions;
        {
            std::scoped_lock lock(m_sessionsMutex);
            sessions.reserve(m_sessions.size());
            for (const auto& worker : m_sessions) {
                sessions.push_back(worker.session);
            }
        }
        for (const auto& session : sessions) {
            session->stop();
        }
    }

private:
    static unsigned int resolveWorkerCount(unsigned int requested)
    {
        return requested == 0
            ? std::max(1U, std::thread::hardware_concurrency())
            : requested;
    }

    struct SessionWorker {
        std::shared_ptr<Session> session;
        JoiningThread thread;
    };

    ServerOptions m_options;
    Listener m_listener;
    ThreadPool m_workers;
    std::atomic_bool m_stopping{false};
    std::mutex m_sessionsMutex;
    std::vector<SessionWorker> m_sessions;
    mutable std::mutex m_errorMutex;
    std::string m_lastError;
};

Server::Server(ServerOptions options)
    : m_impl(std::make_unique<Impl>(std::move(options)))
{
}

Server::~Server() = default;

std::uint16_t Server::port() const noexcept
{
    return m_impl->port();
}

std::string Server::lastError() const
{
    return m_impl->lastError();
}

void Server::run()
{
    m_impl->run();
}

void Server::requestStop() noexcept
{
    m_impl->requestStop();
}

} // namespace amrvis::remote
