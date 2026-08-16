// Property/fuzz harness for the remote wire trust boundary. Contract:
// codec::decode of arbitrary or near-valid bytes, inspect of the decoded
// envelope, and every payload-matching fromWire converter (where the hostile
// payload *contents* are validated -- vector length agreement, finite values,
// enum ranges) yield a std::exception on rejection and never crash, read out
// of bounds, or let a non-std::exception escape; an accepted envelope's
// inspect() must agree with the envelope it summarizes. Run under the
// qt-sanitizers preset to catch UB/OOB (the plain sanitizers preset builds
// without remote support, so this target does not exist there). Deterministic
// and bounded so it runs as a normal ctest; configure with
// -DAMREXPLORER_LIBFUZZER=ON (Clang) to build it instead as a coverage-guided
// libFuzzer driver.
#include "fuzz_util.hpp"

#include "Codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

namespace codec = amrvis::remote::codec;
namespace fb = codec::fb;

// fromWire on a payload pointer, mirroring the server: a null pointer (the
// union tag disagreeing with the stored table on a crafted buffer) is the
// server's "payload is missing" rejection, not a dereference.
template <typename Payload>
void convert(const Payload* payload)
{
    if (payload != nullptr) {
        static_cast<void>(codec::fromWire(*payload));
    }
}

// Route a decoded envelope's payload through the matching fromWire converter
// -- the layer where the server/client actually validate hostile payload
// contents (validateResultVectors, finite-value and enum checks). Only arms
// with a nontrivial converter are dispatched; a converter throwing
// std::exception is the expected rejection.
void exerciseFromWire(const codec::NativeEnvelope& envelope)
{
    switch (envelope.payload.type) {
    case fb::Payload::HelloRequest:
        convert(envelope.payload.AsHelloRequest());
        break;
    case fb::Payload::HelloResponse:
        convert(envelope.payload.AsHelloResponse());
        break;
    case fb::Payload::OpenDatasetRequest:
        convert(envelope.payload.AsOpenDatasetRequest());
        break;
    case fb::Payload::DatasetOpened:
        convert(envelope.payload.AsDatasetOpened());
        break;
    case fb::Payload::SliceViewRequest:
        convert(envelope.payload.AsSliceViewRequest());
        break;
    case fb::Payload::SliceViewResponse:
        convert(envelope.payload.AsSliceViewResponse());
        break;
    case fb::Payload::LineViewRequest:
        convert(envelope.payload.AsLineViewRequest());
        break;
    case fb::Payload::LineViewResponse:
        convert(envelope.payload.AsLineViewResponse());
        break;
    case fb::Payload::DatasetPageRequest:
        convert(envelope.payload.AsDatasetPageRequest());
        break;
    case fb::Payload::DatasetPageResponse:
        convert(envelope.payload.AsDatasetPageResponse());
        break;
    case fb::Payload::ParticleSampleRequest:
        convert(envelope.payload.AsParticleSampleRequest());
        break;
    case fb::Payload::ParticleSampleResponse:
        convert(envelope.payload.AsParticleSampleResponse());
        break;
    case fb::Payload::RangeRequest:
        convert(envelope.payload.AsRangeRequest());
        break;
    case fb::Payload::RangeResponse:
        convert(envelope.payload.AsRangeResponse());
        break;
    default:
        break;  // trivial payloads (ping/pong/cancel/...) have no converter
    }
}

void exerciseWire(std::span<const std::uint8_t> bytes)
{
    try {
        auto envelope = codec::decode(bytes);
        // Postcondition: a decode that succeeds must be inspectable, and the
        // summary must agree with the envelope it summarizes.
        const auto info = codec::inspect(*envelope);
        if (info.requestId != envelope->request_id
            || info.protocolMajor != envelope->protocol_major
            || info.protocolMinorVersion != envelope->protocol_minor_version) {
            amrvis::fuzz::fail("codec::inspect disagrees with its envelope");
        }
        exerciseFromWire(*envelope);
    } catch (const std::exception&) {
        // Expected rejection path (the codec's documented contract).
    } catch (...) {
        amrvis::fuzz::fail("the wire codec let a non-std::exception escape");
    }
}

