// Property/fuzz harness for the expression parser. Contract: compile() of
// arbitrary or near-valid text either returns a program or throws
// ExpressionError -- never another exception type (a std::logic_error would
// mean the compiler emitted a program its own stack check rejects), never a
// crash, and never unbounded recursion; and an accepted program evaluates
// without throwing, with the batch path agreeing with the scalar path bit for
// bit. Expression text is typed by hand but also imported from files, which
// is what puts it on this side of the trust boundary. Deterministic and
// bounded so it runs as a normal ctest; configure with
// -DAMREXPLORER_LIBFUZZER=ON (Clang), together with
// AMREXPLORER_ENABLE_SANITIZERS=ON so the fuzzer sees UB/OOB and not only
// hard crashes, to build it instead as a coverage-guided libFuzzer driver.
#include "fuzz_util.hpp"

#include <amrexplorer/expression/Expression.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using amrvis::fuzz::fail;

// How many inputs got past compile() and were actually evaluated. Random bytes
// practically never compile, so without this the whole evaluation half of the
// contract could stop being exercised -- by a mutation set that never produces
// a valid expression, or by a parser change -- and the harness would still
// report success.
long accepted = 0;

bool sameValue(double left, double right)
{
    return left == right || (std::isnan(left) && std::isnan(right));
}

void exerciseExpression(std::string_view source)
{
    amrvis::CompiledExpression compiled = [&source] {
        try {
            return amrvis::CompiledExpression::compile(source);
        } catch (const amrvis::ExpressionError&) {
            // The one rejection the parser promises.
            throw;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "fuzz: unexpected exception: %s\n",
                error.what());
            fail("compile threw something other than ExpressionError");
        } catch (...) {
            fail("compile threw a non-std::exception");
        }
    }();

    const auto symbols = compiled.symbols();
    // Values chosen to reach the domain edges of the functions the grammar
    // offers: a negative (log, sqrt), a zero (division, log), and a large
    // magnitude (exp, pow). More points than the batch path's chunk holds, so
    // the batch/scalar agreement below is checked across a chunk seam and not
    // only within one pass.
    static constexpr std::array<double, 5> pattern{
        1.5, -2.0, 0.0, 1.0e300, -0.25};
    static constexpr std::size_t points = 520;
    std::vector<std::vector<double>> columns(symbols.size());
    for (std::size_t symbol = 0; symbol < symbols.size(); ++symbol) {
        columns[symbol].reserve(points);
        for (std::size_t point = 0; point < points; ++point) {
            columns[symbol].push_back(
                pattern[(symbol + point) % pattern.size()]);
        }
    }
    ++accepted;
    std::vector<std::span<const double>> views;
    views.reserve(columns.size());
    for (const auto& column : columns) {
        views.emplace_back(column);
    }

    try {
        std::vector<double> batch(points, 0.0);
        auto batchEvaluator = compiled.makeEvaluator();
        batchEvaluator.evaluate(views, batch);

        auto scalarEvaluator = compiled.makeEvaluator();
        std::vector<double> point(columns.size(), 0.0);
        for (std::size_t index = 0; index < points; ++index) {
            for (std::size_t symbol = 0; symbol < columns.size(); ++symbol) {
                point[symbol] = columns[symbol][index];
            }
            if (!sameValue(scalarEvaluator.evaluate(point), batch[index])) {
                fail("batch evaluation disagrees with scalar evaluation");
            }
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fuzz: unexpected exception: %s\n", error.what());
        fail("evaluating an accepted expression threw");
    } catch (...) {
        fail("evaluating an accepted expression threw a non-std::exception");
    }
}

// Swallows the promised rejection so the callers below can feed anything.
void exercise(std::string_view source)
{
    try {
        exerciseExpression(source);
    } catch (const amrvis::ExpressionError&) {
    }
}

const std::vector<std::string>& expressionSeeds()
{
    static const std::vector<std::string> seeds{
        "density",
        "sqrt(u**2 + v**2)",
        "pressure/density - 1.0",
        "log10(abs(${x-momentum}))",
        "pow(${Y(H2)}, 2) + exp10(-3)",
        "-2**-3.5e2",
        "abs(x)/(y*z) + exp(log(1.5))",
        "${a}+${b}*(${c}-${d})/2",
    };
    return seeds;
}

} // namespace

#if defined(AMREXPLORER_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    amrvis::fuzz::setCurrentInput(0, data, size);
    exercise(size == 0
            ? std::string_view{}
            : std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
#else
int main()
{
    constexpr int iterations = 40000;
    std::uint64_t rng = 0x51ed'270b'7dea'0b6dULL;
    const auto& seeds = expressionSeeds();
    long iteration = 0;

    // The two ceilings, at and just past their constants: a source that long,
    // and nesting that deep. Random text never reaches either, and each is
    // what stands between an imported file and an overflowed C++ stack.
    for (const std::size_t n : {amrvis::maximumExpressionBytes - 1,
             amrvis::maximumExpressionBytes,
             amrvis::maximumExpressionBytes + 1}) {
        const std::string text(n, '1');
        amrvis::fuzz::setCurrentInput(iteration++, text.data(), text.size());
        exercise(text);
    }
    for (const std::size_t n : {amrvis::maximumExpressionDepth - 1,
             amrvis::maximumExpressionDepth,
             amrvis::maximumExpressionDepth + 1,
             amrvis::maximumExpressionBytes / 2}) {
        for (const std::string& text :
            {std::string(n, '(') + "1" + std::string(n, ')'),
                std::string(n, '-') + "1",
                std::string(n, '(') + "1"}) {
            amrvis::fuzz::setCurrentInput(
                iteration++, text.data(), text.size());
            exercise(text);
        }
    }

    for (int i = 0; i < iterations; ++i) {
        const auto bytes = amrvis::fuzz::randomBytes(rng, 96);
        amrvis::fuzz::setCurrentInput(iteration++, bytes.data(), bytes.size());
        exercise(amrvis::fuzz::viewOf(bytes));
        const auto mutated = amrvis::fuzz::mutateText(
            rng, seeds[amrvis::fuzz::nextRandom(rng) % seeds.size()]);
        amrvis::fuzz::setCurrentInput(
            iteration++, mutated.data(), mutated.size());
        exercise(mutated);
    }
    if (accepted < iterations / 100) {
        fail("too few inputs compiled: the evaluation contract went untested");
    }
    std::printf("fuzz_expression: %d iterations, %ld compiled, no crash\n",
        iterations, accepted);
    return 0;
}
#endif
