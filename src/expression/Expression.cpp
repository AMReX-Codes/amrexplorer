#include <amrexplorer/expression/Expression.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace amrvis {
namespace expression_detail {

enum class Opcode : std::uint8_t {
    PushConstant,
    PushVariable,
    Negate,
    Add,
    Subtract,
    Multiply,
    Divide,
    Abs,
    Sqrt,
    Pow,
    Exp,
    Log,
    Exp10,
    Log10
};

struct Instruction {
    Opcode opcode;
    double constant = 0.0;
    std::size_t index = 0;
};

struct Program {
    std::vector<Instruction> instructions;
    std::vector<std::string> symbols;
    std::size_t stackDepth = 0;
};

} // namespace expression_detail
namespace {

using expression_detail::Instruction;
using expression_detail::Opcode;
using expression_detail::Program;

// Points per batch pass. Small enough that stackDepth slots stay in L1 and
// large enough to amortise the per-instruction dispatch.
constexpr std::size_t batchPoints = 512;

std::string errorMessage(std::string message, std::size_t offset)
{
    return std::move(message) + " at byte " + std::to_string(offset);
}

enum class TokenKind : std::uint8_t {
    End,
    Number,
    Identifier,
    // A ${...} field reference: never a function call, whatever it spells.
    Symbol,
    Plus,
    Minus,
    Star,
    Slash,
    Power,
    LeftParen,
    RightParen,
    Comma
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::size_t offset = 0;
    std::string_view text{};
    double number = 0.0;
};

bool isIdentifierStart(char value)
{
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z') || value == '_';
}

bool isIdentifierContinuation(char value)
{
    return isIdentifierStart(value) || (value >= '0' && value <= '9')
        || value == '.';
}

bool hasNonzeroSignificand(std::string_view text)
{
    for (const auto value : text) {
        if (value == 'e' || value == 'E') {
            break;
        }
        if (value >= '1' && value <= '9') {
            return true;
        }
    }
    return false;
}

class Lexer {
public:
    explicit Lexer(std::string_view source)
        : m_source(source)
    {
    }

    Token next()
    {
        skipHorizontalWhitespace();
        if (m_position == m_source.size()) {
            return {.kind = TokenKind::End, .offset = m_position};
        }

        const auto offset = m_position;
        const auto value = m_source[m_position];
        if (value == '\n' || value == '\r') {
            throw ExpressionError("newlines are not allowed", offset);
        }
        if ((value >= '0' && value <= '9')
            || (value == '.' && m_position + 1 < m_source.size()
                && m_source[m_position + 1] >= '0'
                && m_source[m_position + 1] <= '9')) {
            return number();
        }
        if (value == '$') {
            return braced();
        }
        if (isIdentifierStart(value)) {
            ++m_position;
            while (m_position < m_source.size()
                && isIdentifierContinuation(m_source[m_position])) {
                ++m_position;
            }
            return {
                .kind = TokenKind::Identifier,
                .offset = offset,
                .text = m_source.substr(offset, m_position - offset)
            };
        }

        ++m_position;
        switch (value) {
        case '+':
            return {.kind = TokenKind::Plus, .offset = offset, .text = "+"};
        case '-':
            return {.kind = TokenKind::Minus, .offset = offset, .text = "-"};
        case '*':
            if (m_position < m_source.size()
                && m_source[m_position] == '*') {
                ++m_position;
                return {
                    .kind = TokenKind::Power, .offset = offset, .text = "**"};
            }
            return {.kind = TokenKind::Star, .offset = offset, .text = "*"};
        case '/':
            return {.kind = TokenKind::Slash, .offset = offset, .text = "/"};
        case '(':
            return {
                .kind = TokenKind::LeftParen, .offset = offset, .text = "("};
        case ')':
            return {
                .kind = TokenKind::RightParen, .offset = offset, .text = ")"};
        case ',':
            return {.kind = TokenKind::Comma, .offset = offset, .text = ","};
        default:
            throw ExpressionError(
                "unexpected token '" + std::string(1, value) + "'", offset);
        }
    }

private:
    void skipHorizontalWhitespace()
    {
        while (m_position < m_source.size()
            && (m_source[m_position] == ' '
                || m_source[m_position] == '\t')) {
            ++m_position;
        }
    }

