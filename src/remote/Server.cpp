#include <amrexplorer/remote/Server.hpp>

#include "Codec.hpp"
#ifdef AMREXPLORER_SERVER_TEST_HOOKS
#include "ServerTestHooks.hpp"
#endif

#include <amrexplorer/data/LocalDatasetSession.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
        try {
            for (unsigned int index = 0; index < count; ++index) {
                m_threads.emplace_back([this] { worker(); });
            }
        } catch (...) {
            // A later std::thread construction can fail after earlier workers
            // have started waiting. Wake those workers before constructor
            // unwinding destroys and joins their JoiningThread wrappers.
            {
                std::scoped_lock lock(m_mutex);
                m_stopping = true;
            }
            m_ready.notify_all();
            throw;
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

// A fresh 128-bit token rendered as 32 lowercase hex characters. Drawn from
// std::random_device, which maps to the operating-system CSPRNG on the
// platforms this server targets (/dev/urandom on Linux/macOS, CryptGenRandom
// on Windows).
std::string generateSessionToken()
{
    std::random_device device;
    static constexpr char digits[] = "0123456789abcdef";
    constexpr int byteCount = 16;
    std::string token;
    token.reserve(static_cast<std::size_t>(byteCount) * 2);
    for (int index = 0; index < byteCount; ++index) {
        const auto value = static_cast<unsigned>(device()) & 0xFFu;
        token.push_back(digits[(value >> 4) & 0xFu]);
        token.push_back(digits[value & 0xFu]);
    }
    return token;
}

// Length-independent-of-content comparison, so a rejected handshake does not
// leak how many leading bytes matched. The token length itself is not secret.
bool constantTimeEquals(std::string_view lhs, std::string_view rhs)
{
    std::size_t difference = lhs.size() ^ rhs.size();
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        const auto other = index < rhs.size()
            ? static_cast<unsigned char>(rhs[index])
            : 0U;
        difference |= static_cast<unsigned char>(lhs[index]) ^ other;
    }
    return difference == 0;
}

// Clients send dataset paths as the user typed them, and "~/..." is a shell
// habit no filesystem call honors. The server is the only side that knows its
// home directory, so it resolves here, once, for every way a path arrives:
// a leading tilde expands, and a relative path is anchored at home as well --
// documented behavior rather than an accident of the process's working
// directory (sshd happens to start the login shell at home; a --port server
// is launched from anywhere). "~user/..." passes through and fails with the
// ordinary not-found error, and with no HOME the path is left untouched.
std::string resolveDatasetPath(const std::string& path)
{
#ifdef _WIN32
    // The deployment server is Linux; a Windows server serves tests and
    // tools, whose paths are absolute, and MSVC deprecates getenv outright.
    return path;
#else
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return path;
    }
    if (path == "~") {
        return home;
    }
    if (path.starts_with("~/")) {
        return std::string(home) + path.substr(1);
    }
    if (path.starts_with('~')) {
        return path;
    }
    if (!std::filesystem::path(path).is_absolute()) {
        return (std::filesystem::path(home) / path).string();
    }
    return path;
#endif
}

// An AMReX plotfile is a directory holding a Header file and one Level_<n>
// subdirectory per level; levels run from 0, so Level_0 is always present.
// This is the Qt-side isAmrexPlotfile rule (Header plus a Level_* directory)
// answered with two stats instead of a directory scan per entry, which is
// what keeps a listing of thousands of entries cheap on a networked
// filesystem.
bool isPlotfileDirectory(const std::filesystem::path& directory)
{
    std::error_code error;
    return std::filesystem::is_regular_file(directory / "Header", error)
        && std::filesystem::is_directory(directory / "Level_0", error);
}

