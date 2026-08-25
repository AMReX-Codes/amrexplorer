#include "../../src/remote/Codec.hpp"
#ifdef AMREXPLORER_SERVER_TEST_HOOKS
#include "../../src/remote/ServerTestHooks.hpp"
#endif

#include <amrexplorer/data/ViewData.hpp>
#include <amrexplorer/remote/Frame.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <ranges>
#include <cerrno>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

class RunningServer {
public:
    explicit RunningServer(amrvis::remote::Server& server)
        : m_server(server)
        , m_done(m_donePromise.get_future())
        , m_thread([this] {
            try {
                m_server.run();
            } catch (...) {
                m_failure = std::current_exception();
            }
            m_donePromise.set_value();
        })
    {
    }

    ~RunningServer()
    {
        m_server.requestStop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_failure) {
            try {
                std::rethrow_exception(m_failure);
            } catch (const std::exception& error) {
                std::cerr << "server thread failed: " << error.what() << '\n';
                std::terminate();
            }
        }
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    bool stopWithin(std::chrono::milliseconds timeout)
    {
        m_server.requestStop();
        if (m_done.wait_for(timeout) != std::future_status::ready) {
            return false;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        return true;
    }

private:
    amrvis::remote::Server& m_server;
    std::promise<void> m_donePromise;
    std::future<void> m_done;
    std::thread m_thread;
    std::exception_ptr m_failure;
};

template <typename Payload>
std::unique_ptr<amrvis::remote::codec::NativeEnvelope> exchange(
    const amrvis::remote::Socket& socket, std::uint64_t requestId,
    Payload payload, std::uint32_t maximumFrameBytes,
    std::uint16_t minorVersion = amrvis::remote::protocolMinorVersion)
{
    using namespace amrvis::remote;
    writeFrame(socket,
        codec::encode(requestId, std::move(payload), minorVersion),
        maximumFrameBytes);
    const auto response = readFrame(socket, maximumFrameBytes);
    require(response.has_value(), "server closed before sending a response");
    auto envelope = codec::decode(*response);
    require(envelope->request_id == requestId,
        "server response used the wrong request ID");
    return envelope;
}

std::unique_ptr<amrvis::remote::codec::NativeEnvelope> readWithDeadline(
    amrvis::remote::Socket& socket, std::uint32_t maximumFrameBytes,
    std::chrono::milliseconds timeout)
{
    auto pending = std::async(std::launch::async,
        [&socket, maximumFrameBytes] {
            const auto response
                = amrvis::remote::readFrame(socket, maximumFrameBytes);
            return response
                ? amrvis::remote::codec::decode(*response)
                : std::unique_ptr<
                      amrvis::remote::codec::NativeEnvelope>{};
        });
    if (pending.wait_for(timeout) != std::future_status::ready) {
        socket.shutdown();
        pending.wait();
        return {};
    }
    return pending.get();
}

#ifdef AMREXPLORER_SERVER_TEST_HOOKS
// One recv, no framing: the trickle-reader test accepts a few kibibytes at a
// time rather than a whole frame. These sockets are non-blocking, so an empty
// receive buffer reports EAGAIN, which means "nothing yet" and must not be
// confused with the close this test is waiting for. Returns the byte count, 0
// on a closed connection, and -1 for "nothing available right now".
long readRaw(const amrvis::remote::Socket& socket,
    std::span<std::uint8_t> destination)
{
#ifdef _WIN32
    const auto count = ::recv(static_cast<SOCKET>(socket.descriptor()),
        reinterpret_cast<char*>(destination.data()),
        static_cast<int>(destination.size()), 0);
    if (count == SOCKET_ERROR) {
        return ::WSAGetLastError() == WSAEWOULDBLOCK ? -1 : 0;
    }
    return count;
#else
    const auto count = ::recv(static_cast<int>(socket.descriptor()),
        destination.data(), destination.size(), 0);
    if (count < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : 0;
    }
    return static_cast<long>(count);
#endif
}
#endif

amrvis::remote::HelloRequestData helloRequest(std::string sessionToken)
{
    using namespace amrvis::remote;
    return {"server integration test", "test", 0, protocolMinorVersion,
        defaultMaximumFrameBytes, std::move(sessionToken), {}};
}

void writeOversizedParticleHeader(const std::filesystem::path& root)
{
    using namespace amrvis::remote;
    constexpr std::uint64_t bytesPerPoint
        = sizeof(std::uint64_t) + 3 * sizeof(double);
    constexpr std::uint64_t particleCount
        = defaultMaximumFrameBytes / bytesPerPoint + 1;
    const auto species = root / "Oversized";
    std::filesystem::create_directories(species);
    std::ofstream header(species / "Header");
    require(static_cast<bool>(header),
        "could not create oversized particle Header");
    header << "Version_Two_Dot_Zero_double\n"
           << "2\n0\n0\n1\n"
           << particleCount << '\n'
           << particleCount + 1 << "\n0\n1\n"
           << "0 " << particleCount << " 0\n";
    require(static_cast<bool>(header),
        "could not write oversized particle Header");
}

#ifdef AMREXPLORER_SERVER_TEST_HOOKS
// Set by the before-write hook and read by the pacing hook, which run on the
// same worker thread around one response write. This is how pacing is aimed at
// a single request rather than at every response the server writes.
thread_local bool g_paceThisWrite = false;
#endif

// A species Header whose ten levels each declare two million grids. Every count
// is under the per-level cap, so only the whole-table cap rejects it. The point
// here is *where* it is read: species discovery runs at dataset open, inside the
// long-lived server, so before that cap this cost the server hundreds of
// megabytes for a few dozen bytes of text supplied by whoever can write a
// server-visible directory.
void writeCraftedGridTable(const std::filesystem::path& root)
{
    const auto species = root / "GridBomb";
    std::filesystem::create_directories(species);
    std::ofstream header(species / "Header");
    require(static_cast<bool>(header),
        "could not create crafted grid-table Header");
    header << "Version_Two_Dot_Zero_double\n"
           << "2\n0\n0\n1\n"
           << "0\n1\n9\n";
    for (int level = 0; level < 10; ++level) {
        header << "2000000\n";
    }
    require(static_cast<bool>(header),
        "could not write crafted grid-table Header");
}

// A plotfile Header whose component-name line never ends. The reader walks a
// Header as text, so without a ceiling this line is accumulated whole before
// any parse can look at it -- and in the long-lived server that allocation is
// charged to a session rather than to a process that can simply die. Two
// hundred kilobytes is enough to tell the two behaviours apart by the error
// text: bounded, the reader names the length; unbounded, it swallows the line
// and fails later on the field after it.
//
// Deliberately does not begin with "Version_", so particle species discovery
// skips this directory and the crafted Header is read only when it is opened
// as a dataset in its own right.
void writeCraftedPlotfileHeader(const std::filesystem::path& root)
{
    const auto crafted = root / "CraftedHeader";
    std::filesystem::create_directories(crafted);
    std::ofstream header(crafted / "Header");
    require(static_cast<bool>(header),
        "could not create crafted plotfile Header");
    header << "HyperCLaw-V1.1\n1\n" << std::string(200000, 'x') << '\n';
    require(static_cast<bool>(header),
        "could not write crafted plotfile Header");
}

} // namespace