    // ${name}: everything between the braces is one symbol, verbatim, so a
    // field name with dashes, parentheses or spaces needs no escaping. The
    // whole token is reported at the '$' -- the offset a reader would point
    // at -- rather than at whichever inner byte the scan stopped on.
    Token braced()
    {
        const auto offset = m_position;
        ++m_position;
        if (m_position == m_source.size() || m_source[m_position] != '{') {
            throw ExpressionError("expected '{' after '$'", offset);
        }
        ++m_position;
        const auto start = m_position;
        while (m_position < m_source.size() && m_source[m_position] != '}') {
            if (m_source[m_position] == '\n' || m_source[m_position] == '\r') {
                throw ExpressionError("newlines are not allowed", m_position);
            }
            ++m_position;
        }
        if (m_position == m_source.size()) {
            throw ExpressionError("unterminated '${'", offset);
        }
        const auto text = m_source.substr(start, m_position - start);
        ++m_position;
        if (text.empty()) {
            throw ExpressionError("'${}' names no field", offset);
        }
        return {.kind = TokenKind::Symbol, .offset = offset, .text = text};
    }

    Token number()
    {
        const auto offset = m_position;
        while (m_position < m_source.size()
            && m_source[m_position] >= '0' && m_source[m_position] <= '9') {
            ++m_position;
        }
        if (m_position < m_source.size() && m_source[m_position] == '.') {
            ++m_position;
            while (m_position < m_source.size()
                && m_source[m_position] >= '0'
                && m_source[m_position] <= '9') {
                ++m_position;
            }
        }
        if (m_position < m_source.size()
            && (m_source[m_position] == 'e'
                || m_source[m_position] == 'E')) {
            ++m_position;
            if (m_position < m_source.size()
                && (m_source[m_position] == '+'
                    || m_source[m_position] == '-')) {
                ++m_position;
            }
            const auto exponentStart = m_position;
            while (m_position < m_source.size()
                && m_source[m_position] >= '0'
                && m_source[m_position] <= '9') {
                ++m_position;
            }
            if (m_position == exponentStart) {
                throw ExpressionError("invalid numeric exponent", m_position);
            }
        }

        const auto text = m_source.substr(offset, m_position - offset);
        double result = 0.0;
        // The classic locale, not the process one: a decimal point must mean a
        // decimal point wherever the viewer runs.
        std::istringstream conversion(std::string{text});
        conversion.imbue(std::locale::classic());
        conversion >> result;
        // The three ways a literal leaves the representable range, which the
        // standard libraries disagree about: a failbit, an infinity, and a
        // silent zero for an underflowing exponent ("1e-9999"). Rejecting all
        // three keeps the same source an error everywhere.
        if (conversion.fail() || !std::isfinite(result)
            || (result == 0.0 && hasNonzeroSignificand(text))) {
            throw ExpressionError("numeric literal is out of range", offset);
        }
        if (!conversion.eof()) {
            throw ExpressionError("invalid numeric literal", offset);
        }
        return {
            .kind = TokenKind::Number,
            .offset = offset,
            .text = text,
            .number = result
        };
    }

    std::string_view m_source;
    std::size_t m_position = 0;
};

class Parser {
public:
    explicit Parser(std::string_view source)
        : m_lexer(source)
        , m_current(m_lexer.next())
    {
    }

    std::shared_ptr<const Program> parse()
    {
        parseExpression();
        if (m_current.kind != TokenKind::End) {
            failUnexpected();
        }
        validateStack();
        return std::make_shared<const Program>(std::move(m_program));
    }

private:
    // Bounds both recursive descents below on the one counter: parenthesis and
    // call nesting through parseExpression, and sign chains ("---x") through
    // parseUnary. Without it a deep enough source overflows the C++ stack.
    class DepthGuard {
    public:
        explicit DepthGuard(Parser& parser)
            : m_parser(parser)
        {
            if (++m_parser.m_depth > maximumExpressionDepth) {
                m_parser.fail("expression nests too deeply");
            }
        }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
        DepthGuard(DepthGuard&&) = delete;
        DepthGuard& operator=(DepthGuard&&) = delete;
        ~DepthGuard() { --m_parser.m_depth; }

