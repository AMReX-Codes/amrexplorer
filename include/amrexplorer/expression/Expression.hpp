#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace amrvis {

class ExpressionError : public std::invalid_argument {
public:
    ExpressionError(std::string message, std::size_t offset);

    [[nodiscard]] std::size_t offset() const noexcept;

private:
    std::size_t m_offset;
};

namespace expression_detail {
struct Program;
}

class ExpressionEvaluator;

class CompiledExpression {
public:
    [[nodiscard]] static CompiledExpression compile(std::string_view source);

    [[nodiscard]] std::span<const std::string> symbols() const noexcept;
    [[nodiscard]] ExpressionEvaluator makeEvaluator() const;

private:
    explicit CompiledExpression(
        std::shared_ptr<const expression_detail::Program> program);

    std::shared_ptr<const expression_detail::Program> m_program;
};

class ExpressionEvaluator {
public:
    [[nodiscard]] double evaluate(std::span<const double> variables);

private:
    friend class CompiledExpression;

    explicit ExpressionEvaluator(
        std::shared_ptr<const expression_detail::Program> program);

    std::shared_ptr<const expression_detail::Program> m_program;
    std::vector<double> m_stack;
};

} // namespace amrvis