int main(int argc, char* argv[])
{
    using namespace amrvis;
    using namespace amrvis::remote;

    if (argc != 2) {
        std::cerr << "usage: test_remote_server MATERIALIZED_PLOTFILE\n";
        return 2;
    }
    writeOversizedParticleHeader(argv[1]);
    writeCraftedGridTable(argv[1]);
    writeCraftedPlotfileHeader(argv[1]);

    ServerOptions options;
    options.workerCount = 2;
    options.maximumDatasets = 2;
    options.maximumOutstandingRequests = 8;
    options.maximumConnections = 4;
    Server server(options);
    RunningServer running(server);

    require(!server.token().empty(),
        "server did not generate a session token");

    // A handshake carrying the wrong token must be refused with Unauthorized,
    // and the server must close that connection rather than serve it.
    {
        auto rogue = connectTo("127.0.0.1", server.port());
        auto rejected = exchange(rogue, 1,
            codec::toWire(HelloRequestData{
                "rogue client", "test", 0, protocolMinorVersion,
                defaultMaximumFrameBytes, server.token() + "x", {}}),
            defaultMaximumFrameBytes);
        require(codec::inspect(*rejected).payload
                == PayloadKind::ErrorResponse,
            "server accepted a bad token");
        require(codec::fromWire(*rejected->payload.AsErrorResponse()).code
                == ErrorCode::Unauthorized,
            "bad token returned the wrong error code");
        require(!readFrame(rogue, defaultMaximumFrameBytes).has_value(),
            "server left the rejected connection open");
    }

    auto socket = connectTo("127.0.0.1", server.port());
    constexpr std::uint32_t reducedFrameBytes = 1U * 1024U * 1024U;
    auto authorizedHello = helloRequest(server.token());
    authorizedHello.maximumFrameBytes = reducedFrameBytes;
    auto envelope = exchange(socket, 1,
        codec::toWire(authorizedHello), reducedFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "server rejected a compatible handshake");
    const auto hello = codec::fromWire(
        *envelope->payload.AsHelloResponse());
    require(hello.selectedMinorVersion == protocolMinorVersion
            && hello.workerCount == options.workerCount,
        "server handshake reported the wrong limits");

    envelope = exchange(socket, 2,
        codec::toWire(OpenDatasetData{
            std::filesystem::path(argv[1]).string(),
            16ULL * 1024ULL * 1024ULL, {}}),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetOpened,
        "server did not open the materialized plotfile");
    const auto opened = codec::fromWire(
        *envelope->payload.AsDatasetOpened());
    require(opened.catalog.dimension == 2
            && opened.catalog.levels.size() == 2,
        "server returned an incomplete dataset catalog");
    require(opened.derivedFieldCount == 0 && opened.derivedFieldSkips.empty(),
        "server reported derived fields for a request that carried none");
    const auto storedFields = opened.catalog.fields.size();

    // Protocol 1.4: the definitions are installed on the session the server
    // opens, so the catalog comes back with the computed field appended and the
    // client is told how many of the fields are computed. A definition this
    // plotfile cannot resolve is skipped with its reason rather than failing
    // the open, and the skip names the definition it belongs to by index.
    // An expression past maximumExpressionBytes is one of those rather than a
    // refusal: compile() bounds it before parsing anything, so the remote
    // answer for such a list is the one a local dataset gives. The last
    // definition is the other side of that bound -- long enough to name a
    // symbol installation must quote back, so the reason it produces is longer
    // than the reason bound, and the reply only decodes because toWire holds
    // it to that bound.
    envelope = exchange(socket, 16,
        codec::toWire(OpenDatasetData{
            std::filesystem::path(argv[1]).string(),
            16ULL * 1024ULL * 1024ULL,
            {{"product", "density * temperature"},
                {"elsewhere", "nonesuch * 2"},
                {"overlong", std::string(maximumExpressionBytes + 1, 'x')},
                {"longreason", std::string(maximumExpressionBytes - 6, 'a')}}}),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetOpened,
        "server refused an open carrying derived fields");
    const auto derivedOpen = codec::fromWire(
        *envelope->payload.AsDatasetOpened());
    require(derivedOpen.catalog.fields.size() == storedFields + 1
            && derivedOpen.catalog.fields.back().name == "product",
        "the computed field is not the tail of the catalog");
    require(derivedOpen.derivedFieldCount == 1,
        "server did not report exactly one field as computed");
    require(derivedOpen.derivedFieldSkips.size() == 3
            && derivedOpen.derivedFieldSkips[0].definitionIndex == 1
            && derivedOpen.derivedFieldSkips[0].name == "elsewhere"
            && !derivedOpen.derivedFieldSkips[0].reason.empty(),
        "the unresolvable definition did not come back as a skip");
    // Reported, not refused -- and the reason is the short one compile() gives,
    // not the expression it is about.
    require(derivedOpen.derivedFieldSkips[1].definitionIndex == 2
            && derivedOpen.derivedFieldSkips[1].name == "overlong"
            && !derivedOpen.derivedFieldSkips[1].reason.empty()
            && derivedOpen.derivedFieldSkips[1].reason.size()
                < maximumExpressionBytes,
        "the over-long expression did not come back as a bounded skip");
    // The reason installation produced for this one is longer than the bound
    // fromWire enforces, so reaching this line at all is the point: before
    // toWire bounded it, the decode above threw and the connection closed.
    // The marker is what proves it was truncated rather than merely short.
    const auto& longest = derivedOpen.derivedFieldSkips[2];
    require(longest.definitionIndex == 3 && longest.name == "longreason"
            && longest.reason.size() == maximumExpressionBytes
            && longest.reason.ends_with("..."),
        "an over-long skip reason was not bounded on the way out");
    // A derived field has no stored statistics, so the server reports no File
    // range for it -- the same answer a local session gives.
    require(derivedOpen.fileRangeAvailable.size()
                == derivedOpen.catalog.fields.size()
            && derivedOpen.fileRangeAvailable.back() == 0,
        "server claimed a File range for a computed field");
    // Released again: this session's dataset limit is 2, and the cases below
    // need the slot.
    codec::fb::CloseDatasetRequestT closeDerived;
    closeDerived.dataset_id = derivedOpen.id.value;
    envelope = exchange(socket, 17, std::move(closeDerived),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetClosed,
        "server did not close the dataset opened with derived fields");

    DatasetPageRequest smallPage;
    smallPage.dataset = opened.id;
    smallPage.field = FieldId{0};
    smallPage.level = 0;
    smallPage.region = opened.catalog.physicalDomain;
    smallPage.normalAxis = 1;
    smallPage.maximumExtent = datasetPageMaxExtent;
    envelope = exchange(socket, 9, codec::toWire(smallPage),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload
                == PayloadKind::DatasetPageResponse,
        "server rejected a small dataset page under a reduced frame limit");
    const auto page
        = codec::fromWire(*envelope->payload.AsDatasetPageResponse());
    require(page.nx == 4 && page.ny == 4 && page.values.size() == 16,
        "server returned the wrong reduced-frame dataset page");

    DatasetPageRequest reversedPage;
    reversedPage.dataset = opened.id;
    reversedPage.field = FieldId{0};
    reversedPage.level = 0;
    reversedPage.region = opened.catalog.physicalDomain;
    std::swap(reversedPage.region.lower[0], reversedPage.region.upper[0]);
    reversedPage.normalAxis = 1;
    envelope = exchange(socket, 10, codec::toWire(reversedPage),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse
            && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                == ErrorCode::InvalidRequest,
        "server did not reject a reversed dataset-page region");

    // The server owns its own trust boundary: a client that skips the
    // client-side check must still be refused. Only the line axis of a region is
    // meaningful, so this reverses exactly that axis.
    LineViewRequest reversedLine;
    reversedLine.query.dataset = opened.id;
    reversedLine.query.field = FieldId{0};
    reversedLine.query.axis = 0;
    reversedLine.query.fixedCoordinates = {0.0, 0.5, 0.0};
    reversedLine.query.maximumLevel = opened.catalog.finestLevel;
    reversedLine.query.region = opened.catalog.physicalDomain;
    std::swap(reversedLine.query.region->lower[0],
        reversedLine.query.region->upper[0]);
    reversedLine.outputWidth = 8;
    envelope = exchange(socket, 11, codec::toWire(reversedLine),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse
            && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                == ErrorCode::InvalidRequest,
        "server did not reject a reversed line region");

    // A region degenerate off the line axis is legitimate: a line plot along x
    // fixes y and z.
    LineViewRequest flatLine = reversedLine;
    flatLine.query.region = opened.catalog.physicalDomain;
    flatLine.query.region->lower[1] = flatLine.query.region->upper[1];
    envelope = exchange(socket, 12, codec::toWire(flatLine),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload
            == PayloadKind::LineViewResponse,
        "server rejected a line region degenerate off its axis");

    envelope = exchange(socket, 8,
        codec::toWire(opened.id, "Oversized", 1.0, 0),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse
            && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                == ErrorCode::ResourceLimitExceeded,
        "oversized particle request reached the particle reader");

    // The crafted grid table was already read once, by species discovery during
    // the open above -- an open that succeeded, which is the property under
    // test: a crafted species costs the server a rejected species, not its
    // memory. The species is simply absent from the catalog, and asking for it
    // by name is an ordinary bounded error rather than anything the server has
    // to survive.
    // "Oversized" proves the absence below means something: its Header is
    // well-formed, only its particle count is beyond one frame, so discovery
    // does list it. A species missing from this list was rejected by the
    // parser, not overlooked by the test.
    require(std::ranges::any_of(opened.particleSpecies,
                [](const auto& candidate) {
                    return candidate.name == "Oversized";
                }),
        "a well-formed particle species was not discovered");
    require(std::ranges::none_of(opened.particleSpecies,
                [](const auto& candidate) {
                    return candidate.name == "GridBomb";
                }),
        "a crafted particle grid table was advertised to the client");
    envelope = exchange(socket, 13,
        codec::toWire(opened.id, "GridBomb", 1.0, 0), hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse
            && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                == ErrorCode::InvalidRequest,
        "a crafted particle species was not refused by name");

    // A crafted plotfile Header opened as a dataset of its own. The property is
    // the one the bounds exist for: the server refuses it on the Header's
    // length rather than accumulating the line, and the session it was sent on
    // keeps working afterwards. The message check is what makes this
    // discriminating -- an unbounded reader would swallow the 200 KB line and
    // fail on the next field instead.
    {
        envelope = exchange(socket, 14,
            codec::toWire(OpenDatasetData{
                (std::filesystem::path(argv[1]) / "CraftedHeader").string(),
                16ULL * 1024ULL * 1024ULL, {}}),
            hello.maximumFrameBytes);
        require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse,
            "a crafted plotfile Header was opened as a dataset");
        const auto rejection
            = codec::fromWire(*envelope->payload.AsErrorResponse());
        // What this case can and cannot show: it pins that the server refuses
        // a crafted Header and keeps serving, on a typed code rather than on
        // prose. It cannot show that the refusal was *cheap* -- the message is
        // the same whether or not the file was accumulated first -- so the
        // allocation itself is measured in header_allocation_bounds instead.
        //
        // The code first, like every other refusal case here: without it this
        // passes on any error that happens to carry the phrase, including a
        // cancellation or an authorization failure.
        require(rejection.code == ErrorCode::DatasetOpenFailure,
            "a crafted plotfile Header was not refused as an open failure");
        require(rejection.message.find("exceeds the supported length")
                != std::string::npos,
            "a crafted plotfile Header was rejected for the wrong reason");

        // ...and the server keeps serving: the dataset opened before the
        // crafted one is still usable on the same connection.
        envelope = exchange(socket, 15, codec::toWire(smallPage),
            hello.maximumFrameBytes);
        require(codec::inspect(*envelope).payload
                == PayloadKind::DatasetPageResponse,
            "the server stopped serving after refusing a crafted Header");
    }

    SliceRequest request;
    request.dataset = opened.id;
    request.field = FieldId{0};
    request.normalDirection = 1;
    request.visibleRegion = opened.catalog.physicalDomain;
    request.physicalPosition = 0.5
        * (request.visibleRegion.lower[1] + request.visibleRegion.upper[1]);
    request.maximumLevel = opened.catalog.finestLevel;
    request.outputSize = {8, 6};
    envelope = exchange(socket, 3, codec::toWire(request),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload
            == PayloadKind::SliceViewResponse,
        "server did not return a slice response");
    const auto slice = codec::fromWire(
        *envelope->payload.AsSliceViewResponse());
    require(slice.plane.width == 8 && slice.plane.height == 6
            && slice.plane.values.size() == 48,
        "server did not honor the bounded slice extent");

    request.outputSize = {maxViewOutputDimension + 1, 1};
    envelope = exchange(socket, 4, codec::toWire(request),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse,
        "server accepted an oversized slice");
    const auto error = codec::fromWire(
        *envelope->payload.AsErrorResponse());
    require(error.code == ErrorCode::ResourceLimitExceeded,
        "oversized slice returned the wrong error");

    envelope = exchange(socket, 5,
        codec::toWire(OpenDatasetData{
            std::filesystem::path(argv[1]).string(),
            16ULL * 1024ULL * 1024ULL, {}}),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetOpened,
        "server rejected the second allowed dataset");
    const auto secondOpened
        = codec::fromWire(*envelope->payload.AsDatasetOpened());

    envelope = exchange(socket, 6,
        codec::toWire(OpenDatasetData{
            std::filesystem::path(argv[1]).string(),
            16ULL * 1024ULL * 1024ULL, {}}),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse
            && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                == ErrorCode::ResourceLimitExceeded,
        "server did not enforce the configured dataset limit");

    codec::fb::CloseDatasetRequestT closeSecond;
    closeSecond.dataset_id = secondOpened.id.value;
    envelope = exchange(socket, 7, std::move(closeSecond),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetClosed,
        "server did not close the second dataset");

    // A peer that negotiated 1.3 asking for derived fields anyway. encode()
    // stamps the envelope's version but filters no fields, so the request can
    // physically carry the vector -- the server's guard is the only thing that
    // refuses it, and it must refuse answerably: a capability the peer is too
    // old for is not a peer that has stopped speaking the protocol.
    {
        auto olderSocket = connectTo("127.0.0.1", server.port());
        auto olderHello = helloRequest(server.token());
        olderHello.maximumMinorVersion = 3;
        auto olderEnvelope = exchange(olderSocket, 1,
            codec::toWire(olderHello), defaultMaximumFrameBytes, 3);
        require(codec::inspect(*olderEnvelope).payload
                == PayloadKind::HelloResponse,
            "server rejected a 1.3 handshake");
        const auto olderInfo = codec::fromWire(
            *olderEnvelope->payload.AsHelloResponse());
        require(olderInfo.selectedMinorVersion == 3,
            "server did not negotiate down to the peer maximum");

        olderEnvelope = exchange(olderSocket, 2,
            codec::toWire(OpenDatasetData{
                std::filesystem::path(argv[1]).string(),
                16ULL * 1024ULL * 1024ULL,
                {{"product", "density * temperature"}}}),
            olderInfo.maximumFrameBytes, 3);
        require(codec::inspect(*olderEnvelope).payload
                == PayloadKind::ErrorResponse,
            "a 1.3 peer derived-field open was not refused");
        const auto olderError = codec::fromWire(
            *olderEnvelope->payload.AsErrorResponse());
        require(olderError.code == ErrorCode::UnsupportedProtocol,
            "a 1.3 peer derived-field open returned the wrong error");

        // And the session survives it: an open without definitions still works.
        olderEnvelope = exchange(olderSocket, 3,
            codec::toWire(OpenDatasetData{
                std::filesystem::path(argv[1]).string(),
                16ULL * 1024ULL * 1024ULL, {}}),
            olderInfo.maximumFrameBytes, 3);
        require(codec::inspect(*olderEnvelope).payload
                == PayloadKind::DatasetOpened,
            "the refusal closed a session that should have survived it");
    }

    ServerOptions duplicateOptions;
    duplicateOptions.workerCount = 2;
    duplicateOptions.maximumDatasets = 1;
    duplicateOptions.maximumOutstandingRequests = 2;
    duplicateOptions.maximumConnections = 1;
    Server duplicateServer(duplicateOptions);
    RunningServer duplicateRunning(duplicateServer);
    auto duplicateSocket
        = connectTo("127.0.0.1", duplicateServer.port());
    envelope = exchange(duplicateSocket, 1,
        codec::toWire(helloRequest(duplicateServer.token())),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "server rejected the duplicate-ID test connection");
    const auto duplicateHello = codec::fromWire(
        *envelope->payload.AsHelloResponse());
    const OpenDatasetData duplicateOpen{
        std::filesystem::path(argv[1]).string(),
        16ULL * 1024ULL * 1024ULL, {}};
    envelope = exchange(duplicateSocket, 2, codec::toWire(duplicateOpen),
        duplicateHello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetOpened,
        "server did not open the duplicate-ID test dataset");
    const auto duplicateOpened = codec::fromWire(
        *envelope->payload.AsDatasetOpened());
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    SliceRequest duplicateSlice;
    duplicateSlice.dataset = duplicateOpened.id;
    duplicateSlice.field = FieldId{0};
    duplicateSlice.normalDirection = 1;
    duplicateSlice.visibleRegion = duplicateOpened.catalog.physicalDomain;
    duplicateSlice.physicalPosition = 0.5
        * (duplicateSlice.visibleRegion.lower[1]
            + duplicateSlice.visibleRegion.upper[1]);
    duplicateSlice.maximumLevel = duplicateOpened.catalog.finestLevel;
    // Keep the first request live while the reader dispatches the duplicate:
    // its response is larger than the socket's send buffer and cannot finish
    // until this client starts reading below.
    duplicateSlice.outputSize = {1536, 1536};
    writeFrame(duplicateSocket,
        codec::encode(3, codec::toWire(duplicateSlice)),
        duplicateHello.maximumFrameBytes);
    writeFrame(duplicateSocket,
        codec::encode(4, codec::toWire(duplicateSlice)),
        duplicateHello.maximumFrameBytes);
    writeFrame(duplicateSocket,
        codec::encode(3, codec::toWire(duplicateSlice)),
        duplicateHello.maximumFrameBytes);
    bool duplicateRejected = false;
    for (int response = 0; response < 3 && !duplicateRejected; ++response) {
        auto duplicateResponse = readWithDeadline(duplicateSocket,
            duplicateHello.maximumFrameBytes, std::chrono::seconds{10});
        require(duplicateResponse != nullptr,
            "duplicate request ID caused an unbounded disconnect");
        if (codec::inspect(*duplicateResponse).payload
            == PayloadKind::ErrorResponse) {
            duplicateRejected
                = codec::fromWire(
                      *duplicateResponse->payload.AsErrorResponse())
                      .code
                == ErrorCode::InvalidRequest;
        }
    }
    require(duplicateRejected,
        "duplicate live request ID did not receive a bounded error");
    require(duplicateRunning.stopWithin(std::chrono::seconds{2}),
        "duplicate-ID server shutdown exceeded its deadline");

