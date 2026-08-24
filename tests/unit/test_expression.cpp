#include <amrexplorer/expression/Expression.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool close(double left, double right)
{
    const auto scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= 1.0e-12 * scale;
}

double evaluate(
    std::string_view source, std::span<const double> variables = {})
{
    auto compiled = amrvis::CompiledExpression::compile(source);
    auto evaluator = compiled.makeEvaluator();
    return evaluator.evaluate(variables);
}

void requireError(
    std::string_view source, std::size_t offset, std::string_view message)
{
    try {
        [[maybe_unused]] const auto compiled =
            amrvis::CompiledExpression::compile(source);
    } catch (const amrvis::ExpressionError& error) {
        require(error.offset() == offset,
            "wrong error offset for '" + std::string(source) + "': "
                + std::to_string(error.offset()));
        require(std::string_view(error.what()).find(message)
                != std::string_view::npos,
            "wrong error message for '" + std::string(source) + "': "
                + error.what());
        return;
    }
    throw std::runtime_error(
        "expression was unexpectedly accepted: " + std::string(source));
}

// A rejection whose offset is not part of the contract: the depth and length
// limits are about what the parser refuses, not where.
void requireErrorMessage(std::string_view source, std::string_view message)
{
    try {
        [[maybe_unused]] const auto compiled =
            amrvis::CompiledExpression::compile(source);
    } catch (const amrvis::ExpressionError& error) {
        require(std::string_view(error.what()).find(message)
                != std::string_view::npos,
            std::string("wrong error message: ") + error.what());
        return;
    }
    throw std::runtime_error("expression was unexpectedly accepted");
}

// The batch evaluator must agree with the scalar one point for point; this is
// the only thing that makes the chunked stack slots interchangeable with the
// scalar stack.
void requireBatchMatchesScalar(
    std::string_view source, const std::vector<std::vector<double>>& columns)
{
    const auto compiled = amrvis::CompiledExpression::compile(source);
    require(compiled.symbols().size() == columns.size(),
        "test supplied the wrong column count for '" + std::string(source)
            + "'");
    const auto points = columns.empty() ? std::size_t{0} : columns[0].size();

    std::vector<std::span<const double>> views;
    views.reserve(columns.size());
    for (const auto& column : columns) {
        require(column.size() == points, "test columns differ in length");
        views.emplace_back(column);
    }
    std::vector<double> batch(points, 0.0);
    auto batchEvaluator = compiled.makeEvaluator();
    batchEvaluator.evaluate(views, batch);

    auto scalarEvaluator = compiled.makeEvaluator();
    std::vector<double> point(columns.size(), 0.0);
    for (std::size_t index = 0; index < points; ++index) {
        for (std::size_t column = 0; column < columns.size(); ++column) {
            point[column] = columns[column][index];
        }
        const auto expected = scalarEvaluator.evaluate(point);
        const auto actual = batch[index];
        const auto agree = close(expected, actual)
            || (std::isnan(expected) && std::isnan(actual))
            || expected == actual;
        require(agree,
            "batch and scalar evaluation differ for '" + std::string(source)
                + "' at point " + std::to_string(index));
    }
}

std::vector<double> ramp(std::size_t count, double first, double step)
{
    std::vector<double> values;
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        values.push_back(first + step * static_cast<double>(index));
    }
    return values;
}

} // namespace

