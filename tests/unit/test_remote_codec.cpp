#include "Codec.hpp"

#include <cstdlib>
#include <iostream>
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

} // namespace

int main()
{
    using namespace amrvis;
    using namespace amrvis::remote;

    HelloRequestData hello{"codec test", "1", 0, protocolMinor, 4096,
        {FrameCompression::Zstd}};
    auto bytes = codec::encode(7, codec::toWire(hello));
    auto envelope = codec::decode(bytes);
    require(codec::inspect(*envelope).requestId == 7,
        "hello request ID did not round-trip");
    require(codec::fromWire(*envelope->payload.AsHelloRequest()).clientName
            == hello.clientName,
        "hello payload did not round-trip");
    require(codec::fromWire(*envelope->payload.AsHelloRequest())
                .supportedCompressions
            == hello.supportedCompressions,
        "hello compression capabilities did not round-trip");

    OpenedDataset opened;
    opened.id = DatasetId{9};
    opened.catalog.dimension = 3;
    opened.catalog.finestLevel = 0;
    opened.catalog.physicalDomain = RealBox{
        Real3{{0.0, 0.0, 0.0}}, Real3{{1.0, 1.0, 1.0}}};
    LevelMetadata level;
    level.level = 0;
    level.domain = IntBox{
        Int3{{0, 0, 0}}, Int3{{3, 3, 3}}, Int3{{0, 0, 0}}};
    level.boxes.push_back(
        IntBox{Int3{{0, 0, 0}}, Int3{{1, 3, 3}}, Int3{{1, 0, 0}}});
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
            && decoded.gridBoxes.size() == 1,
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
    return 0;
}