// The directory listing a browsing client sees: subdirectories only (files
// are not navigable and plotfiles are directories), sorted by name, the
// first maximumDirectoryEntries of them with the truncated flag set when
// more exist. Path resolution is the dataset-open one, so what a user
// browses to is what a typed path opens. The scan keeps only the names it
// will return; the per-entry plotfile stats are spent on those. Entries that
// cannot be stat'ed are skipped rather than failing the whole listing.
RemoteDirectoryListing listServerDirectory(
    const std::string& requestedPath, StopToken cancellation)
{
    std::error_code error;
    auto path = std::filesystem::path(
        resolveDatasetPath(requestedPath.empty() ? "~" : requestedPath))
                    .lexically_normal();
    if (!path.has_filename() && path.has_parent_path()
        && path.parent_path() != path) {
        // "dir/" normalizes with a trailing separator; drop it so
        // parent_path() is the parent rather than the directory itself.
        path = path.parent_path();
    }
    if (!std::filesystem::is_directory(path, error) || error) {
        throw std::invalid_argument(
            "not a readable directory: " + path.string());
    }
    RemoteDirectoryListing listing;
    listing.path = path.string();
    listing.parentPath = path.has_parent_path() ? path.parent_path().string()
                                                : listing.path;
    // The smallest maximumDirectoryEntries names, kept as a max-heap while
    // scanning, so memory and work are bounded by the cap rather than by the
    // directory, and the scan stays cancellable at every entry.
    std::vector<std::string> names;
    for (std::filesystem::directory_iterator entries(path,
             std::filesystem::directory_options::skip_permission_denied,
             error),
         end;
         !error && entries != end; entries.increment(error)) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        std::error_code entryError;
        if (!entries->is_directory(entryError) || entryError) {
            continue;
        }
        auto name = entries->path().filename().string();
        if (names.size() == maximumDirectoryEntries) {
            listing.truncated = true;
            if (!(name < names.front())) {
                continue;
            }
            std::pop_heap(names.begin(), names.end());
            names.pop_back();
        }
        names.push_back(std::move(name));
        std::push_heap(names.begin(), names.end());
    }
    if (error) {
        throw std::runtime_error(
            "could not read " + listing.path + ": " + error.message());
    }
    std::sort_heap(names.begin(), names.end());
    listing.entries.reserve(names.size());
    for (auto& name : names) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        auto subdirectory = path / name;
        const bool plotfile = isPlotfileDirectory(subdirectory);
        listing.entries.push_back(
            {std::move(name), subdirectory.string(), plotfile});
    }
    return listing;
}

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
    Session(std::unique_ptr<Channel> channel, ThreadPool& workers,
        std::atomic<unsigned int>& rendersInFlight, const ServerOptions& options)
        : m_channel(std::move(channel))
        , m_workers(workers)
        , m_rendersInFlight(rendersInFlight)
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
                    ? readFrame(*m_channel, m_maximumFrameBytes.load(),
                          std::chrono::steady_clock::time_point::max(),
                          m_lifecycleStop.get_token())
                    : readFrame(*m_channel,
                          std::min(m_options.maximumFrameBytes,
                              m_options.maximumHandshakeFrameBytes),
                          m_handshakeDeadline,
                          m_lifecycleStop.get_token());
                if (!frame) {
                    break;
                }
                auto envelope = codec::decode(*frame);
                dispatch(std::move(envelope));
            }
        } catch (...) {
        }
        stop();
        // The reader has exited and lifecycle cancellation makes any
        // in-flight writer leave its bounded readiness loop. Serialize with
        // that writer before releasing the socket handle so closesocket never
        // overlaps another Winsock operation.
        std::scoped_lock lock(m_writeMutex);
        m_channel->shutdown();
        m_channel->close();
    }

    void stop() noexcept
    {
        if (m_stopping.exchange(true)) {
            return;
        }
        m_lifecycleStop.request_stop();
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
        if (info.protocolMinorVersion != m_selectedMinorVersion) {
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
            || request->minimum_minor_version > protocolMinorVersion
            || request->maximum_minor_version
                < request->minimum_minor_version) {
            sendError(envelope.request_id, {ErrorCode::UnsupportedProtocol,
                "client protocol range is unsupported"});
            stop();
            return;
        }
        if (!constantTimeEquals(
                request->session_token, m_options.sessionToken)) {
            sendError(envelope.request_id, {ErrorCode::Unauthorized,
                "invalid or missing session token"});
            stop();
            return;
        }
        m_selectedMinorVersion = std::min<std::uint16_t>(
            protocolMinorVersion, request->maximum_minor_version);
        m_maximumFrameBytes = std::min(
            m_options.maximumFrameBytes, request->maximum_frame_bytes);
        HelloResponseData response;
        response.serverName = "AMReXplorer server";
        response.softwareVersion = m_options.softwareVersion;
        response.selectedMinorVersion = m_selectedMinorVersion;
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
            case PayloadKind::ListDirectoryRequest:
                listDirectory(*envelope, cancellation);
                break;
            case PayloadKind::RenderedFrameRequest:
                renderedFrame(*envelope, cancellation);
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
        // Decoded here, before the reservation below: this is what bounds the
        // definition list a client sent (codec::fromWire), and an over-cap list
        // should be refused before anything is reserved or allocated for it.
        auto open = codec::fromWire(*request);
        // Also before the reservation, so an early return need not know that
        // bookkeeping. Not theatre: encode() stamps the envelope's version but
        // filters no fields, so a 1.3-negotiated envelope can physically carry
        // this vector -- and a client that read our Hello knows better, so this
        // is answerable rather than fatal.
        if (!open.derivedFields.empty() && m_selectedMinorVersion < 4) {
            throw RemoteError(ErrorCode::UnsupportedProtocol,
                "derived fields require protocol 1.4");
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
            // The definitions are installed under DerivedFieldPolicy::Skip,
            // so one this plotfile cannot resolve is reported back rather than
            // failing the open -- a list written for another dataset must not
            // make this one unopenable.
            dataset = std::make_shared<LocalDatasetSession>(
                resolveDatasetPath(open.path), id, open.cacheBudgetBytes,
                cancellation, std::move(open.derivedFields));
            // The operator's number, on its own. The constructor has just
            // seeded the grid pool from cache_budget_bytes, which is the
            // client's *block* cache budget -- a different pool holding
            // different things, and no kind of ceiling for this one. Taking
            // the smaller of the two would mean an operator could never set
            // the grid pool above whatever the client asked for its blocks,
            // a client with a small AMREXPLORER_CACHE_SIZE_MB would starve
            // the pool until every render re-sampled from disk, and a peer
            // that omitted the field (wire default 0) would zero it.
            //
            // So a client cannot ask for a smaller grid pool. That is a real
            // gap, but it needs a wire field of its own rather than the
            // block budget standing in for one.
            static_cast<void>(dataset->setVolumeGridCacheBudget(
                m_options.volumeGridCacheBytes));
            OpenedDataset opened;
            opened.id = id;
            opened.catalog = dataset->metadata();
            opened.metadataMetrics = dataset->metadataReadMetrics();
            opened.fileVersion = dataset->fileVersion();
            opened.particleSpecies = dataset->particleSpecies();
            // The derived tail of the catalog, and what could not be
            // installed. Both come from the session just built, so the client
            // is told exactly what it is looking at.
            opened.derivedFieldCount = static_cast<std::uint32_t>(
                opened.catalog.fields.size() - dataset->storedFieldCount());
            opened.derivedFieldSkips = dataset->skippedDerivedFields();
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
                    m_selectedMinorVersion)
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
        auto request = codec::fromWire(*payload);
        validateSliceBound(request);
        request.maximumGridBoxes = maximumSliceGridBoxes(request);
        const auto dataset = requireDataset(request.dataset);
        auto result = std::get<SliceQueryResult>(
            dataset->requestView(ViewDataRequest{request}, cancellation));
        const auto responseSize = [&] {
            return codec::encode(envelope.request_id,
                codec::toWire(result, dataset->cacheMetrics()),
                m_selectedMinorVersion).size();
        };
        // The planner uses a conservative per-box budget. Keep an exact final
        // guard so FlatBuffers layout changes can only shed optional overlays,
        // never reject an otherwise valid raster response.
        auto encodedBytes = responseSize();
        while (encodedBytes > m_maximumFrameBytes.load()
            && !result.gridBoxes.empty()) {
            result.gridBoxesTruncated = true;
            result.gridBoxes.resize(result.gridBoxes.size() / 2U);
            encodedBytes = responseSize();
        }
        if (encodedBytes > m_maximumFrameBytes.load()) {
            throw RemoteError(ErrorCode::ResourceLimitExceeded,
                "slice raster cannot fit in one negotiated frame");
        }
        send(envelope.request_id,
            codec::toWire(result, dataset->cacheMetrics()));
    }

    void renderedFrame(
        const codec::NativeEnvelope& envelope, StopToken cancellation)
    {
        const auto* payload = envelope.payload.AsRenderedFrameRequest();
        if (payload == nullptr) {
            throw std::invalid_argument("rendered-frame payload is missing");
        }
        if (m_selectedMinorVersion < 2) {
            throw RemoteError(ErrorCode::UnsupportedProtocol,
                "volume rendering requires protocol 1.2");
        }
        auto request = codec::fromWire(*payload);
        // A client that negotiated 1.2 has no sampling field to send, so this
        // can only be a peer claiming one version and speaking another. Say so
        // rather than rendering by a rule it did not ask for -- a refusal the
        // client can read, and one that leaves the connection up.
        if (request.sampling == SamplingPolicy::Linear
            && m_selectedMinorVersion < 3) {
            throw RemoteError(ErrorCode::UnsupportedProtocol,
                "smooth volume sampling requires protocol 1.3");
        }
        validateVolumeBound(request);
        // The server's own voxel cap applies on top of the client's budget.
        request.maximumVoxels = std::min<std::uint64_t>(
            request.maximumVoxels, m_options.maximumVolumeVoxels);
        const auto dataset = requireDataset(request.dataset);
        struct RenderCount {
            std::atomic<unsigned int>& counter;
            explicit RenderCount(std::atomic<unsigned int>& value) : counter(value)
            {
                counter.fetch_add(1, std::memory_order_relaxed);
            }
            ~RenderCount() { counter.fetch_sub(1, std::memory_order_relaxed); }
            RenderCount(const RenderCount&) = delete;
            RenderCount& operator=(const RenderCount&) = delete;
        };
        // Counted around the render and nothing else. Holding it through the
        // encode and the send would keep a finished render "running" for as
        // long as its client takes to read -- and the write budget allows a
        // trickle-reader over seventeen minutes for a 67 MiB response -- so
        // renders starting on other connections would divide the machine by a
        // number that had stopped meaning anything.
        auto frame = [&] {
            const RenderCount counted(m_rendersInFlight);
            return dataset->renderVolume(
                request, cancellation, volumeRenderThreads());
        }();
        // One encode, and the pixels moved into it. send() makes the exact
        // size check itself before touching the socket and answers an
        // oversized response with the same ResourceLimitExceeded, so encoding
        // a buffer here just to measure it, discarding it and building it
        // again was pure waste -- unlike sliceView there is nothing to shed on
        // a second pass. The frame is not read after this, so its 67 MiB of
        // pixels move rather than copy; the builder and the encoded buffer
        // still hold one each, which is as few as this shape allows.
        send(envelope.request_id,
            codec::toWire(std::move(frame), dataset->cacheMetrics()));
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

    void listDirectory(
        const codec::NativeEnvelope& envelope, StopToken cancellation)
    {
        const auto* request = envelope.payload.AsListDirectoryRequest();
        if (request == nullptr
            || request->path.find(char{}) != std::string::npos) {
            throw std::invalid_argument("directory path is invalid");
        }
        if (m_selectedMinorVersion < 1) {
            throw RemoteError(ErrorCode::UnsupportedProtocol,
                "directory browsing requires protocol 1.1");
        }
        send(envelope.request_id,
            codec::toWire(listServerDirectory(request->path, cancellation)));
    }

    void setCacheBudget(const codec::NativeEnvelope& envelope)
    {
        const auto* request = envelope.payload.AsSetCacheBudgetRequest();
        if (request == nullptr) {
            throw std::invalid_argument("cache-budget payload is missing");
        }
        const auto dataset = requireDataset(DatasetId{request->dataset_id});
        // The block pool only. setCacheBudget moves both, which is what a
        // local user setting one number wants -- but this number comes from
        // the peer, and the grid pool answers to --volume-cache-mib alone.
        static_cast<void>(dataset->setBlockCacheBudget(request->budget_bytes));
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

    void validateVolumeBound(const VolumeRenderRequest& request) const
    {
        // Structural validity first (a hostile peer can vary every field),
        // then the frame's pixels against the negotiated frame; the session
        // validators check the request against the dataset.
        if (const auto errors = validateVolumeRenderRequest(request, 3);
            !errors.empty()) {
            throw std::invalid_argument(errors.front());
        }
        const auto pixels = static_cast<std::uint64_t>(request.outputSize[0])
            * static_cast<std::uint64_t>(request.outputSize[1]);
        // Reserving the volume response's own overhead, not the slice's: the
        // point of checking before rendering is that a frame which cannot fit
        // is refused without doing the work, and reserving 512 bytes for
        // tables that cost 4096 lets an oversized one through to be rendered
        // in full and then rejected by send().
        if (!fitsResponse(pixels * sizeof(std::uint32_t),
                volumeResponseOverheadBytes)) {
            throw RemoteError(ErrorCode::ResourceLimitExceeded,
                "rendered frame cannot fit in one negotiated frame");
        }
    }

    [[nodiscard]] std::size_t maximumSliceGridBoxes(
        const SliceRequest& request) const noexcept
    {
        if (!request.includeGridBoxes) {
            return 0;
        }
        const auto cells = static_cast<std::uint64_t>(request.outputSize[0])
            * static_cast<std::uint64_t>(request.outputSize[1]);
        // A grid box carries its vector offset, table/vtable, level, RealBox
        // table, six doubles, and alignment. This conservative charge keeps
        // collection bounded before serialization; the exact guard above is
        // authoritative.
        constexpr std::uint64_t bytesPerGridBox = 128;
        const auto frameBytes
            = static_cast<std::uint64_t>(m_maximumFrameBytes.load());
        if (frameBytes <= responseOverheadReserveBytes) {
            return 0;
        }
        const auto available = frameBytes - responseOverheadReserveBytes;
        const auto rasterBytes = cells * sliceResponseBytesPerCell;
        if (available <= rasterBytes) {
            return 0;
        }
        return static_cast<std::size_t>(
            (available - rasterBytes) / bytesPerGridBox);
    }

    // How many threads one volume render may use: the operator's CPU budget
    // shared between the renders actually running, counted server-wide.
    //
    // The budget is workerCount, not hardware concurrency. --threads is what
    // an operator sets to bound this server's CPU, and it is the only figure
    // that knows about a container quota -- hardware_concurrency() reports
    // the host's cores, so `--threads 1` inside a two-CPU cgroup on a 64-core
    // machine would otherwise put 64 threads on one ray cast. workerCount
    // defaults to hardware concurrency, so an unconfigured server still gives
    // a lone render the machine.
    //
    // Dividing by the renders in flight rather than by workerCount is what
    // keeps that lone render from being pinned to one thread: --stdio serves
    // one client with one render at a time, which is the case that matters
    // most and the one a static division gets worst.
    //
    // A render keeps the count it started with, so a burst that arrives
    // together can transiently exceed the budget -- eight simultaneous first
    // renders read 1, 2, ... 8 and sum to about 2.7 times it. Holding to the
    // budget exactly needs render threads drawn from a shared pool of tokens
    // rather than a number computed per render; see the follow-up note.
    [[nodiscard]] unsigned int volumeRenderThreads() const noexcept
    {
        const auto budget = std::max(1U, m_options.workerCount);
        const auto running = std::max(1U,
            m_rendersInFlight.load(std::memory_order_relaxed));
        return std::max(1U, budget / running);
    }

    // overheadBytes defaults to the slice reserve, which is what every
    // response but a volume frame is measured against.
    [[nodiscard]] bool fitsResponse(std::uint64_t vectorBytes,
        std::uint64_t overheadBytes = responseOverheadReserveBytes) const noexcept
    {
        // Reserve room for the envelope, tables, vector offsets, metrics, and
        // FlatBuffers alignment. send() performs the final exact encoded-size
        // check before touching the socket.
        const auto frameBytes
            = static_cast<std::uint64_t>(m_maximumFrameBytes.load());
        return frameBytes >= overheadBytes
            && vectorBytes <= frameBytes - overheadBytes;
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
        case PayloadKind::RenderedFrameRequest: {
            const auto* value
                = envelope.payload.AsRenderedFrameRequest();
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
            const auto bytes = codec::encode(
                requestId, std::move(payload), m_selectedMinorVersion);
            if (bytes.size() > m_maximumFrameBytes.load()) {
                sendError(requestId, {ErrorCode::ResourceLimitExceeded,
                    "response cannot fit in one negotiated frame"});
                return;
            }
            std::scoped_lock lock(m_writeMutex);
            if (!m_stopping.load()) {
#ifdef AMREXPLORER_SERVER_TEST_HOOKS
                testing::notifyBeforeResponseWrite(requestId);
                // Marks this thread as writing a server response, which is what
                // scopes write pacing to the server: a test's own client
                // sockets live in the same process.
                const testing::ResponseWriteScope pacingScope;
#endif
                writeFrameWithBudget(*m_channel, bytes,
                    m_maximumFrameBytes.load(), writeBudget(), {},
                    m_lifecycleStop.get_token());
            }
        } catch (...) {
            stop();
        }
    }

    void sendError(std::uint64_t requestId, ErrorData error) noexcept
    {
        try {
            const auto bytes = codec::encode(
                requestId, codec::toWire(error), m_selectedMinorVersion);
            if (bytes.size() > m_maximumFrameBytes.load()) {
                stop();
                return;
            }
            std::scoped_lock lock(m_writeMutex);
            if (!m_stopping.load()) {
                writeFrameWithBudget(*m_channel, bytes,
                    m_maximumFrameBytes.load(), writeBudget(), {},
                    m_lifecycleStop.get_token());
            }
        } catch (...) {
            stop();
        }
    }

    // Both write limits, per response: no-progress and whole-write. A write
    // that exceeds either throws, and both callers above answer a throw by
    // retiring the session, which is what releases the worker and the write
    // mutex a trickle-reader would otherwise hold forever.
    [[nodiscard]] FrameWriteBudget writeBudget() const noexcept
    {
        return {m_options.responseWriteStallTimeout,
            m_options.responseWriteMinimumBytesPerSecond};
    }

    std::unique_ptr<Channel> m_channel;
    static constexpr std::uint64_t responseOverheadReserveBytes
        = sliceResponseOverheadBytes;
    ThreadPool& m_workers;
    std::atomic<unsigned int>& m_rendersInFlight;
    ServerOptions m_options;
    std::chrono::steady_clock::time_point m_handshakeDeadline;
    std::atomic<std::uint32_t> m_maximumFrameBytes;
    std::atomic_bool m_stopping{false};
    StopSource m_lifecycleStop;
    std::uint16_t m_selectedMinorVersion = 0;
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
    // With a channel, the server serves that one pre-connected peer and never
    // listens; without one it listens on loopback. Options are validated
    // before either a listener or the worker pool exists.
    Impl(ServerOptions options, std::unique_ptr<Channel> channel)
        : m_options(validated(std::move(options)))
        , m_channel(std::move(channel))
        , m_listener(m_channel ? std::optional<Listener>{}
                               : std::optional<Listener>{
                                     listenOnLoopback(m_options.port)})
        , m_workers(m_options.workerCount)
    {
    }

    ~Impl()
    {
        requestStop();
    }

    std::uint16_t port() const noexcept
    {
        return m_listener ? m_listener->port : 0;
    }

    std::string lastError() const
    {
        std::scoped_lock lock(m_errorMutex);
        return m_lastError;
    }

    const std::string& token() const noexcept
    {
        return m_options.sessionToken;
    }

    void run()
    {
        if (!m_listener) {
            runSingleSession();
            return;
        }
        auto retryDelay = std::chrono::milliseconds{10};
        while (!m_stopping.load()) {
            try {
                auto socket = std::make_unique<Socket>(acceptConnection(
                    m_listener->socket, m_acceptStop.get_token()));
                auto session = std::make_shared<Session>(
                    std::move(socket), m_workers, m_rendersInFlight, m_options);
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
        m_listener->socket.close();
    }

    void requestStop() noexcept
    {
        if (m_stopping.exchange(true)) {
            return;
        }
        m_acceptStop.request_stop();
        std::vector<std::shared_ptr<Session>> sessions;
        {
            std::scoped_lock lock(m_sessionsMutex);
            sessions.reserve(m_sessions.size() + 1);
            for (const auto& worker : m_sessions) {
                sessions.push_back(worker.session);
            }
            if (m_singleSession) {
                sessions.push_back(m_singleSession);
            }
        }
        for (const auto& session : sessions) {
            session->stop();
        }
    }

private:
    static ServerOptions validated(ServerOptions options)
    {
        if (options.maximumConnections == 0
            || options.maximumDatasets == 0
            || options.maximumOutstandingRequests == 0
            || options.maximumFrameBytes == 0
            || options.maximumHandshakeFrameBytes == 0
            || options.handshakeTimeout
                <= std::chrono::milliseconds::zero()
            || options.responseWriteStallTimeout
                <= std::chrono::milliseconds::zero()
            || options.responseWriteMinimumBytesPerSecond == 0
            || options.maximumVolumeVoxels == 0
            || options.volumeGridCacheBytes == 0) {
            throw std::invalid_argument(
                "server resource limits must be greater than zero");
        }
        options.maximumVolumeVoxels = std::min(
            options.maximumVolumeVoxels, maxVolumeVoxelBudget);
        // Capped here as well as in the CLI parser, so an embedder that sets
        // ServerOptions directly gets the bound too -- the cache evicts at any
        // budget, but one this large stops bounding the process by anything a
        // host can actually lend it.
        options.volumeGridCacheBytes = std::min<std::uint64_t>(
            options.volumeGridCacheBytes, maximumVolumeGridCacheBytes);
        options.workerCount = resolveWorkerCount(options.workerCount);
        if (options.sessionToken.empty()) {
            options.sessionToken = generateSessionToken();
        }
        return options;
    }

    static unsigned int resolveWorkerCount(unsigned int requested)
    {
        return requested == 0
            ? std::max(1U, std::thread::hardware_concurrency())
            : requested;
    }

    // One session over the pre-connected channel, run on the caller's thread.
    // Returns when the peer closes the stream, the session fails, or
    // requestStop() is called; the process that owns the channel then exits,
    // which is what ties an ssh-launched server's lifetime to its session.
    void runSingleSession()
    {
        std::shared_ptr<Session> session;
        {
            std::scoped_lock lock(m_sessionsMutex);
            if (m_stopping.load() || !m_channel) {
                return;
            }
            session = std::make_shared<Session>(
                std::move(m_channel), m_workers, m_rendersInFlight, m_options);
            m_singleSession = session;
        }
        session->run();
    }

    struct SessionWorker {
        std::shared_ptr<Session> session;
        JoiningThread thread;
    };

    ServerOptions m_options;
    std::unique_ptr<Channel> m_channel;
    std::optional<Listener> m_listener;
    // Before m_workers, deliberately: members die in reverse declaration
    // order, and ~ThreadPool only *signals* its workers -- they are joined
    // when its JoiningThread members die, after the body. A task still
    // running then holds a Session that holds a reference to this counter, so
    // a counter declared after the pool would already be gone when its
    // RenderCount destructor fired.
    //
    // Volume renders running right now, across every session: what each one
    // divides the machine by when it picks its thread count.
    std::atomic<unsigned int> m_rendersInFlight{0};
    ThreadPool m_workers;
    std::atomic_bool m_stopping{false};
    StopSource m_acceptStop;
    std::mutex m_sessionsMutex;
    std::vector<SessionWorker> m_sessions;
    std::shared_ptr<Session> m_singleSession;
    mutable std::mutex m_errorMutex;
    std::string m_lastError;
};

Server::Server(ServerOptions options)
    : m_impl(std::make_unique<Impl>(std::move(options), nullptr))
{
}

Server::Server(std::unique_ptr<Channel> channel, ServerOptions options)
    : m_impl(std::make_unique<Impl>(std::move(options),
          channel ? std::move(channel)
                  : throw std::invalid_argument(
                        "server channel must not be null")))
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

const std::string& Server::token() const noexcept
{
    return m_impl->token();
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