#ifdef AMREXPLORER_SERVER_TEST_HOOKS
    // A response-write failure must retire that session and release the shared
    // worker for another connection. Frame's socket-pair test proves the real
    // write timeout; this integration test injects the same failure at the
    // server boundary so worker ordering is deterministic and independent of
    // response size, socket buffers, and scheduler timing.
    ServerOptions stalledOptions;
    stalledOptions.workerCount = 1;
    stalledOptions.maximumDatasets = 1;
    stalledOptions.maximumConnections = 2;
    Server stalledServer(stalledOptions);
    RunningServer stalledRunning(stalledServer);
    auto stalledSocket = connectTo("127.0.0.1", stalledServer.port());
    envelope = exchange(stalledSocket, 1,
        codec::toWire(helloRequest(stalledServer.token())),
        defaultMaximumFrameBytes);
    const auto stalledHello
        = codec::fromWire(*envelope->payload.AsHelloResponse());
    envelope = exchange(stalledSocket, 2, codec::toWire(duplicateOpen),
        stalledHello.maximumFrameBytes);
    const auto stalledOpened
        = codec::fromWire(*envelope->payload.AsDatasetOpened());
    SliceRequest stalledSlice = duplicateSlice;
    stalledSlice.dataset = stalledOpened.id;
    stalledSlice.outputSize = {2, 2};

    std::promise<void> stalledWriteEnteredPromise;
    auto stalledWriteEntered = stalledWriteEnteredPromise.get_future();
    std::promise<void> releaseStalledWritePromise;
    const auto releaseStalledWrite
        = releaseStalledWritePromise.get_future().share();
    std::atomic_bool injectedWriteFailure{false};
    testing::setBeforeResponseWrite(
        [&](std::uint64_t requestId) {
            if (requestId == 3 && !injectedWriteFailure.exchange(true)) {
                stalledWriteEnteredPromise.set_value();
                releaseStalledWrite.wait();
                throw std::runtime_error(
                    "injected stalled response write failure");
            }
        });
    writeFrame(stalledSocket,
        codec::encode(3, codec::toWire(stalledSlice)),
        stalledHello.maximumFrameBytes);
    require(stalledWriteEntered.wait_for(std::chrono::seconds{10})
            == std::future_status::ready,
        "stalled response never reached the server write boundary");

    auto healthySocket = connectTo("127.0.0.1", stalledServer.port());
    envelope = exchange(healthySocket, 1,
        codec::toWire(helloRequest(stalledServer.token())),
        defaultMaximumFrameBytes);
    const auto healthyHello
        = codec::fromWire(*envelope->payload.AsHelloResponse());
    writeFrame(healthySocket,
        codec::encode(2, codec::toWire(duplicateOpen)),
        healthyHello.maximumFrameBytes);
    testing::clearBeforeResponseWrite();
    releaseStalledWritePromise.set_value();
    envelope = readWithDeadline(healthySocket,
        healthyHello.maximumFrameBytes, std::chrono::seconds{10});
    require(envelope != nullptr && envelope->request_id == 2,
        "healthy request received no bounded response");
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetOpened,
        "stalled response retained the shared server worker");
    bool stalledRetired = false;
    try {
        stalledRetired = readWithDeadline(stalledSocket,
            stalledHello.maximumFrameBytes, std::chrono::seconds{2})
            == nullptr;
    } catch (const std::exception&) {
        stalledRetired = true;
    }
    require(stalledRetired,
        "server did not retire the stalled response session");
    require(stalledRunning.stopWithin(std::chrono::seconds{2}),
        "stalled-response server shutdown exceeded its deadline");
