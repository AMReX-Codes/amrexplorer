#include "Codec.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

template <typename Function>
void requireRejected(Function&& function, const char* message)
{
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    require(false, message);
}

amrvis::remote::codec::Bytes preCapabilitiesHelloFixture()
{
    namespace codec = amrvis::remote::codec;
    flatbuffers::FlatBufferBuilder builder;
    const auto client = builder.CreateString("legacy client");
    const auto version = builder.CreateString("1");
    const auto token = builder.CreateString("legacy-token");
    // Omitting the final capabilities field produces the same table layout as
    // the pre-capabilities protocol 1.0 schema.
    const auto hello = codec::fb::CreateHelloRequest(builder, client, version,
        0, amrvis::remote::protocolMinorVersion, 4096, token);
    const auto envelope = codec::fb::CreateEnvelope(builder,
        amrvis::remote::protocolMajor,
        amrvis::remote::protocolMinorVersion, 41,
        codec::fb::Payload::HelloRequest, hello.Union());
    codec::fb::FinishEnvelopeBuffer(builder, envelope);
    return {builder.GetBufferPointer(),
        builder.GetBufferPointer() + builder.GetSize()};
}

} // namespace

int main()
{
    using namespace amrvis;
    using namespace amrvis::remote;

    HelloRequestData hello{
        "codec test", "1", 0, protocolMinorVersion, 4096, "test-token",
        {7, 9}};
    auto bytes = codec::encode(7, codec::toWire(hello));
    auto envelope = codec::decode(bytes);
    require(codec::inspect(*envelope).requestId == 7,
        "hello request ID did not round-trip");
    const auto decodedHello
        = codec::fromWire(*envelope->payload.AsHelloRequest());
    require(decodedHello.clientName == hello.clientName,
        "hello payload did not round-trip");
    require(decodedHello.sessionToken == hello.sessionToken,
        "hello session token did not round-trip");
    require(codec::fromWire(*envelope->payload.AsHelloRequest()).capabilities
            == hello.capabilities,
        "hello capabilities did not round-trip");

    const std::array payloadKinds{
        PayloadKind::HelloRequest,
        PayloadKind::HelloResponse,
        PayloadKind::OpenDatasetRequest,
        PayloadKind::DatasetOpened,
        PayloadKind::CloseDatasetRequest,
        PayloadKind::DatasetClosed,
        PayloadKind::SliceViewRequest,
        PayloadKind::SliceViewResponse,
        PayloadKind::LineViewRequest,
        PayloadKind::LineViewResponse,
        PayloadKind::DatasetPageRequest,
        PayloadKind::DatasetPageResponse,
        PayloadKind::ParticleSampleRequest,
        PayloadKind::ParticleSampleResponse,
        PayloadKind::RangeRequest,
        PayloadKind::RangeResponse,
        PayloadKind::ClearCacheRequest,
        PayloadKind::SetCacheBudgetRequest,
        PayloadKind::CacheResponse,
        PayloadKind::CancelRequest,
        PayloadKind::CancelAcknowledged,
        PayloadKind::PingRequest,
        PayloadKind::PongResponse,
        PayloadKind::ErrorResponse,
    };
    for (const auto kind : payloadKinds) {
        codec::NativeEnvelope native;
        native.request_id = 1;
        native.payload.type = static_cast<codec::fb::Payload>(kind);
        require(codec::inspect(native).payload == kind,
            "payload enum value did not round-trip");
    }

    const std::array errorCodes{
        ErrorCode::UnsupportedProtocol,
        ErrorCode::InvalidRequest,
        ErrorCode::UnknownDataset,
        ErrorCode::DatasetOpenFailure,
        ErrorCode::Cancelled,
        ErrorCode::CacheBudgetExceeded,
        ErrorCode::ResourceLimitExceeded,
        ErrorCode::OperationFailure,
        ErrorCode::InternalServerError,
        ErrorCode::Disconnected,
        ErrorCode::Unauthorized,
    };
    for (const auto code : errorCodes) {
        const auto decodedError = codec::fromWire(
            codec::toWire(ErrorData{code, "test"}));
        require(decodedError.code == code,
            "error enum value did not round-trip");
    }

    const auto legacyBytes = preCapabilitiesHelloFixture();
    const auto legacyEnvelope = codec::decode(legacyBytes);
    const auto legacyHello = codec::fromWire(
        *legacyEnvelope->payload.AsHelloRequest());
    require(legacyHello.sessionToken == "legacy-token"
            && legacyHello.capabilities.empty(),
        "pre-capabilities hello fixture did not decode compatibly");
    OpenedDataset opened;
    opened.id = DatasetId{9};
    opened.catalog.dimension = 3;
    opened.catalog.finestLevel = 0;
    opened.catalog.physicalDomain = RealBox{
        Real3{{0.0, 0.0, 0.0}}, Real3{{1.0, 1.0, 1.0}}};
    LevelMetadata level;
    level.level = 0;
    level.domain = IntBox{
        Int3{{0, 0, 0}}, Int3{{3, 3, 3}}, Int3{{1, 0, 0}}};
    level.boxes.push_back(
        IntBox{Int3{{0, 0, 0}}, Int3{{1, 3, 3}}, Int3{{1, 0, 0}}});
    // A one-cell box: an IntBox is allowed equal corners, unlike a RealBox.
    level.boxes.push_back(
        IntBox{Int3{{2, 2, 2}}, Int3{{2, 2, 2}}, Int3{{1, 0, 0}}});
    opened.catalog.levels.push_back(level);
    bytes = codec::encode(8, codec::toWire(opened));
    envelope = codec::decode(bytes);
    const auto openedDecoded = codec::fromWire(
        *envelope->payload.AsDatasetOpened());
    require(openedDecoded.catalog.levels.size() == 1
            && openedDecoded.catalog.levels.front().boxes == level.boxes,
        "AMR wireframe boxes did not round-trip in the catalog");

    SliceQueryResult slice;
    slice.plane.width = 2;
    slice.plane.height = 1;
    slice.plane.physicalRegion = RealBox{
        Real3{{0.0, 0.0, 0.0}}, Real3{{1.0, 1.0, 0.0}}};
    slice.plane.values = {1.0F, 2.0F};
    slice.plane.valid = {1, 1};
    slice.plane.sourceLevel = {0, 1};
    slice.gridBoxesIncluded = true;
    slice.gridBoxesTruncated = true;
    slice.gridBoxes.push_back(
        {1, RealBox{Real3{{0.5, 0.0, 0.0}},
                Real3{{1.0, 1.0, 0.0}}}});
    bytes = codec::encode(
        10, codec::toWire(slice, CacheMetrics{}));
    envelope = codec::decode(bytes);
    const auto decoded = codec::fromWire(
        *envelope->payload.AsSliceViewResponse());
    require(decoded.plane.values == slice.plane.values
            && decoded.gridBoxesIncluded
            && decoded.gridBoxesTruncated
            && decoded.gridBoxes.size() == 1
            && decoded.gridBoxes.front().level == 1
            && decoded.gridBoxes.front().physicalRegion
                == slice.gridBoxes.front().physicalRegion,
        "bounded slice response did not round-trip");

    auto wrongIdentifier = bytes;
    wrongIdentifier[4] = 'X';
    requireRejected([&] { static_cast<void>(
                        codec::decode(wrongIdentifier)); },
        "wrong FlatBuffers identifier was accepted");

    const auto truncated = std::span<const std::uint8_t>(
        bytes.data(), bytes.size() - 1);
    requireRejected([&] { static_cast<void>(codec::decode(truncated)); },
        "truncated FlatBuffer was accepted");

    requireRejected([&] { static_cast<void>(
                        codec::encode(0, codec::toWire(hello))); },
        "zero request ID was accepted for encoding");

    flatbuffers::FlatBufferBuilder zeroIdBuilder;
    const auto zeroIdHello = codec::fb::CreateHelloRequest(zeroIdBuilder);
    const auto zeroIdEnvelope = codec::fb::CreateEnvelope(zeroIdBuilder,
        protocolMajor, protocolMinorVersion, 0,
        codec::fb::Payload::HelloRequest,
        zeroIdHello.Union());
    codec::fb::FinishEnvelopeBuffer(zeroIdBuilder, zeroIdEnvelope);
    const auto zeroIdBytes = std::span<const std::uint8_t>(
        zeroIdBuilder.GetBufferPointer(), zeroIdBuilder.GetSize());
    requireRejected([&] { static_cast<void>(codec::decode(zeroIdBytes)); },
        "zero request ID was accepted while decoding");

    codec::fb::SliceViewResponseT inconsistent;
    inconsistent.width = 2;
    inconsistent.height = 2;
    inconsistent.physical_region
        = codec::toWire(slice.plane.physicalRegion);
    inconsistent.values = {1.0F};
    inconsistent.valid = {1};
    inconsistent.source_level = {0};
    requireRejected([&] { static_cast<void>(
                        codec::fromWire(inconsistent)); },
        "inconsistent slice vectors were accepted");

    auto openedWire = codec::toWire(opened);
    openedWire.dimension = 4;
    requireRejected([&] { static_cast<void>(codec::fromWire(openedWire)); },
        "out-of-range dataset dimension was accepted");

    openedWire = codec::toWire(opened);
    openedWire.finest_level = -1;
    requireRejected([&] { static_cast<void>(codec::fromWire(openedWire)); },
        "negative finest level was accepted");

    openedWire = codec::toWire(opened);
    openedWire.level_range_available = {1};
    requireRejected([&] { static_cast<void>(codec::fromWire(openedWire)); },
        "level ranges with an empty field catalog were accepted");

    openedWire = codec::toWire(opened);
    openedWire.time = std::numeric_limits<double>::infinity();
    requireRejected([&] { static_cast<void>(codec::fromWire(openedWire)); },
        "non-finite dataset time was accepted");

    auto badCentering = std::make_unique<codec::fb::FieldCatalogT>();
    badCentering->centering = static_cast<codec::fb::Centering>(255);
    openedWire = codec::toWire(opened);
    openedWire.fields.push_back(std::move(badCentering));
    openedWire.file_range_available = {0};
    openedWire.level_range_available = {0};
    requireRejected([&] { static_cast<void>(codec::fromWire(openedWire)); },
        "unknown centering enum was accepted");

    codec::fb::SliceViewRequestT badSampling;
    badSampling.visible_region = codec::toWire(slice.plane.physicalRegion);
    badSampling.sampling = static_cast<codec::fb::SamplingPolicy>(255);
    requireRejected([&] { static_cast<void>(codec::fromWire(badSampling)); },
        "unknown sampling enum was accepted");

    codec::fb::RangeRequestT badScope;
    badScope.scope = static_cast<codec::fb::RangeScope>(255);
    requireRejected([&] { static_cast<void>(codec::fromWire(badScope)); },
        "unknown range-scope enum was accepted");

    codec::fb::ErrorResponseT badError;
    badError.code = static_cast<codec::fb::ErrorCode>(65535);
    requireRejected([&] { static_cast<void>(codec::fromWire(badError)); },
        "unknown error enum was accepted");

    codec::fb::Real3T nonFinite;
    nonFinite.values = {0.0, std::numeric_limits<double>::quiet_NaN(), 1.0};
    requireRejected([&] { static_cast<void>(codec::fromWire(&nonFinite)); },
        "non-finite geometry was accepted");

    // The decoded catalog must prove what a local reader proves. Each of these
    // is structurally valid wire that no local open would have produced.
    openedWire = codec::toWire(opened);
    openedWire.levels.front()->boxes.front()->centering
        = codec::toWire(Int3{{0, 0, 0}});
    requireRejected([&] { static_cast<void>(codec::fromWire(openedWire)); },
        "a level box disagreeing with its domain centering was accepted");

    openedWire = codec::toWire(opened);
    openedWire.levels.front()->boxes.front()->upper
        = codec::toWire(Int3{{9, 3, 3}});
    requireRejected([&] { static_cast<void>(codec::fromWire(openedWire)); },
        "a level box outside the level domain was accepted");

    openedWire = codec::toWire(opened);
    openedWire.levels.front()->boxes.front()->lower
        = codec::toWire(Int3{{2, 0, 0}});
    openedWire.levels.front()->boxes.front()->upper
        = codec::toWire(Int3{{1, 3, 3}});
    requireRejected([&] { static_cast<void>(codec::fromWire(openedWire)); },
        "a reversed 3-D level box was accepted");

    openedWire = codec::toWire(opened);
    openedWire.levels.front()->cell_size = codec::toWire(Real3{{0.0, 1.0, 1.0}});
    requireRejected([&] { static_cast<void>(codec::fromWire(openedWire)); },
        "a zero cell size was accepted");

    openedWire = codec::toWire(opened);
    openedWire.levels.front()->level = 3;
    requireRejected([&] { static_cast<void>(codec::fromWire(openedWire)); },
        "a level numbered other than its index was accepted");

    openedWire = codec::toWire(opened);
    openedWire.physical_domain = codec::toWire(
        RealBox{Real3{{1.0, 0.0, 0.0}}, Real3{{0.0, 1.0, 1.0}}});
    requireRejected([&] { static_cast<void>(codec::fromWire(openedWire)); },
        "a reversed physical domain was accepted");

    // A 2-D catalog is free to be degenerate on the inactive third axis, and a
    // reversed box there must stay acceptable: only active axes carry meaning.
    OpenedDataset flat;
    flat.id = DatasetId{4};
    flat.catalog.dimension = 2;
    flat.catalog.finestLevel = 0;
    flat.catalog.physicalDomain = RealBox{
        Real3{{0.0, 0.0, 1.0}}, Real3{{1.0, 1.0, 0.0}}};
    LevelMetadata flatLevel;
    flatLevel.level = 0;
    flatLevel.domain = IntBox{
        Int3{{0, 0, 7}}, Int3{{3, 3, 2}}, Int3{{0, 0, 0}}};
    flatLevel.boxes.push_back(flatLevel.domain);
    flat.catalog.levels.push_back(flatLevel);
    const auto flatDecoded = codec::fromWire(codec::toWire(flat));
    require(flatDecoded.catalog.levels.size() == 1
            && flatDecoded.catalog.levels.front().boxes.front()
                == flatLevel.domain,
        "a 2-D catalog with a degenerate inactive axis was rejected");

    // A named species with a dimension outside [1, 3] cannot describe points.
    auto badSpecies = codec::toWire(opened);
    badSpecies.particle_species.push_back(
        std::make_unique<codec::fb::ParticleSpeciesCatalogT>());
    badSpecies.particle_species.back()->name = "electrons";
    badSpecies.particle_species.back()->dimension = 4;
    requireRejected([&] { static_cast<void>(codec::fromWire(badSpecies)); },
        "a particle species with an impossible dimension was accepted");

    ParticleSample sample;
    sample.species = {"electrons", 3, 0, 0, 4, ParticleRealPrecision::Double};
    sample.points.push_back({1, Real3{{0.25, 0.5, 0.75}}});
    auto particleWire = codec::toWire(sample, CacheMetrics{});
    const auto particleDecoded = codec::fromWire(particleWire);
    require(particleDecoded.points.size() == 1
            && particleDecoded.points.front().position
                == sample.points.front().position,
        "a particle sample did not round-trip");
    particleWire = codec::toWire(sample, CacheMetrics{});
    particleWire.positions[1] = std::numeric_limits<double>::quiet_NaN();
    requireRejected([&] { static_cast<void>(codec::fromWire(particleWire)); },
        "a non-finite particle position was accepted");
    particleWire = codec::toWire(sample, CacheMetrics{});
    particleWire.positions[2] = std::numeric_limits<double>::infinity();
    requireRejected([&] { static_cast<void>(codec::fromWire(particleWire)); },
        "an infinite particle position was accepted");
    return 0;
}