int main()
{
    try {
        require(close(evaluate("1"), 1.0), "integer literal failed");
        require(close(evaluate("1."), 1.0), "trailing decimal failed");
        require(close(evaluate(".5"), 0.5), "leading decimal failed");
        require(close(evaluate("2.5E-3"), 0.0025), "exponent literal failed");
        require(close(evaluate("0e-9999"), 0.0),
            "zero with extreme exponent failed");
        require(close(evaluate(" 1 + 2 * 3 \t"), 7.0), "precedence failed");
        require(close(evaluate("8 / 2 - 1"), 3.0),
            "division or subtraction failed");
        require(close(evaluate("(1 + 2) * 3"), 9.0), "parentheses failed");
        require(close(evaluate("+3 + -2"), 1.0), "unary signs failed");
        require(close(evaluate("2**3**2"), 512.0),
            "power is not right-associative");
        require(close(evaluate("2**-2"), 0.25), "signed exponent failed");
        require(close(evaluate("-2**2"), -4.0),
            "power did not bind more tightly than unary minus");
        require(close(evaluate("2**3"), evaluate("pow(2,3)")),
            "power syntaxes differ");

        require(close(evaluate("abs(2.0)"), 2.0),
            "abs changed a positive value");
        require(close(evaluate("abs(-2.0)"), 2.0),
            "abs failed for a negative value");
        require(close(evaluate("sqrt(9)"), 3.0), "sqrt failed");
        require(close(evaluate("exp(1)"), std::exp(1.0)), "exp failed");
        require(close(evaluate("log(exp(2))"), 2.0), "log failed");
        require(close(evaluate("exp10(3)"), 1000.0), "exp10 failed");
        require(close(evaluate("log10(1000)"), 3.0), "log10 failed");

        const auto absolute =
            amrvis::CompiledExpression::compile("abs(x)");
        auto absoluteEvaluator = absolute.makeEvaluator();
        const std::array negativeValue{-4.5};
        require(close(absoluteEvaluator.evaluate(negativeValue), 4.5),
            "abs of a variable failed");

        const auto expression =
            amrvis::CompiledExpression::compile("z + x*z + log");
        const auto symbols = expression.symbols();
        require(symbols.size() == 3 && symbols[0] == "z"
                && symbols[1] == "x" && symbols[2] == "log",
            "symbols are not ordered by first appearance");
        auto evaluator = expression.makeEvaluator();
        const std::array variables{2.0, 3.0, 4.0};
        require(close(evaluator.evaluate(variables), 12.0),
            "variable evaluation failed");

        // ${...} names a symbol a bare identifier cannot spell, verbatim, and
        // is never read as a function call.
        {
            const auto braced = amrvis::CompiledExpression::compile(
                "sqrt(${x-momentum}**2 + ${Y(H2)}**2)");
            const auto names = braced.symbols();
            require(names.size() == 2 && names[0] == "x-momentum"
                    && names[1] == "Y(H2)",
                "braced symbols were not taken verbatim");
            auto bracedEvaluator = braced.makeEvaluator();
            const std::array components{3.0, 4.0};
            require(close(bracedEvaluator.evaluate(components), 5.0),
                "braced symbol evaluation failed");
        }
        {
            // The same name braced and bare is the same symbol, and a name
            // with a space or a leading digit is reachable only braced.
            const auto mixed = amrvis::CompiledExpression::compile(
                "density + ${density} + ${2nd moment} + ${ x }");
            const auto names = mixed.symbols();
            require(names.size() == 3 && names[0] == "density"
                    && names[1] == "2nd moment" && names[2] == " x ",
                "braced and bare spellings did not share one symbol");
        }

        // Non-finite results are values, not errors: the display layer already
        // treats them as invalid samples.
        require(std::isnan(evaluate("log(0-1)")), "log of a negative was not NaN");
        require(std::isinf(evaluate("1/(1-1)")), "division by zero was not inf");
        require(std::isnan(evaluate("(1-1)/(1-1)")), "0/0 was not NaN");

        bool wrongCountRejected = false;
        try {
            [[maybe_unused]] const auto ignored =
                evaluator.evaluate(std::span<const double>{});
        } catch (const std::invalid_argument&) {
            wrongCountRejected = true;
        }
        require(wrongCountRejected, "wrong variable count was accepted");

        // The batch path: more points than one chunk holds, so the chunk seam
        // is exercised, plus a constant-only and a single-symbol program.
        requireBatchMatchesScalar("2*a + b/a - sqrt(abs(b))",
            {ramp(1300, -3.0, 0.5), ramp(1300, 7.0, -0.25)});
        requireBatchMatchesScalar("log10(${x-momentum}) + 3**2",
            {ramp(700, -2.0, 1.0)});
        requireBatchMatchesScalar("density", {ramp(5, 1.0, 1.0)});
        {
            const auto constant = amrvis::CompiledExpression::compile("1+2");
            auto constantEvaluator = constant.makeEvaluator();
            std::vector<double> out(3, 0.0);
            constantEvaluator.evaluate({}, out);
            require(close(out[0], 3.0) && close(out[2], 3.0),
                "constant batch evaluation failed");
            // An empty output is a no-op, not an error.
            constantEvaluator.evaluate({}, std::span<double>{});
        }
        {
            const auto program = amrvis::CompiledExpression::compile("a+b");
            auto batchEvaluator = program.makeEvaluator();
            const std::array<double, 2> first{1.0, 2.0};
            const std::array<double, 3> second{1.0, 2.0, 3.0};
            std::vector<double> out(3, 0.0);
            bool raggedRejected = false;
            try {
                const std::array<std::span<const double>, 2> columns{
                    std::span<const double>{first},
                    std::span<const double>{second}};
                batchEvaluator.evaluate(columns, out);
            } catch (const std::invalid_argument&) {
                raggedRejected = true;
            }
            require(raggedRejected, "ragged batch columns were accepted");
        }

        std::vector<std::future<double>> results;
        for (int index = 0; index < 16; ++index) {
            results.push_back(std::async(
                std::launch::async, [&expression, index] {
                    auto local = expression.makeEvaluator();
                    const std::array values{
                        static_cast<double>(index), 2.0, 1.0};
                    return local.evaluate(values);
                }));
        }
        for (int index = 0; index < 16; ++index) {
            require(close(results[static_cast<std::size_t>(index)].get(),
                        3.0 * static_cast<double>(index) + 1.0),
                "concurrent evaluation failed");
        }

        requireError("", 0, "expected expression");
        requireError("1\n+2", 1, "newlines are not allowed");
        requireError("1\r+2", 1, "newlines are not allowed");
        requireError("2^3", 1, "unexpected token");
        requireError("x=1", 1, "unexpected token");
        requireError("1;2", 1, "unexpected token");
        requireError("sin(1)", 0, "unknown function");
        requireError("pow(1)", 5, "expected ','");
        requireError("pow(1,2,3)", 7, "expected ')'");
        requireError("2(3)", 1, "unexpected token");
        requireError("1e+", 3, "invalid numeric exponent");
        requireError("1e9999", 0, "out of range");
        requireError("1e-9999", 0, "out of range");
        requireError(".1e-9999", 0, "out of range");
        requireError("$density", 0, "expected '{'");
        requireError("1 + ${", 4, "unterminated");
        requireError("1 + ${density", 4, "unterminated");
        requireError("${}", 0, "names no field");
        requireError("${a\nb}", 3, "newlines are not allowed");
        requireError("${a}${b}", 4, "unexpected token");

        // Deep nesting is bounded rather than left to the C++ stack: both the
        // parenthesis descent and a sign chain.
        {
            const auto depth = amrvis::maximumExpressionDepth;
            requireErrorMessage(
                std::string(depth, '(') + "1" + std::string(depth, ')'),
                "nests too deeply");
            requireErrorMessage(
                std::string(depth, '-') + "1", "nests too deeply");
            // One parenthesis short of the limit still compiles, so the limit
            // bounds the recursion rather than the whole grammar.
            const auto allowed = std::string(depth - 1, '(') + "2"
                + std::string(depth - 1, ')');
            require(close(evaluate(allowed), 2.0),
                "an expression within the depth limit was rejected");
            require(close(evaluate(std::string(depth - 1, '-') + "1"),
                        (depth - 1) % 2 == 0 ? 1.0 : -1.0),
                "a sign chain within the depth limit was rejected");
        }
        {
            const auto tooLong =
                std::string(amrvis::maximumExpressionBytes + 1, '1');
            requireError(tooLong, amrvis::maximumExpressionBytes, "exceeds");
        }

        std::cout << "expression parser tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "test_expression failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