    private:
        Parser& m_parser;
    };

    void advance()
    {
        m_current = m_lexer.next();
    }

    [[noreturn]] void fail(std::string message) const
    {
        throw ExpressionError(std::move(message), m_current.offset);
    }

    [[noreturn]] void failUnexpected() const
    {
        if (m_current.kind == TokenKind::End) {
            fail("unexpected end of expression");
        }
        fail("unexpected token '" + std::string(m_current.text) + "'");
    }

    void expect(TokenKind kind, std::string message)
    {
        if (m_current.kind != kind) {
            fail(std::move(message));
        }
        advance();
    }

    void emit(Opcode opcode)
    {
        m_program.instructions.push_back({.opcode = opcode});
    }

    void parseExpression()
    {
        const DepthGuard guard(*this);
        parseAdditive();
    }

    void parseAdditive()
    {
        parseMultiplicative();
        while (m_current.kind == TokenKind::Plus
            || m_current.kind == TokenKind::Minus) {
            const auto operation = m_current.kind;
            advance();
            parseMultiplicative();
            emit(operation == TokenKind::Plus ? Opcode::Add : Opcode::Subtract);
        }
    }

    void parseMultiplicative()
    {
        parseUnary();
        while (m_current.kind == TokenKind::Star
            || m_current.kind == TokenKind::Slash) {
            const auto operation = m_current.kind;
            advance();
            parseUnary();
            emit(operation == TokenKind::Star ? Opcode::Multiply : Opcode::Divide);
        }
    }

    void parseUnary()
    {
        // Guarded only where it recurses into itself -- a sign chain. Taking
        // the guard on the way down to parsePower as well would charge two
        // levels for every parenthesis, halving the limit the constant states.
        if (m_current.kind == TokenKind::Plus
            || m_current.kind == TokenKind::Minus) {
            const DepthGuard guard(*this);
            const auto negate = m_current.kind == TokenKind::Minus;
            advance();
            parseUnary();
            if (negate) {
                emit(Opcode::Negate);
            }
            return;
        }
        parsePower();
    }

    void parsePower()
    {
        parsePrimary();
        if (m_current.kind == TokenKind::Power) {
            advance();
            parseUnary();
            emit(Opcode::Pow);
        }
    }

    void parsePrimary()
    {
        if (m_current.kind == TokenKind::Number) {
            m_program.instructions.push_back({
                .opcode = Opcode::PushConstant,
                .constant = m_current.number
            });
            advance();
            return;
        }
        if (m_current.kind == TokenKind::Symbol) {
            pushVariable(std::string(m_current.text));
            advance();
            return;
        }
        if (m_current.kind == TokenKind::Identifier) {
            parseIdentifier();
            return;
        }
        if (m_current.kind == TokenKind::LeftParen) {
            advance();
            parseExpression();
            expect(TokenKind::RightParen, "expected ')'");
            return;
        }
        if (m_current.kind == TokenKind::End) {
            fail("expected expression");
        }
        failUnexpected();
    }

    void parseIdentifier()
    {
        const auto name = std::string(m_current.text);
        const auto nameOffset = m_current.offset;
        advance();
        if (m_current.kind != TokenKind::LeftParen) {
            pushVariable(name);
            return;
        }

        advance();
        if (name == "pow") {
            parseExpression();
            expect(TokenKind::Comma, "expected ',' after first argument to pow");
            parseExpression();
            expect(TokenKind::RightParen, "expected ')' after arguments to pow");
            emit(Opcode::Pow);
            return;
        }

        const auto operation = unaryFunction(name);
        if (!operation.has_value()) {
            throw ExpressionError("unknown function '" + name + "'", nameOffset);
        }
        parseExpression();
        expect(
            TokenKind::RightParen, "expected ')' after argument to " + name);
        emit(*operation);
    }

    static std::optional<Opcode> unaryFunction(const std::string& name)
    {
        if (name == "abs") {
            return Opcode::Abs;
        }
        if (name == "sqrt") {
            return Opcode::Sqrt;
        }
        if (name == "exp") {
            return Opcode::Exp;
        }
        if (name == "log") {
            return Opcode::Log;
        }
        if (name == "exp10") {
            return Opcode::Exp10;
        }
        if (name == "log10") {
            return Opcode::Log10;
        }
        return std::nullopt;
    }

