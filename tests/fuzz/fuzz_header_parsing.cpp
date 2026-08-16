// Property/fuzz harness for the crafted-plotfile parsing helpers: the FAB/VisMF
// header primitives that treat every byte of a header as hostile. Contract:
// malformed input is rejected with exactly the caller-supplied Error type --
// never any other exception, a crash, an out-of-bounds read, or a hang -- and
// accepted input satisfies the helper's postconditions (a parsed box is valid,
// an inferred dimension is 1..3). Run under the sanitizers preset to catch
// UB/OOB. Deterministic and bounded so it runs as a normal ctest; configure
// with -DAMREXPLORER_LIBFUZZER=ON (Clang) to build it instead as a
// coverage-guided libFuzzer driver.
#include "fuzz_util.hpp"

#include <amrexplorer/io/detail/FabHeaderParsing.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Any type constructible from std::string satisfies the helper's Error param.
// Deliberately NOT derived from the std:: hierarchy's common leaf types the
// helpers might throw by accident: catching FuzzError (and only FuzzError) as
// the expected path is what pins the "failures arrive as the caller's Error
// type" contract -- real call sites catch narrowly, so a leaked
// std::out_of_range would crash the reader while a loose harness catch
// (const std::exception&) would record it as a clean rejection.
struct FuzzError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void exerciseHeaderText(std::string_view text)
{
    namespace detail = amrvis::detail;
    const std::string owned(text);
    try {
        const auto integers = detail::parseIntegers(owned);  // never throws
        for (int dimension = 1; dimension <= 3; ++dimension) {
            try {
                const auto box
                    = detail::parseAmrexBox<FuzzError>(owned, dimension);
                // Postcondition: parse is extraction, not validation (an
                // inverted box is rejected downstream by pointCount) -- but it
                // must extract faithfully: the box holds exactly the integers
                // parseIntegers saw, in lower/upper/centering order.
                for (int axis = 0; axis < dimension; ++axis) {
                    const auto i = static_cast<std::size_t>(axis);
                    const auto d = static_cast<std::size_t>(dimension);
                    if (box.lower[i] != integers[i]
                        || box.upper[i] != integers[d + i]
                        || box.centering[i] != integers[2 * d + i]) {
                        amrvis::fuzz::fail(
                            "parseAmrexBox transposed the parsed integers");
                    }
                }
                // A grown box either throws the Error type or stays exact.
                try {
                    const auto grown = detail::grownBox<FuzzError>(
                        box, {1, 1, 1}, dimension);
                    for (int axis = 0; axis < dimension; ++axis) {
                        const auto i = static_cast<std::size_t>(axis);
                        // int64 arithmetic: the harness must not overflow in
                        // the very comparison that checks for a wrap.
                        if (static_cast<std::int64_t>(grown.lower[i])
                                != static_cast<std::int64_t>(box.lower[i]) - 1
                            || static_cast<std::int64_t>(grown.upper[i])
                                != static_cast<std::int64_t>(box.upper[i]) + 1) {
                            amrvis::fuzz::fail(
                                "grownBox produced a wrong or wrapped box");
                        }
                    }
                } catch (const FuzzError&) {
                }
            } catch (const FuzzError&) {
            }
        }
        try {
            int inferred = 0;
            static_cast<void>(detail::parseAmrexBoxInferDimension<FuzzError>(
                owned, inferred));
            // Postcondition: the inferred dimension is a real one.
            if (inferred < 1 || inferred > 3) {
                amrvis::fuzz::fail(
                    "parseAmrexBoxInferDimension accepted a bad dimension");
            }
        } catch (const FuzzError&) {
        }
        try {
            const auto end
                = detail::balancedExpressionEnd<FuzzError>(owned, 0);
            // Postcondition: one past the matching ')', so in (0, size()].
            if (end == 0 || end > owned.size()) {
                amrvis::fuzz::fail(
                    "balancedExpressionEnd returned an out-of-range index");
            }
        } catch (const FuzzError&) {
        }
        try {
            static_cast<void>(detail::parseRealDescriptor<FuzzError>(owned));
        } catch (const FuzzError&) {
        }
        {
            std::istringstream input(owned);
            std::string line;
            try {
                while (detail::readBoundedLine<FuzzError>(input, line)) {
                    // Postcondition: a delivered line respects the ceiling.
                    if (line.size() > detail::maximumHeaderLineBytes) {
                        amrvis::fuzz::fail(
                            "readBoundedLine delivered an over-limit line");
                    }
                }
            } catch (const FuzzError&) {
            }
        }
        {
            std::istringstream input(owned);
            try {
                const auto token = detail::readBoundedToken<FuzzError>(
                    input, "fuzz", "token");
                if (token.size() > detail::maximumHeaderTokenBytes) {
                    amrvis::fuzz::fail(
                        "readBoundedToken delivered an over-limit token");
                }
            } catch (const FuzzError&) {
            }
        }
        {
            std::istringstream input(owned);
            try {
                static_cast<void>(detail::readBoundedInteger<FuzzError, int>(
                    input, "fuzz", "int"));
            } catch (const FuzzError&) {
            }
        }
        {
            std::istringstream input(owned);
            try {
                static_cast<void>(
                    detail::readBoundedInteger<FuzzError, std::uint64_t>(
                        input, "fuzz", "u64"));
            } catch (const FuzzError&) {
            }
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fuzz: escaping exception: %s\n", error.what());
        amrvis::fuzz::fail(
            "a header helper threw something other than the Error type");
    } catch (...) {
        amrvis::fuzz::fail("a header helper let a non-std::exception escape");
    }
}

#if !defined(AMREXPLORER_LIBFUZZER)
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
#endif

} // namespace

#if defined(AMREXPLORER_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    amrvis::fuzz::setCurrentInput(0, data, size);
    exerciseHeaderText(size == 0
        ? std::string_view{}
        : std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
#else
int main()
{
    constexpr int iterations = 60000;
    std::uint64_t rng = 0x0f1e'2d3c'4b5a'6987ULL;
    const auto& seeds = headerSeeds();
    // Boundary inputs the 256-byte random cases never reach: a single
    // token/line at and just past the reader ceilings (maximumHeaderTokenBytes,
    // maximumHeaderLineBytes), so the most hostile length limits are exercised
    // deterministically and keep tracking the constants.
    namespace detail = amrvis::detail;
    long iteration = 0;
    for (const std::size_t n : {detail::maximumHeaderTokenBytes - 1,
             detail::maximumHeaderTokenBytes, detail::maximumHeaderTokenBytes + 1,
             detail::maximumHeaderLineBytes - 1, detail::maximumHeaderLineBytes,
             detail::maximumHeaderLineBytes + 1}) {
        const std::string text(n, 'x');
        amrvis::fuzz::setCurrentInput(iteration++, text.data(), text.size());
        exerciseHeaderText(text);
    }
    for (int i = 0; i < iterations; ++i) {
        const auto bytes = amrvis::fuzz::randomBytes(rng, 256);
        amrvis::fuzz::setCurrentInput(iteration++, bytes.data(), bytes.size());
        exerciseHeaderText(amrvis::fuzz::viewOf(bytes));
        const auto mutated = amrvis::fuzz::mutateText(
            rng, seeds[amrvis::fuzz::nextRandom(rng) % seeds.size()]);
        amrvis::fuzz::setCurrentInput(
            iteration++, mutated.data(), mutated.size());
        exerciseHeaderText(mutated);
    }
    std::printf("fuzz_header_parsing: %d iterations, no crash\n", iterations);
    return 0;
}
#endif
