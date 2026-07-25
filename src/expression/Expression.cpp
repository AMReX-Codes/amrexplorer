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

std::string errorMessage(std::string message, std::size_t offset)
{
    return std::move(message) + " at byte " + std::to_string(offset);
}

enum class TokenKind : std::uint8_t {
    End,
    Number,
    Identifier,
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
        std::istringstream conversion(std::string{text});
        conversion.imbue(std::locale::classic());
        conversion >> result;
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
        if (m_current.kind == TokenKind::Plus) {
            advance();
            parseUnary();
            return;
        }
        if (m_current.kind == TokenKind::Minus) {
            advance();
            parseUnary();
            emit(Opcode::Negate);
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
            const auto index = symbolIndex(name);
            m_program.instructions.push_back({
                .opcode = Opcode::PushVariable,
                .index = index
            });
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

} // namespace amrvis
