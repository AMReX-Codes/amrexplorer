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
            "wrong error offset for '" + std::string(source) + "'");
        require(std::string_view(error.what()).find(message)
                != std::string_view::npos,
            "wrong error message for '" + std::string(source) + "': "
                + error.what());
        return;
    }
    throw std::runtime_error(
        "expression was unexpectedly accepted: " + std::string(source));
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

        bool wrongCountRejected = false;
        try {
            [[maybe_unused]] const auto ignored =
                evaluator.evaluate(std::span<const double>{});
        } catch (const std::invalid_argument&) {
            wrongCountRejected = true;
        }
        require(wrongCountRejected, "wrong variable count was accepted");

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

        std::cout << "expression parser tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "test_expression failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