#endif

    codec::fb::CloseDatasetRequestT close;
    close.dataset_id = opened.id.value;
    envelope = exchange(socket, 9, std::move(close),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetClosed,
        "server did not close the dataset");
    require(running.stopWithin(std::chrono::seconds{2}),
        "server shutdown exceeded its deadline");

    ServerOptions connectionOptions;
    connectionOptions.workerCount = 1;
    connectionOptions.maximumConnections = 1;
    Server connectionLimitedServer(connectionOptions);
    RunningServer connectionLimitedRunning(connectionLimitedServer);
    auto allowedSocket
        = connectTo("127.0.0.1", connectionLimitedServer.port());
    envelope = exchange(allowedSocket, 1,
        codec::toWire(helloRequest(connectionLimitedServer.token())),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "server rejected the allowed connection");
    auto excessSocket
        = connectTo("127.0.0.1", connectionLimitedServer.port());
    bool excessDisconnected = false;
    try {
        excessDisconnected = readWithDeadline(excessSocket,
            defaultMaximumFrameBytes, std::chrono::seconds{2})
            == nullptr;
    } catch (const std::exception&) {
        excessDisconnected = true;
    }
    require(excessDisconnected,
        "server did not enforce the configured connection limit");
    require(connectionLimitedRunning.stopWithin(std::chrono::seconds{2}),
        "connection-limited server shutdown exceeded its deadline");

    ServerOptions handshakeTimeoutOptions;
    handshakeTimeoutOptions.workerCount = 1;
    handshakeTimeoutOptions.maximumConnections = 1;
    handshakeTimeoutOptions.handshakeTimeout
        = std::chrono::milliseconds{100};
    Server handshakeTimeoutServer(handshakeTimeoutOptions);
    RunningServer handshakeTimeoutRunning(handshakeTimeoutServer);
    auto silentSocket
        = connectTo("127.0.0.1", handshakeTimeoutServer.port());
    const auto silentStart = std::chrono::steady_clock::now();
    require(readWithDeadline(silentSocket, defaultMaximumFrameBytes,
                std::chrono::seconds{2})
            == nullptr,
        "silent unauthenticated connection did not time out");
    require(std::chrono::steady_clock::now() - silentStart
            < std::chrono::seconds{1},
        "silent unauthenticated connection exceeded its handshake deadline");
    auto recoveredSocket
        = connectTo("127.0.0.1", handshakeTimeoutServer.port());
    envelope = exchange(recoveredSocket, 1,
        codec::toWire(helloRequest(handshakeTimeoutServer.token())),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "authorized client could not reclaim a timed-out connection slot");
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    codec::fb::PingRequestT delayedPing;
    delayedPing.nonce = 17;
    envelope = exchange(recoveredSocket, 2, std::move(delayedPing),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::PongResponse,
        "server retained the handshake deadline after authentication");
    require(handshakeTimeoutRunning.stopWithin(std::chrono::seconds{2}),
        "handshake-timeout server shutdown exceeded its deadline");

    ServerOptions handshakeFrameOptions;
    handshakeFrameOptions.workerCount = 1;
    handshakeFrameOptions.maximumConnections = 1;
    handshakeFrameOptions.sessionToken = "bounded-handshake-token";
    const auto normalHelloBytes = codec::encode(
        1, codec::toWire(helloRequest(handshakeFrameOptions.sessionToken)));
    handshakeFrameOptions.maximumHandshakeFrameBytes
        = static_cast<std::uint32_t>(normalHelloBytes.size() + 16);
    Server handshakeFrameServer(handshakeFrameOptions);
    RunningServer handshakeFrameRunning(handshakeFrameServer);
    auto oversizedHello = helloRequest(handshakeFrameServer.token());
    oversizedHello.clientName.assign(
        handshakeFrameOptions.maximumHandshakeFrameBytes, 'x');
    auto oversizedHelloBytes
        = codec::encode(1, codec::toWire(std::move(oversizedHello)));
    require(oversizedHelloBytes.size()
            > handshakeFrameOptions.maximumHandshakeFrameBytes,
        "oversized hello fixture did not exceed the handshake frame cap");
    auto oversizedHelloSocket
        = connectTo("127.0.0.1", handshakeFrameServer.port());
    writeFrame(oversizedHelloSocket, oversizedHelloBytes,
        defaultMaximumFrameBytes);
    bool oversizedHelloRejected = false;
    try {
        oversizedHelloRejected = readWithDeadline(oversizedHelloSocket,
                                     defaultMaximumFrameBytes,
                                     std::chrono::seconds{2})
            == nullptr;
    } catch (const std::exception&) {
        oversizedHelloRejected = true;
    }
    require(oversizedHelloRejected,
        "server accepted a hello above the pre-authentication frame cap");
    auto boundedHandshakeSocket
        = connectTo("127.0.0.1", handshakeFrameServer.port());
    envelope = exchange(boundedHandshakeSocket, 1,
        codec::toWire(helloRequest(handshakeFrameServer.token())),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "server rejected a hello within the pre-authentication frame cap");
    envelope = exchange(boundedHandshakeSocket, 2,
        codec::toWire(OpenDatasetData{
            std::string(
                handshakeFrameOptions.maximumHandshakeFrameBytes * 2U, 'x'),
            16ULL * 1024ULL * 1024ULL, {}}),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse,
        "server retained the handshake frame cap after authentication");
    require(handshakeFrameRunning.stopWithin(std::chrono::seconds{2}),
        "handshake-frame server shutdown exceeded its deadline");

    ServerOptions boundedOptions;
    boundedOptions.workerCount = 1;
    boundedOptions.maximumDatasets = 1;
    boundedOptions.maximumFrameBytes = 1024;
    Server boundedServer(boundedOptions);
    RunningServer boundedRunning(boundedServer);
    auto boundedSocket = connectTo("127.0.0.1", boundedServer.port());
    auto boundedHello = helloRequest(boundedServer.token());
    boundedHello.maximumFrameBytes = boundedOptions.maximumFrameBytes;
    envelope = exchange(boundedSocket, 1, codec::toWire(boundedHello),
        boundedOptions.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "small-frame server rejected a fitting hello response");
    for (std::uint64_t requestId : {2ULL, 3ULL}) {
        envelope = exchange(boundedSocket, requestId,
            codec::toWire(OpenDatasetData{
                std::filesystem::path(argv[1]).string(),
                16ULL * 1024ULL * 1024ULL, {}}),
            boundedOptions.maximumFrameBytes);
        require(codec::inspect(*envelope).payload
                    == PayloadKind::ErrorResponse
                && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                    == ErrorCode::ResourceLimitExceeded
                && codec::fromWire(*envelope->payload.AsErrorResponse())
                       .message.find("catalog")
                    != std::string::npos,
            "oversized dataset catalog was not rejected before publication");
    }
    require(boundedRunning.stopWithin(std::chrono::seconds{2}),
        "small-frame server shutdown exceeded its deadline");