    void pushVariable(const std::string& name)
    {
        m_program.instructions.push_back({
            .opcode = Opcode::PushVariable,
            .index = symbolIndex(name)
        });
    }

    std::size_t symbolIndex(const std::string& name)
    {
        for (std::size_t index = 0; index < m_program.symbols.size(); ++index) {
            if (m_program.symbols[index] == name) {
                return index;
            }
        }
        m_program.symbols.push_back(name);
        return m_program.symbols.size() - 1;
    }

    void validateStack()
    {
        std::size_t depth = 0;
        std::size_t maximum = 0;
        for (const auto& instruction : m_program.instructions) {
            switch (instruction.opcode) {
            case Opcode::PushConstant:
            case Opcode::PushVariable:
                ++depth;
                maximum = std::max(maximum, depth);
                break;
            case Opcode::Negate:
            case Opcode::Abs:
            case Opcode::Sqrt:
            case Opcode::Exp:
            case Opcode::Log:
            case Opcode::Exp10:
            case Opcode::Log10:
                if (depth < 1) {
                    throw std::logic_error(
                        "expression compiler produced an invalid unary operation");
                }
                break;
            case Opcode::Add:
            case Opcode::Subtract:
            case Opcode::Multiply:
            case Opcode::Divide:
            case Opcode::Pow:
                if (depth < 2) {
                    throw std::logic_error(
                        "expression compiler produced an invalid binary operation");
                }
                --depth;
                break;
            }
        }
        if (depth != 1) {
            throw std::logic_error(
                "expression compiler produced an invalid final stack");
        }
        m_program.stackDepth = maximum;
    }

    Lexer m_lexer;
    Token m_current;
    Program m_program;
    std::size_t m_depth = 0;
};

} // namespace

ExpressionError::ExpressionError(std::string message, std::size_t offset)
    : std::invalid_argument(errorMessage(std::move(message), offset))
    , m_offset(offset)
{
}

std::size_t ExpressionError::offset() const noexcept
{
    return m_offset;
}

CompiledExpression::CompiledExpression(
    std::shared_ptr<const expression_detail::Program> program)
    : m_program(std::move(program))
{
}

CompiledExpression CompiledExpression::compile(std::string_view source)
{
    if (source.size() > maximumExpressionBytes) {
        throw ExpressionError(
            "expression exceeds " + std::to_string(maximumExpressionBytes)
                + " bytes",
            maximumExpressionBytes);
    }
    return CompiledExpression(Parser(source).parse());
}

std::span<const std::string> CompiledExpression::symbols() const noexcept
{
    return m_program->symbols;
}

ExpressionEvaluator CompiledExpression::makeEvaluator() const
{
    return ExpressionEvaluator(m_program);
}

ExpressionEvaluator::ExpressionEvaluator(
    std::shared_ptr<const expression_detail::Program> program)
    : m_program(std::move(program))
    , m_stack(m_program->stackDepth)
{
}

double ExpressionEvaluator::evaluate(std::span<const double> variables)
{
    if (variables.size() != m_program->symbols.size()) {
        throw std::invalid_argument(
            "expression evaluator received "
            + std::to_string(variables.size()) + " variables; expected "
            + std::to_string(m_program->symbols.size()));
    }

    std::size_t depth = 0;
    for (const auto& instruction : m_program->instructions) {
        switch (instruction.opcode) {
        case Opcode::PushConstant:
            m_stack[depth++] = instruction.constant;
            break;
        case Opcode::PushVariable:
            m_stack[depth++] = variables[instruction.index];
            break;
        case Opcode::Negate:
            m_stack[depth - 1] = -m_stack[depth - 1];
            break;
        case Opcode::Add:
            --depth;
            m_stack[depth - 1] += m_stack[depth];
            break;
        case Opcode::Subtract:
            --depth;
            m_stack[depth - 1] -= m_stack[depth];
            break;
        case Opcode::Multiply:
            --depth;
            m_stack[depth - 1] *= m_stack[depth];
            break;
        case Opcode::Divide:
            --depth;
            m_stack[depth - 1] /= m_stack[depth];
            break;
        case Opcode::Abs:
            m_stack[depth - 1] = std::abs(m_stack[depth - 1]);
            break;
        case Opcode::Sqrt:
            m_stack[depth - 1] = std::sqrt(m_stack[depth - 1]);
            break;
        case Opcode::Pow:
            --depth;
            m_stack[depth - 1] = std::pow(m_stack[depth - 1], m_stack[depth]);
            break;
        case Opcode::Exp:
            m_stack[depth - 1] = std::exp(m_stack[depth - 1]);
            break;
        case Opcode::Log:
            m_stack[depth - 1] = std::log(m_stack[depth - 1]);
            break;
        case Opcode::Exp10:
            m_stack[depth - 1] = std::pow(10.0, m_stack[depth - 1]);
            break;
        case Opcode::Log10:
            m_stack[depth - 1] = std::log10(m_stack[depth - 1]);
            break;
        }
    }
    return m_stack.front();
}

