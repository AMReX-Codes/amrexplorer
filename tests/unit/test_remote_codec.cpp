#include "Codec.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
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

// A LineViewResponse whose [double] positions vector is only 4-byte aligned,
// which flatbuffers' own Verifier permits (it checks the vector's length
// prefix, not its elements). `pad` shifts the vector by four bytes relative to
// the buffer start, so exactly one of the two layouts leaves the doubles
// misaligned and the other is the control that must still decode.
amrvis::remote::codec::Bytes fourByteAlignedPositionsFixture(bool pad)
{
    namespace codec = amrvis::remote::codec;
    flatbuffers::FlatBufferBuilder builder;
    if (pad) {
        // An orphan empty vector: four bytes nothing references, which shifts
        // everything built after it (hence before it in memory) by four.
        static_cast<void>(builder.CreateVector(std::vector<std::uint8_t>{}));
    }
    std::uint8_t* raw = nullptr;
    const flatbuffers::Offset<flatbuffers::Vector<double>> positions(
        builder.CreateUninitializedVector(2, sizeof(double), 4, &raw));
    const std::array<double, 2> values{0.5, 1.5};
    std::memcpy(raw, values.data(), sizeof(values));
    const auto response
        = codec::fb::CreateLineViewResponse(builder, 0, false, positions);
    const auto envelope = codec::fb::CreateEnvelope(builder,
        amrvis::remote::protocolMajor,
        amrvis::remote::protocolMinorVersion, 43,
        codec::fb::Payload::LineViewResponse, response.Union());
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
        PayloadKind::ListDirectoryRequest,
        PayloadKind::DirectoryListing,
        PayloadKind::RenderedFrameRequest,
        PayloadKind::RenderedFrameResponse,
    };
    for (const auto kind : payloadKinds) {
        codec::NativeEnvelope native;
        native.request_id = 1;
        native.payload.type = static_cast<codec::fb::Payload>(kind);
        require(codec::inspect(native).payload == kind,
            "payload enum value did not round-trip");
    }

    RemoteDirectoryListing listing{"/scratch/run", "/scratch",
        {{"plt00010", "/scratch/run/plt00010", true},
            {"inputs", "/scratch/run/inputs", false}},
        true};
    const auto decodedListing = codec::fromWire(codec::toWire(listing));
    require(decodedListing.path == listing.path
            && decodedListing.parentPath == listing.parentPath
            && decodedListing.truncated && decodedListing.entries.size() == 2
            && decodedListing.entries.front().isPlotfile
            && !decodedListing.entries.back().isPlotfile
            && decodedListing.entries.front().path
                == listing.entries.front().path,
        "directory listing did not round-trip");
    // A backslash is a legal filename character on the Linux servers this
    // client browses; the decode must not reject the whole listing for it.
    RemoteDirectoryListing backslashListing{"/scratch/run", "/scratch",
        {{"run\\final", "/scratch/run/run\\final", true}}, false};
    require(codec::fromWire(codec::toWire(backslashListing)).entries.size()
            == 1,
        "backslash directory entry did not round-trip");
    bool rejectedSlashEntry = false;
    try {
        RemoteDirectoryListing bad{"/scratch", "/",
            {{"a/b", "/scratch/a/b", false}}, false};
        static_cast<void>(codec::fromWire(codec::toWire(bad)));
    } catch (const std::invalid_argument&) {
        rejectedSlashEntry = true;
    }
    require(rejectedSlashEntry, "slash in a directory entry name was accepted");

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

    // flatbuffers' Verifier does not check vector element alignment, so
    // decode must reject a misaligned [double] vector itself rather than let
    // UnPack read it.
    int misalignedLayouts = 0;
    for (const bool pad : {false, true}) {
        const auto layout = fourByteAlignedPositionsFixture(pad);
        flatbuffers::Verifier verifier(layout.data(), layout.size());
        require(codec::fb::VerifyEnvelopeBuffer(verifier),
            "four-byte-aligned positions layout failed the verifier");
        const auto* positions = codec::fb::GetEnvelope(layout.data())
            ->payload_as_LineViewResponse()->positions();
        const bool aligned = static_cast<std::size_t>(
            positions->Data() - layout.data()) % alignof(double) == 0;
        if (aligned) {
            const auto decodedLayout = codec::decode(layout);
            require(decodedLayout->payload.AsLineViewResponse()->positions
                    == std::vector<double>{0.5, 1.5},
                "aligned positions layout did not decode");
        } else {
            ++misalignedLayouts;
            requireRejected([&] { static_cast<void>(codec::decode(layout)); },
                "misaligned [double] vector was accepted while decoding");
        }
    }
    require(misalignedLayouts == 1,
        "the fixture did not produce a misaligned [double] layout");

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

    // The local parser refuses component counts outside [0, 100000]; so must the
    // wire, or a species that cannot exist reaches the client.
    for (const int realCount : {-1, maximumParticleComponents + 1}) {
        auto badComponents = codec::toWire(opened);
        badComponents.particle_species.push_back(
            std::make_unique<codec::fb::ParticleSpeciesCatalogT>());
        badComponents.particle_species.back()->name = "electrons";
        badComponents.particle_species.back()->dimension = 3;
        badComponents.particle_species.back()->real_component_count
            = realCount;
        requireRejected(
            [&] { static_cast<void>(codec::fromWire(badComponents)); },
            "an out-of-range real component count was accepted");
    }
    for (const int intCount : {-2, maximumParticleComponents + 1}) {
        auto badComponents = codec::toWire(opened);
        badComponents.particle_species.push_back(
            std::make_unique<codec::fb::ParticleSpeciesCatalogT>());
        badComponents.particle_species.back()->name = "electrons";
        badComponents.particle_species.back()->dimension = 3;
        badComponents.particle_species.back()->int_component_count = intCount;
        requireRejected(
            [&] { static_cast<void>(codec::fromWire(badComponents)); },
            "an out-of-range integer component count was accepted");
    }

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

    // Protocol 1.2: a rendered-frame request round-trips with and without
    // an explicit range, a frame round-trips, and the converters refuse what
    // a hostile peer can vary: mismatched or oversized transfer vectors,
    // non-finite camera or range values, a pixel count that disagrees with
    // the size, and grid dimensions that are not three.
    VolumeRenderRequest volume;
    volume.dataset = DatasetId{5};
    volume.field = FieldId{2};
    volume.maximumLevel = 1;
    volume.composition = CompositionPolicy::ExactLevel;
    volume.region = RealBox{Real3{{0.0, -1.0, 2.0}}, Real3{{1.0, 1.0, 3.0}}};
    volume.camera = {0.7, -0.2, 2.5};
    volume.outputSize = {320, 200};
    volume.range = VolumeRange{0.5, 4.0, true};
    volume.transfer.colors = {0x0000FFU, 0x00FF00U, 0xFF0000U};
    volume.transfer.opacities = {0.0F, 0.5F, 1.0F};
    volume.samplesPerVoxel = 4;
    volume.maximumVoxels = 1 << 20;
    require(codec::fromWire(codec::toWire(volume)) == volume,
        "a volume request with a range did not round-trip");
    // The value on the wire, not just that it survives a round trip. Two
    // transposed mappings are inverses of each other, so every round-trip
    // check in the suite passes while a peer on the other side of a real
    // socket reads the wrong policy -- which is the only situation protocol
    // 1.3 exists for, and the one no round trip can see.
    volume.sampling = SamplingPolicy::Linear;
    require(codec::toWire(volume).sampling == amrexplorer::wire::SamplingPolicy::Linear,
        "Linear does not encode as Linear on the wire");
    volume.sampling = SamplingPolicy::Nearest;
    require(codec::toWire(volume).sampling == amrexplorer::wire::SamplingPolicy::Nearest,
        "Nearest does not encode as Nearest on the wire");
    volume.sampling = SamplingPolicy::PiecewiseConstant;
    require(codec::toWire(volume).sampling == amrexplorer::wire::SamplingPolicy::PiecewiseConstant,
        "PiecewiseConstant does not encode as PiecewiseConstant on the wire");
    volume.sampling = SamplingPolicy::Linear;
    // The two logarithmic flags, against the wire rather than through a
    // round trip. They are both bools and mean opposite cases -- one is the
    // explicit range's mapping, the other the mapping asked for when there is
    // no range -- so a converter that transposed them would round-trip
    // cleanly and show up only against a third-party or mixed-version peer.
    {
        const auto withRange = codec::toWire(volume);
        require(withRange.has_range && withRange.range_logarithmic
                && !withRange.visible_logarithmic,
            "a request's explicit logarithmic range did not set "
            "range_logarithmic alone");
        auto visible = volume;
        visible.range.reset();
        visible.logarithmic = true;
        const auto withoutRange = codec::toWire(visible);
        require(!withoutRange.has_range && withoutRange.visible_logarithmic
                && !withoutRange.range_logarithmic,
            "a request's Visible logarithmic mapping did not set "
            "visible_logarithmic alone");
    }
    volume.range.reset();
    volume.logarithmic = true;
    require(codec::fromWire(codec::toWire(volume)) == volume,
        "a volume request without a range did not round-trip");
    auto volumeWire = codec::toWire(volume);
    volumeWire.transfer_opacities.pop_back();
    requireRejected([&] { static_cast<void>(codec::fromWire(volumeWire)); },
        "mismatched transfer vectors were accepted");
    volumeWire = codec::toWire(volume);
    volumeWire.transfer_colors.assign(maxVolumeTransferEntries + 1, 0U);
    volumeWire.transfer_opacities.assign(maxVolumeTransferEntries + 1, 0.5F);
    requireRejected([&] { static_cast<void>(codec::fromWire(volumeWire)); },
        "an oversized transfer function was accepted");
    volumeWire = codec::toWire(volume);
    volumeWire.zoom = std::numeric_limits<double>::quiet_NaN();
    requireRejected([&] { static_cast<void>(codec::fromWire(volumeWire)); },
        "a NaN zoom was accepted");
    volumeWire = codec::toWire(volume);
    volumeWire.transfer_opacities[1] = std::numeric_limits<float>::infinity();
    requireRejected([&] { static_cast<void>(codec::fromWire(volumeWire)); },
        "an infinite opacity was accepted");
    volumeWire = codec::toWire(volume);
    volumeWire.has_range = true;
    volumeWire.minimum = std::numeric_limits<double>::infinity();
    requireRejected([&] { static_cast<void>(codec::fromWire(volumeWire)); },
        "an infinite range bound was accepted");

    VolumeFrame frame;
    frame.width = 3;
    frame.height = 2;
    frame.pixels = {0xFF000000U, 0x80112233U, 0U, 0xFFFFFFFFU, 0x01020304U, 0x7F7F7F7FU};
    frame.usedRange = {0.5, 4.0, true};
    frame.metrics.gridDims = {8, 4, 2};
    frame.metrics.coveredVoxels = 60;
    frame.metrics.sampledMaximumLevel = 1;
    frame.metrics.gridFromCache = true;
    frame.metrics.sampleMicroseconds = 12;
    frame.metrics.renderMicroseconds = 3;
    frame.metrics.candidateBlocks = 5;
    frame.metrics.blocksRead = 4;
    frame.metrics.cacheHits = 1;
    frame.metrics.payloadBytesRead = 4096;
    frame.cacheFallbackFromLevel = 1;
    frame.cacheFallbackToLevel = 0;
    require(codec::fromWire(codec::toWire(frame, CacheMetrics{})) == frame,
        "a rendered frame did not round-trip");
    // A response that never set the two fallback levels: they default to the
    // no-fallback sentinel, so it decodes as "no fallback" rather than as a
    // fallback from level 0 to level 0 -- which validateSessionVolumeResult
    // refuses as impossible, and that refusal costs the whole connection.
    {
        auto omitted = codec::toWire(frame, CacheMetrics{});
        const codec::fb::RenderedFrameResponseT fresh;
        omitted.cache_fallback_from_level = fresh.cache_fallback_from_level;
        omitted.cache_fallback_to_level = fresh.cache_fallback_to_level;
        const auto unset = codec::fromWire(omitted);
        require(unset.cacheFallbackFromLevel == -1
                && unset.cacheFallbackToLevel == -1,
            "an omitted cache fallback did not decode as no fallback");
    }
    auto frameWire = codec::toWire(frame, CacheMetrics{});
    frameWire.pixels.pop_back();
    requireRejected([&] { static_cast<void>(codec::fromWire(frameWire)); },
        "a frame whose pixels disagree with its size was accepted");
    frameWire = codec::toWire(frame, CacheMetrics{});
    frameWire.grid_dims = {8, 4};
    requireRejected([&] { static_cast<void>(codec::fromWire(frameWire)); },
        "a frame with two grid dimensions was accepted");
    frameWire = codec::toWire(frame, CacheMetrics{});
    frameWire.width = 0;
    requireRejected([&] { static_cast<void>(codec::fromWire(frameWire)); },
        "a zero-width frame was accepted");
    frameWire = codec::toWire(frame, CacheMetrics{});
    frameWire.used_maximum = std::numeric_limits<double>::quiet_NaN();
    requireRejected([&] { static_cast<void>(codec::fromWire(frameWire)); },
        "a NaN used range was accepted");
    return 0;
}
