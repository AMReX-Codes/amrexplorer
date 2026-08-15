// Property/fuzz harness for the remote wire decoder -- the malicious-peer trust
// boundary. Contract: codec::decode of arbitrary or near-valid bytes yields a
// std::exception and never crashes, reads out of bounds, or lets a
// non-std::exception escape. Run under the sanitizers preset to catch UB/OOB.
// Deterministic and bounded so it runs as a normal ctest; build with
// -DAMREXPLORER_LIBFUZZER for coverage-guided fuzzing.
#include "fuzz_util.hpp"

#include "Codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

void exerciseWire(std::span<const std::uint8_t> bytes)
{
    try {
        auto envelope = amrvis::remote::codec::decode(bytes);
        // A decode that succeeds must be inspectable without surprises.
        static_cast<void>(amrvis::remote::codec::inspect(*envelope));
    } catch (const std::exception&) {
        // Expected rejection path.
    } catch (...) {
        amrvis::fuzz::fail("codec::decode let a non-std::exception escape");
    }
}

std::vector<std::vector<std::uint8_t>> wireSeeds()
{
    namespace codec = amrvis::remote::codec;
    std::vector<std::vector<std::uint8_t>> seeds;
    {
        codec::fb::PingRequestT ping;
        ping.nonce = 7;
        seeds.push_back(codec::encode(1, std::move(ping)));
    }
    {
        codec::fb::HelloRequestT hello;
        hello.client_name = "fuzz";
        hello.maximum_frame_bytes = 1 << 20;
        hello.maximum_minor_version = 1;
        seeds.push_back(codec::encode(2, std::move(hello)));
    }
    {
        codec::fb::ErrorResponseT error;
        error.message = "boom";
        seeds.push_back(codec::encode(3, std::move(error)));
    }
    return seeds;
}

} // namespace

#if defined(AMREXPLORER_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    exerciseWire({data, size});
    return 0;
}
#else
int main()
{
    constexpr int iterations = 60000;
    std::uint64_t rng = 0x1234'5678'9abc'def0ULL;
    const auto seeds = wireSeeds();
    for (int i = 0; i < iterations; ++i) {
        exerciseWire(amrvis::fuzz::randomBytes(rng, 512));
        exerciseWire(amrvis::fuzz::mutate(
            rng, seeds[amrvis::fuzz::nextRandom(rng) % seeds.size()]));
    }
    std::printf("fuzz_wire_codec: %d iterations, no crash\n", iterations);
    return 0;
}
#endif
