#include <amrexplorer/core/DerivedField.hpp>

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

DerivedFieldInstallation installDerivedFields(DatasetMetadata& metadata,
    std::span<const DerivedFieldDefinition> definitions,
    DerivedFieldPolicy policy)
{
    DerivedFieldInstallation installation;
    installation.programs.reserve(definitions.size());
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
            if (symbols.size() > maximumDerivedFieldInputs) {
                throw fail("an expression may read at most "
                    + std::to_string(maximumDerivedFieldInputs) + " fields");
            }
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

            metadata.fields.push_back(FieldMetadata{
                .name = definition.name,
                .centering = centering.value_or(Centering::Cell),
                .componentNames = {}});
            installation.programs.push_back(std::move(program));
        } catch (const DerivedFieldError& error) {
            if (policy == DerivedFieldPolicy::Strict) {
                throw;
            }
            installation.skipped.push_back(
                {index, definition.name, error.what()});
        }
    }
    return installation;
}

} // namespace amrvis
