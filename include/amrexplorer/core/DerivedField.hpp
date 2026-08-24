#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/expression/Expression.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Fields the viewer computes rather than reads: a name and an algebraic
// expression over the fields a dataset does store (expression/Expression.hpp
// for the grammar). Resolving a definition against a dataset turns its symbols
// into field ids and coordinate axes and appends a FieldMetadata for it, after
// which it is an ordinary field to everything above -- one FieldId that the
// slice, line, page, volume and range paths all treat like any other.

namespace amrvis {

// What the user typed. This is also what a settings file or an exported
// expression list carries, so it stays plain data.
struct DerivedFieldDefinition {
    std::string name;
    std::string expression;

    friend bool operator==(
        const DerivedFieldDefinition&, const DerivedFieldDefinition&) = default;
};

// Where one symbol of a resolved expression takes its value from: a field of
// the dataset, or a coordinate axis.
struct DerivedFieldInput {
    // The field to read, when axis is negative.
    FieldId field;
    // 0, 1, 2 for the x, y, z sample coordinate; -1 for a field input.
    int axis = -1;

    [[nodiscard]] bool isCoordinate() const noexcept { return axis >= 0; }
};

// A definition resolved against one dataset: the compiled expression plus, in
// expression.symbols() order, where each symbol's values come from.
struct DerivedFieldProgram {
    CompiledExpression expression;
    std::vector<DerivedFieldInput> inputs;
};

// What to do with a definition that does not resolve.
enum class DerivedFieldPolicy : std::uint8_t {
    // Throw DerivedFieldError on the first one. What an editor validating a
    // list the user just typed wants: the reason, and the definition it
    // belongs to.
    Strict,
    // Leave it out and record why. What opening a dataset wants: a definition
    // written for another plotfile must not stop this one from opening, and a
    // sequence frame that happens to lack a field must still load.
    Skip
};

// A definition that was left out under DerivedFieldPolicy::Skip.
struct DerivedFieldSkip {
    std::size_t definitionIndex = 0;
    std::string name;
    // The same text DerivedFieldError would have carried.
    std::string reason;
};

struct DerivedFieldInstallation {
    // One per *installed* definition, in the order their fields were appended
    // -- which is the definition order with the skipped ones removed.
    std::vector<DerivedFieldProgram> programs;
    // Empty under Strict, which throws instead.
    std::vector<DerivedFieldSkip> skipped;
};

class DerivedFieldError : public std::invalid_argument {
public:
    DerivedFieldError(std::size_t definitionIndex, const std::string& name,
        const std::string& message,
        std::optional<std::size_t> sourceOffset = std::nullopt);

    // Which definition of the installed list failed, so an editor can select
    // the row it belongs to.
    [[nodiscard]] std::size_t definitionIndex() const noexcept;
    // Byte offset into that definition's expression, set only when the problem
    // was inside the expression itself.
    [[nodiscard]] const std::optional<std::size_t>& sourceOffset()
        const noexcept;

private:
    std::size_t m_definitionIndex;
    std::optional<std::size_t> m_sourceOffset;
};

// Symbols one derived field may read. The cap bounds how many blocks
// evaluating one derived block has to hold pinned at once.
inline constexpr std::size_t maximumDerivedFieldInputs = 16;

// Appends one FieldMetadata per installed definition to metadata.fields, in
// order, and returns the programs that produce them. Definitions are resolved
// in order, so one may read the fields of any definition before it -- and only
// those, which makes the dependency graph acyclic by construction. A
// definition that is skipped is therefore also unavailable to the ones after
// it, which are then skipped for naming a field that is not there.
//
// A symbol names a stored field, then an earlier derived field, then a
// coordinate axis ("x", "y", "z"): a dataset with a field named x therefore
// shadows the coordinate, which is the only rule that stays predictable when
// the two collide.
//
// Under Strict the first problem throws and the caller is expected to discard
// the metadata it passed in; under Skip the metadata is always left usable.
// Either way the caller passes a copy, since a dataset's stored metadata is
// shared.
[[nodiscard]] DerivedFieldInstallation installDerivedFields(
    DatasetMetadata& metadata,
    std::span<const DerivedFieldDefinition> definitions,
    DerivedFieldPolicy policy = DerivedFieldPolicy::Strict);

} // namespace amrvis
