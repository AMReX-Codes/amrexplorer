// Property/fuzz harness for the crafted-plotfile parsing helpers: the FAB/VisMF
// header primitives that treat every byte of a header as hostile. Contract:
// malformed input yields a std::exception and never crashes, reads out of
// bounds, hangs, or lets a non-std::exception escape. Run under the sanitizers
// preset to catch UB/OOB. Deterministic and bounded so it runs as a normal
// ctest; build with -DAMREXPLORER_LIBFUZZER for coverage-guided fuzzing.
#include "fuzz_util.hpp"

#include <amrexplorer/io/detail/FabHeaderParsing.hpp>

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Any type constructible from std::string satisfies the helper's Error param.
struct FuzzError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void exerciseHeaderText(std::string_view text)
{
    namespace detail = amrvis::detail;
    const std::string owned(text);
    try {
        static_cast<void>(detail::parseIntegers(owned));  // never throws
        for (int dimension = 1; dimension <= 3; ++dimension) {
            try {
                static_cast<void>(
                    detail::parseAmrexBox<FuzzError>(owned, dimension));
            } catch (const std::exception&) {
            }
        }
        try {
            int inferred = 0;
            static_cast<void>(detail::parseAmrexBoxInferDimension<FuzzError>(
                owned, inferred));
        } catch (const std::exception&) {
        }
        try {
            static_cast<void>(
                detail::balancedExpressionEnd<FuzzError>(owned, 0));
        } catch (const std::exception&) {
        }
        try {
            static_cast<void>(detail::parseRealDescriptor<FuzzError>(owned));
        } catch (const std::exception&) {
        }
        {
            std::istringstream input(owned);
            std::string line;
            try {
                while (detail::readBoundedLine<FuzzError>(input, line)) {
                }
            } catch (const std::exception&) {
            }
        }
        {
            std::istringstream input(owned);
            try {
                static_cast<void>(detail::readBoundedToken<FuzzError>(
                    input, "fuzz", "token"));
            } catch (const std::exception&) {
            }
        }
        {
            std::istringstream input(owned);
            try {
                static_cast<void>(detail::readBoundedInteger<FuzzError, int>(
                    input, "fuzz", "int"));
            } catch (const std::exception&) {
            }
        }
        {
            std::istringstream input(owned);
            try {
                static_cast<void>(
                    detail::readBoundedInteger<FuzzError, std::uint64_t>(
                        input, "fuzz", "u64"));
            } catch (const std::exception&) {
            }
        }
    } catch (...) {
        amrvis::fuzz::fail("a header helper let a non-std::exception escape");
    }
}

const std::vector<std::string>& headerSeeds()
{
    static const std::vector<std::string> seeds{
        "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))",
        "((8, (32 8 23 0 1 9 0 127)),(8, (1 2 3 4 5 6 7 8)))",
        "((0,0) (7,7) (0,0))",
        "((0,0,0) (3,3,3) (1,0,1))",
        "FAB ((8, (64 11 52 0 1 12 0 1023)),(8, (1 2 3 4 5 6 7 8)))"
        "((0,0) (7,7) (0,0)) 1",
        "1\n1\n1\n0\n(1 0\n((0,0) (3,3) (0,0))\n)\n",
        "2,1\n1.0,\n2.0,\n",
        "-2147483648 2147483647 0 nan inf -inf",
    };
    return seeds;
}

} // namespace

#if defined(AMREXPLORER_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    exerciseHeaderText(
        std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
#else
int main()
{
    constexpr int iterations = 60000;
    std::uint64_t rng = 0x0f1e'2d3c'4b5a'6987ULL;
    const auto& seeds = headerSeeds();
    for (int i = 0; i < iterations; ++i) {
        const auto bytes = amrvis::fuzz::randomBytes(rng, 256);
        exerciseHeaderText(std::string_view(
            reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        exerciseHeaderText(amrvis::fuzz::mutateText(
            rng, seeds[amrvis::fuzz::nextRandom(rng) % seeds.size()]));
    }
    std::printf("fuzz_header_parsing: %d iterations, no crash\n", iterations);
    return 0;
}
#endif