#if !defined(AMREXPLORER_LIBFUZZER)
// Seeds across the payload arms, including the vector-bearing responses whose
// fromWire converters hold the length/content validation -- mutations of these
// are what actually reach validateResultVectors with near-valid data.
std::vector<std::vector<std::uint8_t>> wireSeeds()
{
    std::vector<std::vector<std::uint8_t>> seeds;
    {
        fb::PingRequestT ping;
        ping.nonce = 7;
        seeds.push_back(codec::encode(1, std::move(ping)));
    }
    {
        fb::HelloRequestT hello;
        hello.client_name = "fuzz";
        hello.maximum_frame_bytes = 1 << 20;
        hello.maximum_minor_version = 1;
        seeds.push_back(codec::encode(2, std::move(hello)));
    }
    {
        fb::ErrorResponseT error;
        error.message = "boom";
        seeds.push_back(codec::encode(3, std::move(error)));
    }
    {
        fb::SliceViewResponseT slice;
        slice.width = 2;
        slice.height = 2;
        slice.physical_region = std::make_unique<fb::RealBoxT>();
        slice.values = {1.0F, 2.0F, 3.0F, 4.0F};
        slice.valid = {1, 1, 1, 1};
        slice.source_level = {0, 0, 0, 0};
        seeds.push_back(codec::encode(4, std::move(slice)));
    }
    {
        fb::LineViewResponseT line;
        line.positions = {0.5, 1.5};
        line.values = {1.0, 2.0};
        line.valid = {1, 1};
        line.source_level = {0, 0};
        seeds.push_back(codec::encode(5, std::move(line)));
    }
    {
        fb::ParticleSampleResponseT particles;
        particles.ids = {11, 12};
        particles.positions = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
        seeds.push_back(codec::encode(6, std::move(particles)));
    }
    {
        fb::SliceViewRequestT request;
        request.dataset_id = 1;
        request.field = 0;
        request.visible_region = std::make_unique<fb::RealBoxT>();
        request.width = 4;
        request.height = 4;
        seeds.push_back(codec::encode(7, std::move(request)));
    }
    return seeds;
}
#endif

} // namespace

#if defined(AMREXPLORER_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    amrvis::fuzz::setCurrentInput(0, data, size);
    exerciseWire({data, size});
    return 0;
}
#else
int main()
{
    constexpr int iterations = 60000;
    std::uint64_t rng = 0x1234'5678'9abc'def0ULL;
    const auto seeds = wireSeeds();
    long iteration = 0;
    for (int i = 0; i < iterations; ++i) {
        // Purely random bytes can never carry the AVR2 file identifier, so
        // decode would reject every one at its first check and the verifier
        // would sit idle. Stamp the identifier at its flatbuffer offset
        // (bytes 4..8) on half the buffers so random inputs reach the
        // verifier and the envelope checks too.
        auto bytes = amrvis::fuzz::randomBytes(rng, 512);
        if (bytes.size() >= 8 && (i % 2) == 0) {
            std::memcpy(bytes.data() + 4, "AVR2", 4);
        }
        amrvis::fuzz::setCurrentInput(iteration++, bytes.data(), bytes.size());
        exerciseWire(bytes);
        const auto mutated = amrvis::fuzz::mutate(
            rng, seeds[amrvis::fuzz::nextRandom(rng) % seeds.size()]);
        amrvis::fuzz::setCurrentInput(
            iteration++, mutated.data(), mutated.size());
        exerciseWire(mutated);
    }
    std::printf("fuzz_wire_codec: %d iterations, no crash\n", iterations);
    return 0;
}
#endif