void ExpressionEvaluator::evaluate(
    std::span<const std::span<const double>> columns, std::span<double> out)
{
    if (columns.size() != m_program->symbols.size()) {
        throw std::invalid_argument(
            "expression evaluator received " + std::to_string(columns.size())
            + " columns; expected "
            + std::to_string(m_program->symbols.size()));
    }
    for (const auto& column : columns) {
        if (column.size() != out.size()) {
            throw std::invalid_argument(
                "expression evaluator columns must be as long as its output");
        }
    }
    if (out.empty()) {
        return;
    }

    m_slots.resize(m_program->stackDepth * batchPoints);
    for (std::size_t start = 0; start < out.size(); start += batchPoints) {
        const auto points = std::min(batchPoints, out.size() - start);
        std::size_t depth = 0;
        const auto slot = [this](std::size_t index) {
            return m_slots.data() + index * batchPoints;
        };
        // Each instruction is one pass over the chunk. depth indexes the stack
        // of chunk-wide slots exactly as it indexes the scalar stack above.
        const auto push = [&](const double* source, bool constant) {
            auto* target = slot(depth++);
            if (constant) {
                std::fill(target, target + points, *source);
            } else {
                std::copy(source + start, source + start + points, target);
            }
        };
        const auto unary = [&](auto operation) {
            auto* target = slot(depth - 1);
            for (std::size_t i = 0; i < points; ++i) {
                target[i] = operation(target[i]);
            }
        };
        const auto binary = [&](auto operation) {
            --depth;
            auto* target = slot(depth - 1);
            const auto* source = slot(depth);
            for (std::size_t i = 0; i < points; ++i) {
                target[i] = operation(target[i], source[i]);
            }
        };
        for (const auto& instruction : m_program->instructions) {
            switch (instruction.opcode) {
            case Opcode::PushConstant:
                push(&instruction.constant, true);
                break;
            case Opcode::PushVariable:
                push(columns[instruction.index].data(), false);
                break;
            case Opcode::Negate:
                unary([](double value) { return -value; });
                break;
            case Opcode::Add:
                binary([](double left, double right) { return left + right; });
                break;
            case Opcode::Subtract:
                binary([](double left, double right) { return left - right; });
                break;
            case Opcode::Multiply:
                binary([](double left, double right) { return left * right; });
                break;
            case Opcode::Divide:
                binary([](double left, double right) { return left / right; });
                break;
            case Opcode::Abs:
                unary([](double value) { return std::abs(value); });
                break;
            case Opcode::Sqrt:
                unary([](double value) { return std::sqrt(value); });
                break;
            case Opcode::Pow:
                binary([](double left, double right) {
                    return std::pow(left, right);
                });
                break;
            case Opcode::Exp:
                unary([](double value) { return std::exp(value); });
                break;
            case Opcode::Log:
                unary([](double value) { return std::log(value); });
                break;
            case Opcode::Exp10:
                unary([](double value) { return std::pow(10.0, value); });
                break;
            case Opcode::Log10:
                unary([](double value) { return std::log10(value); });
                break;
            }
        }
        const auto* result = m_slots.data();
        std::copy(result, result + points,
            out.begin() + static_cast<std::ptrdiff_t>(start));
    }
}

} // namespace amrvis
