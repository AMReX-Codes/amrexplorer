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
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
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
    case fb::Payload::ErrorResponse:
        convert(envelope.payload.AsErrorResponse());
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
std::unique_ptr<fb::Real3T> real3(double x, double y, double z)
{
    auto value = std::make_unique<fb::Real3T>();
    value->values = {x, y, z};
    return value;
}

std::unique_ptr<fb::Int3T> int3(int x, int y, int z)
{
    auto value = std::make_unique<fb::Int3T>();
    value->values = {x, y, z};
    return value;
}

std::unique_ptr<fb::RealBoxT> unitBox()
{
    auto box = std::make_unique<fb::RealBoxT>();
    box->lower = real3(0.0, 0.0, 0.0);
    box->upper = real3(1.0, 1.0, 1.0);
    return box;
}

// One seed per fromWire converter arm (plus a trivial ping), each valid enough
// to pass its converter unmutated -- main() checks that -- so mutations reach
// the deep validators (validateResultVectors, finite-value and enum checks)
// with near-valid data instead of dying at a converter's first check.
std::vector<std::vector<std::uint8_t>> wireSeeds()
{
    std::vector<std::vector<std::uint8_t>> seeds;
    std::uint64_t requestId = 0;
    const auto add = [&](auto payload) {
        seeds.push_back(codec::encode(++requestId, std::move(payload)));
    };
    {
        fb::PingRequestT ping;
        ping.nonce = 7;
        add(std::move(ping));
    }
    {
        fb::HelloRequestT hello;
        hello.client_name = "fuzz";
        hello.maximum_frame_bytes = 1 << 20;
        hello.maximum_minor_version = 1;
        add(std::move(hello));
    }
    {
        fb::HelloResponseT hello;
        hello.server_name = "fuzz";
        hello.maximum_frame_bytes = 1 << 20;
        hello.maximum_datasets = 2;
        hello.capabilities = {1, 2};
        add(std::move(hello));
    }
    {
        fb::OpenDatasetRequestT open;
        open.path = "/plt00000";
        open.cache_budget_bytes = 1 << 20;
        add(std::move(open));
    }
    {
        // Two fields on a one-level, two-box catalog: the smallest shape that
        // exercises every count check in fromWire(DatasetOpenedT).
        fb::DatasetOpenedT opened;
        opened.dataset_id = 1;
        opened.dimension = 3;
        opened.finest_level = 0;
        opened.time = 0.5;
        opened.physical_domain = unitBox();
        for (const char* name : {"density", "pressure"}) {
            auto field = std::make_unique<fb::FieldCatalogT>();
            field->name = name;
            opened.fields.push_back(std::move(field));
        }
        auto level = std::make_unique<fb::LevelCatalogT>();
        auto domain = std::make_unique<fb::IntBoxT>();
        domain->lower = int3(0, 0, 0);
        domain->upper = int3(3, 3, 3);
        domain->centering = int3(0, 0, 0);
        level->domain = std::move(domain);
        level->cell_size = real3(0.25, 0.25, 0.25);
        level->index_origin = real3(0.0, 0.0, 0.0);
        for (int lo : {0, 2}) {
            auto box = std::make_unique<fb::IntBoxT>();
            box->lower = int3(lo, 0, 0);
            box->upper = int3(lo + 1, 3, 3);
            box->centering = int3(0, 0, 0);
            level->boxes.push_back(std::move(box));
        }
        opened.levels.push_back(std::move(level));
        auto species = std::make_unique<fb::ParticleSpeciesCatalogT>();
        species->name = "electrons";
        species->dimension = 3;
        opened.particle_species.push_back(std::move(species));
        opened.file_range_available = {1, 1};
        opened.level_range_available = {1, 0};
        opened.metadata_metrics = std::make_unique<fb::MetadataReadMetricsT>();
        opened.cache = std::make_unique<fb::CacheStateT>();
        add(std::move(opened));
    }
    {
        fb::SliceViewRequestT request;
        request.dataset_id = 1;
        request.field = 0;
        request.visible_region = unitBox();
        request.width = 4;
        request.height = 4;
        add(std::move(request));
    }
    {
        fb::SliceViewResponseT slice;
        slice.width = 2;
        slice.height = 2;
        slice.physical_region = unitBox();
        slice.values = {1.0F, 2.0F, 3.0F, 4.0F};
        slice.valid = {1, 1, 1, 1};
        slice.source_level = {0, 0, 0, 0};
        auto gridBox = std::make_unique<fb::SliceGridBoxT>();
        gridBox->physical_region = unitBox();
        slice.grid_boxes.push_back(std::move(gridBox));
        slice.cache = std::make_unique<fb::CacheStateT>();
        add(std::move(slice));
    }
    {
        fb::LineViewRequestT request;
        request.dataset_id = 1;
        request.axis = 0;
        request.fixed_coordinates = real3(0.5, 0.5, 0.5);
        request.region = unitBox();
        request.has_region = true;
        request.output_width = 8;
        add(std::move(request));
    }
    {
        fb::LineViewResponseT line;
        line.positions = {0.5, 1.5};
        line.values = {1.0, 2.0};
        line.valid = {1, 1};
        line.source_level = {0, 0};
        line.cache = std::make_unique<fb::CacheStateT>();
        add(std::move(line));
    }
    {
        fb::DatasetPageRequestT request;
        request.dataset_id = 1;
        request.region = unitBox();
        request.slice_position = 0.5;
        request.maximum_extent = 8;
        add(std::move(request));
    }
    {
        fb::DatasetPageResponseT page;
        page.lower = {0, 0};
        page.upper = {1, 1};
        page.nx = 2;
        page.ny = 2;
        page.values = {1.0F, 2.0F, 3.0F, 4.0F};
        page.covered = {1, 1, 1, 1};
        page.minimum = 1.0;
        page.maximum = 4.0;
        page.has_finite_values = true;
        page.cache = std::make_unique<fb::CacheStateT>();
        add(std::move(page));
    }
    {
        fb::ParticleSampleRequestT request;
        request.dataset_id = 1;
        request.species = "electrons";
        request.fraction = 0.5;
        request.seed = 3;
        add(std::move(request));
    }
    {
        fb::ParticleSampleResponseT particles;
        particles.species = std::make_unique<fb::ParticleSpeciesCatalogT>();
        particles.species->name = "electrons";
        particles.species->dimension = 3;
        particles.ids = {11, 12};
        particles.positions = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
        particles.cache = std::make_unique<fb::CacheStateT>();
        add(std::move(particles));
    }
    {
        fb::RangeRequestT request;
        request.dataset_id = 1;
        request.field = 0;
        request.scope = fb::RangeScope::Level;
        add(std::move(request));
    }
    {
        fb::RangeResponseT range;
        range.has_range = true;
        range.minimum = -1.0;
        range.maximum = 1.0;
        range.cache = std::make_unique<fb::CacheStateT>();
        add(std::move(range));
    }
    {
        fb::ErrorResponseT error;
        error.code = fb::ErrorCode::InvalidRequest;
        error.message = "boom";
        add(std::move(error));
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
    // A seed its own converter rejects would only ever exercise that first
    // check, so require every seed to decode and convert unmutated.
    for (const auto& seed : seeds) {
        amrvis::fuzz::setCurrentInput(iteration++, seed.data(), seed.size());
        try {
            exerciseFromWire(*codec::decode(seed));
        } catch (const std::exception&) {
            amrvis::fuzz::fail("a wire seed is rejected before mutation");
        }
    }
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
