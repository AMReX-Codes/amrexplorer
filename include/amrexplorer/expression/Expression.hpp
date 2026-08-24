#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Single-line algebraic expressions over named symbols, compiled once into a
// flat instruction list and evaluated by a small stack machine. The grammar:
//
//   expression      := additive
//   additive        := multiplicative (("+" | "-") multiplicative)*
//   multiplicative  := unary (("*" | "/") unary)*
//   unary           := ("+" | "-") unary | power
//   power           := primary ("**" unary)?
//   primary         := number | symbol | "(" expression ")"
//                    | unary-function "(" expression ")"
//                    | "pow" "(" expression "," expression ")"
//   unary-function  := "abs" | "sqrt" | "exp" | "log" | "exp10" | "log10"
//   symbol          := identifier | "${" any-but-brace-or-newline+ "}"
//
// A symbol is a bare identifier ([A-Za-z_][A-Za-z0-9_.]*) or, for a name a
// bare identifier cannot spell -- "x-momentum", "Y(H2)" -- the same name
// inside ${...}, taken verbatim. Nothing here knows what a symbol *means*;
// symbols() reports them in first-appearance order and the caller supplies a
// value per symbol in that order.

namespace amrvis {

class ExpressionError : public std::invalid_argument {
public:
    ExpressionError(std::string message, std::size_t offset);

    // Byte offset into the compiled source where the problem was found, so an
    // editor can point at the character rather than repeat the message.
    [[nodiscard]] std::size_t offset() const noexcept;

private:
    std::size_t m_offset;
};

// Bounds compile() enforces before it recurses. Expressions are typed by hand
// but also arrive from files, so neither the length nor the nesting depth is
// left to the C++ stack to discover: "((((..." nested deep enough would
// otherwise overflow it.
inline constexpr std::size_t maximumExpressionBytes = 4096;
inline constexpr std::size_t maximumExpressionDepth = 64;

namespace expression_detail {
struct Program;
}

class ExpressionEvaluator;

// A compiled expression: immutable, cheap to copy, and safe to share between
// threads. Evaluation state lives in the evaluators it hands out, so each
// thread makes its own.
class CompiledExpression {
public:
    [[nodiscard]] static CompiledExpression compile(std::string_view source);

    // The symbols the expression reads, in first-appearance order. This is the
    // order every evaluate() call expects its values in.
    [[nodiscard]] std::span<const std::string> symbols() const noexcept;
    [[nodiscard]] ExpressionEvaluator makeEvaluator() const;

private:
    explicit CompiledExpression(
        std::shared_ptr<const expression_detail::Program> program);

    std::shared_ptr<const expression_detail::Program> m_program;
};

class ExpressionEvaluator {
public:
    // One point: `variables` holds one value per symbol().
    [[nodiscard]] double evaluate(std::span<const double> variables);

    // Many points at once: one column per symbol, every column and `out` the
    // same length. Evaluated in fixed-size chunks -- one pass per instruction
    // over a chunk rather than the whole program per point -- so the stack
    // slots stay in cache and the per-instruction loops vectorise, without
    // the whole-block temporaries a single pass over all points would need.
    void evaluate(
        std::span<const std::span<const double>> columns, std::span<double> out);

private:
    friend class CompiledExpression;

    explicit ExpressionEvaluator(
        std::shared_ptr<const expression_detail::Program> program);

    std::shared_ptr<const expression_detail::Program> m_program;
    std::vector<double> m_stack;
    // Chunked stack slots for the batch path, allocated on first use so the
    // scalar path pays nothing for them.
    std::vector<double> m_slots;
};

} // namespace amrvis
