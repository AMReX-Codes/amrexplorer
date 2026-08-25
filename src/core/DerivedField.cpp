#include <amrexplorer/core/DerivedField.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace amrvis {
namespace {

// The coordinate symbols, indexed by axis.
constexpr std::array<std::string_view, 3> coordinateNames{"x", "y", "z"};

std::string describe(std::size_t definitionIndex, const std::string& name,
    const std::string& message)
{
    const auto subject = name.empty()
        ? "derived field " + std::to_string(definitionIndex + 1)
        : "derived field '" + name + "'";
    return subject + ": " + message;
}

// The index of `name` in the fields installed so far, which is also its
// FieldId: stored fields keep the order the plotfile lists them in and each
// derived field is appended as it is resolved.
std::optional<std::size_t> fieldIndex(
    const DatasetMetadata& metadata, std::string_view name)
{
    for (std::size_t index = 0; index < metadata.fields.size(); ++index) {
        if (metadata.fields[index].name == name) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<int> coordinateAxis(std::string_view name)
{
    for (int axis = 0; axis < 3; ++axis) {
        if (coordinateNames[static_cast<std::size_t>(axis)] == name) {
            return axis;
        }
    }
    return std::nullopt;
}

} // namespace

DerivedFieldError::DerivedFieldError(std::size_t definitionIndex,
    const std::string& name, const std::string& message,
    std::optional<std::size_t> sourceOffset)
    : std::invalid_argument(describe(definitionIndex, name, message))
    , m_definitionIndex(definitionIndex)
    , m_sourceOffset(sourceOffset)
    , m_message(message)
{
}

std::size_t DerivedFieldError::definitionIndex() const noexcept
{
    return m_definitionIndex;
}

const std::optional<std::size_t>& DerivedFieldError::sourceOffset()
    const noexcept
{
    return m_sourceOffset;
}

const std::string& DerivedFieldError::message() const noexcept
{
    return m_message;
}

std::optional<DerivedFieldListFault> validateDerivedFieldGraph(
    std::span<const DerivedFieldDefinition> definitions)
{
    // Mirrors what installDerivedFields counts, without a dataset to resolve
    // against: a symbol naming an earlier definition is a derived input, x/y/z
    // is a coordinate, and anything else is a stored field -- which is what it
    // will be in every dataset that can satisfy the list at all.
    std::vector<std::size_t> depths;
    depths.reserve(definitions.size());
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const auto& definition = definitions[index];
        // Not default-constructible, and an expression that does not parse is
        // the caller's to report, with the offset it carries.
        std::optional<CompiledExpression> expression;
        try {
            expression = CompiledExpression::compile(definition.expression);
        } catch (const ExpressionError&) {
            return std::nullopt;
        }
        std::size_t fieldInputs = 0;
        std::size_t depth = 1;
        for (const auto& symbol : expression->symbols()) {
            std::optional<std::size_t> earlier;
            for (std::size_t before = 0; before < index; ++before) {
                if (definitions[before].name == symbol) {
                    earlier = before;
                }
            }
            if (earlier) {
                ++fieldInputs;
                depth = std::max(depth, depths[*earlier] + 1);
                continue;
            }
            if (coordinateAxis(symbol)) {
                continue;
            }
            ++fieldInputs;
        }
        if (fieldInputs > maximumDerivedFieldInputs) {
            return DerivedFieldListFault{index,
                "an expression may read at most "
                    + std::to_string(maximumDerivedFieldInputs) + " fields"};
        }
        if (depth > maximumDerivedFieldDepth) {
            return DerivedFieldListFault{index,
                "a derived field may not read a chain of more than "
                    + std::to_string(maximumDerivedFieldDepth)
                    + " derived fields"};
        }
        depths.push_back(depth);
    }
    return std::nullopt;
}

DerivedFieldInstallation installDerivedFields(DatasetMetadata& metadata,
    std::span<const DerivedFieldDefinition> definitions,
    DerivedFieldPolicy policy)
{
    DerivedFieldInstallation installation;
    installation.programs.reserve(definitions.size());
    // Where the stored fields end, so a resolved input can be recognised as
    // derived (its id is storedCount + its program's index, which is how
    // PlotfileDataset finds the program again), and the depth of each
    // installed program, for the chain bound below.
    const auto storedCount = metadata.fields.size();
    std::vector<std::size_t> depths;
    depths.reserve(definitions.size());
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const auto& definition = definitions[index];
        const auto fail = [index, &definition](const std::string& message,
                              std::optional<std::size_t> offset = std::nullopt) {
            return DerivedFieldError(index, definition.name, message, offset);
        };
        // Under Skip the loop body's throws are caught per definition, so one
        // that does not resolve leaves the metadata as it was before it and the
        // ones after it are still tried. Written as a try around the body
        // rather than as a return at every check so the two policies cannot
        // drift apart on which problems they report.
        try {
            // Thrown per definition rather than up front so Skip keeps its
            // promise: the ones that fit are installed and the rest are
            // reported, instead of the whole list failing.
            if (installation.programs.size() >= maximumDerivedFieldCount) {
                throw fail("a dataset may have at most "
                    + std::to_string(maximumDerivedFieldCount)
                    + " derived fields");
            }
            if (definition.name.empty()) {
                throw fail("a derived field needs a name");
            }
            if (fieldIndex(metadata, definition.name)) {
                // Both a stored field and an earlier definition:
                // field names are the namespace expressions resolve in,
                // and validateMetadata requires them unique.
                throw fail("the name is already in use by another field");
            }
            if (definition.expression.empty()) {
                throw fail("a derived field needs an expression");
            }

            DerivedFieldProgram program{
                [&] {
                    try {
                        return CompiledExpression::compile(
                            definition.expression);
                    } catch (const ExpressionError& error) {
                        // The offset is into what the user typed: symbols
                        // come from the source as written, so nothing has
                        // to be mapped back through a rewrite.
                        throw fail(error.what(), error.offset());
                    }
                }(),
                {}};

            const auto symbols = program.expression.symbols();
            program.inputs.reserve(symbols.size());
            // Every field an expression reads must share one centering, because
            // its values are combined sample for sample; the derived field then
            // has that centering. A coordinates-only expression is cell-centered.
            std::optional<Centering> centering;
            for (const auto& symbol : symbols) {
                if (const auto input = fieldIndex(metadata, symbol)) {
                    if (*input > std::numeric_limits<std::uint32_t>::max()) {
                        throw fail("the dataset has more fields than a field id "
                                   "can name");
                    }
                    const auto& field = metadata.fields[*input];
                    if (centering && *centering != field.centering) {
                        throw fail("'" + symbol
                            + "' is not centered like the other fields the "
                              "expression reads");
                    }
                    centering = field.centering;
                    program.inputs.push_back(
                        {.field = FieldId{static_cast<std::uint32_t>(*input)},
                            .axis = -1});
                    continue;
                }
                if (const auto axis = coordinateAxis(symbol)) {
                    if (!metadata.hasPhysicalGeometry) {
                        throw fail("'" + symbol
                            + "' needs physical coordinates, which this dataset "
                              "does not carry");
                    }
                    if (*axis >= metadata.dimension) {
                        throw fail("'" + symbol + "' is not an axis of a "
                            + std::to_string(metadata.dimension)
                            + "-D dataset");
                    }
                    program.inputs.push_back({.field = {}, .axis = *axis});
                    continue;
                }
                throw fail("no field or coordinate is named '" + symbol + "'");
            }

            // Counted over the fields, not the symbols: this bounds the
            // blocks one evaluation holds pinned, and a coordinate is
            // computed rather than read.
            const auto fieldInputs = static_cast<std::size_t>(
                std::count_if(program.inputs.begin(), program.inputs.end(),
                    [](const DerivedFieldInput& input) {
                        return !input.isCoordinate();
                    }));
            if (fieldInputs > maximumDerivedFieldInputs) {
                throw fail("an expression may read at most "
                    + std::to_string(maximumDerivedFieldInputs) + " fields");
            }
            // One deeper than the deepest derived field it reads. Evaluation
            // recurses once per link, so this is what keeps that recursion off
            // the end of the stack.
            std::size_t depth = 1;
            for (const auto& input : program.inputs) {
                if (input.isCoordinate()) {
                    continue;
                }
                const auto id = static_cast<std::size_t>(input.field.value);
                if (id >= storedCount) {
                    depth = std::max(depth, depths[id - storedCount] + 1);
                }
            }
            if (depth > maximumDerivedFieldDepth) {
                throw fail("a derived field may not read a chain of more than "
                    + std::to_string(maximumDerivedFieldDepth)
                    + " derived fields");
            }

            metadata.fields.push_back(FieldMetadata{
                .name = definition.name,
                .centering = centering.value_or(Centering::Cell),
                .componentNames = {}});
            installation.programs.push_back(std::move(program));
            depths.push_back(depth);
        } catch (const DerivedFieldError& error) {
            if (policy == DerivedFieldPolicy::Strict) {
                throw;
            }
            installation.skipped.push_back(
                {index, definition.name, error.message()});
        }
    }
    return installation;
}

} // namespace amrvis