#ifdef AMREXPLORER_SERVER_TEST_HOOKS
    // A peer that keeps accepting a trickle of bytes renews the no-progress
    // deadline forever, so the stall timeout alone would let it hold a pooled
    // worker and this session's write mutex indefinitely. The whole-response
    // budget must retire it within a bound the operator can compute, and a
    // second connection must keep working while it is still being retired.
    //
    // The trickle is injected rather than provoked. Producing one from a real
    // socket needs a response too big to fit in the kernel's buffers, which
    // meant a 2048x2048 slice -- and the sampling and encoding of it were paid
    // before the write this case actually measures even began. Pacing the write
    // to one byte per 50 ms gives the same property from a 16x16 response:
    // progress lands well inside every 500 ms stall interval, so the
    // no-progress timeout can never fire, while one byte per 50 ms is orders of
    // magnitude under the floor, so only the whole-response budget can end it.
    // A server without that budget would never retire this session at all.
    // test_remote_frame still pins down which limit fires, on a socket whose
    // buffers it controls.
    {
        ServerOptions trickleOptions;
        trickleOptions.workerCount = 2;
        trickleOptions.maximumDatasets = 2;
        trickleOptions.maximumConnections = 4;
        // Ten stall intervals fit inside the paced write below, so a stall
        // firing instead of the budget would be a defect, not a slow runner.
        trickleOptions.responseWriteStallTimeout
            = std::chrono::milliseconds{500};
        // 8 KiB/s over a response of a few kibibytes: half a second of grace
        // plus well under a second of transfer allowance.
        //
        // Retirement lands at stallTimeout + payloadBytes / minBytesPerSecond,
        // so the margin the `elapsed > stallTimeout` assertion below rests on
        // is the transfer term alone. It cannot go negative, and elapsed only
        // grows under load, so this is not a flake; but shrinking the response
        // or raising the floor shrinks that term, and past some point the
        // assertion stops distinguishing the budget from the stall in any
        // meaningful way. Retune the two together, not one at a time.
        trickleOptions.responseWriteMinimumBytesPerSecond = 8ULL * 1024ULL;
        Server trickleServer(trickleOptions);
        RunningServer trickleRunning(trickleServer);

        auto slow = connectTo("127.0.0.1", trickleServer.port());
        envelope = exchange(slow, 1,
            codec::toWire(helloRequest(trickleServer.token())),
            defaultMaximumFrameBytes);
        require(codec::inspect(*envelope).payload
                == PayloadKind::HelloResponse,
            "trickle server rejected a valid handshake");
        envelope = exchange(slow, 2,
            codec::toWire(OpenDatasetData{
                std::filesystem::path(argv[1]).string(),
                16ULL * 1024ULL * 1024ULL, {}}),
            defaultMaximumFrameBytes);
        require(codec::inspect(*envelope).payload
                == PayloadKind::DatasetOpened,
            "trickle server did not open the plotfile");
        const auto trickleOpened = codec::fromWire(
            *envelope->payload.AsDatasetOpened());

        // Both hooks run on the worker thread serving one response, in this
        // order, so the first can tell the second whether this is the write to
        // pace. Without that, pacing would also throttle the neighbour's
        // responses below and retire that healthy session too.
        constexpr std::uint64_t pacedRequestId = 4242;
        testing::setBeforeResponseWrite([](std::uint64_t requestId) {
            g_paceThisWrite = requestId == pacedRequestId;
        });
        testing::setWriteChunkLimit([](std::size_t requested) {
            if (!g_paceThisWrite) {
                return requested;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            return std::size_t{1};
        });

        SliceRequest paced;
        paced.dataset = trickleOpened.id;
        paced.field = FieldId{0};
        paced.normalDirection = 1;
        paced.visibleRegion = trickleOpened.catalog.physicalDomain;
        paced.physicalPosition = 0.5
            * (paced.visibleRegion.lower[1] + paced.visibleRegion.upper[1]);
        paced.maximumLevel = trickleOpened.catalog.finestLevel;
        paced.outputSize = {16, 16};
        writeFrame(slow, codec::encode(pacedRequestId, codec::toWire(paced)),
            defaultMaximumFrameBytes);

        std::atomic_bool draining{true};
        std::atomic<std::uint64_t> drained{0};
        auto retired = std::async(std::launch::async,
            [&slow, &draining, &drained] {
                const auto start = std::chrono::steady_clock::now();
                std::vector<std::uint8_t> chunk(4U * 1024U);
                while (draining.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{20});
                    const auto count = readRaw(slow, chunk);
                    if (count == 0) {
                        break;  // the server retired the session
                    }
                    if (count > 0) {
                        drained += static_cast<std::uint64_t>(count);
                    }
                }
                return std::chrono::steady_clock::now() - start;
            });

        // While that response is still being written, a second authenticated
        // connection opens the dataset and gets a small response.
        auto neighbour = connectTo("127.0.0.1", trickleServer.port());
        envelope = exchange(neighbour, 1,
            codec::toWire(helloRequest(trickleServer.token())),
            defaultMaximumFrameBytes);
        require(codec::inspect(*envelope).payload
                == PayloadKind::HelloResponse,
            "the trickle reader blocked a second handshake");
        envelope = exchange(neighbour, 2,
            codec::toWire(OpenDatasetData{
                std::filesystem::path(argv[1]).string(),
                16ULL * 1024ULL * 1024ULL, {}}),
            defaultMaximumFrameBytes);
        require(codec::inspect(*envelope).payload
                == PayloadKind::DatasetOpened,
            "the trickle reader blocked a second dataset open");
        const auto neighbourOpened = codec::fromWire(
            *envelope->payload.AsDatasetOpened());
        DatasetPageRequest neighbourPage;
        neighbourPage.dataset = neighbourOpened.id;
        neighbourPage.field = FieldId{0};
        neighbourPage.level = 0;
        neighbourPage.region = neighbourOpened.catalog.physicalDomain;
        neighbourPage.normalAxis = 1;
        neighbourPage.maximumExtent = datasetPageMaxExtent;
        envelope = exchange(neighbour, 3, codec::toWire(neighbourPage),
            defaultMaximumFrameBytes);
        require(codec::inspect(*envelope).payload
                == PayloadKind::DatasetPageResponse,
            "the trickle reader blocked a second small response");

        // The slow session is retired on its own, without the server being
        // stopped: the write throws past its budget and the session shuts down.
        const auto retirementBound = std::chrono::seconds{15};
        if (retired.wait_for(retirementBound) != std::future_status::ready) {
            std::cerr << "trickle reader drained " << drained.load()
                      << " bytes without the session being retired\n";
            slow.shutdown();
            retired.wait();
            testing::clearWriteChunkLimit();
            testing::clearBeforeResponseWrite();
            require(false, "the trickle-reading session was never retired");
        }
        draining.store(false);
        const auto elapsed = retired.get();
        testing::clearWriteChunkLimit();
        testing::clearBeforeResponseWrite();
        require(elapsed < retirementBound,
            "the trickle-reading session outlived its retirement bound");
        // The write outlived the stall grace while still moving bytes, so it was
        // the whole-response budget that retired it: the pacing above never let
        // 500 ms pass without progress.
        if (elapsed <= trickleOptions.responseWriteStallTimeout) {
            std::cerr << "trickle session ended after "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             elapsed)
                             .count()
                      << " ms having drained " << drained.load()
                      << " bytes\n";
        }
        require(elapsed > trickleOptions.responseWriteStallTimeout,
            "the trickle-reading session ended within the stall grace");
        require(trickleRunning.stopWithin(std::chrono::seconds{5}),
            "trickle server shutdown exceeded its deadline");
    }
#endif

    std::filesystem::remove_all(
        std::filesystem::path(argv[1]) / "Oversized");
    std::filesystem::remove_all(
        std::filesystem::path(argv[1]) / "GridBomb");
    std::filesystem::remove_all(
        std::filesystem::path(argv[1]) / "CraftedHeader");
    return 0;
}
