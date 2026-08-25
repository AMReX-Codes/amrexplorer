#include <amrexplorer/core/DerivedField.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using amrvis::Centering;
using amrvis::DatasetMetadata;
using amrvis::DerivedFieldDefinition;
using amrvis::DerivedFieldError;
using amrvis::FieldMetadata;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// A 3-D dataset with physical geometry and three cell-centered fields, one of
// them named so that only ${...} can spell it.
DatasetMetadata makeMetadata(int dimension = 3)
{
    DatasetMetadata metadata;
    metadata.dimension = dimension;
    metadata.hasPhysicalGeometry = true;
    metadata.finestLevel = 0;
    // A whole, valid single-level dataset, so the validateMetadata assertion
    // below is about what installing did and not about a half-built fixture.
    for (std::size_t axis = 0; axis < 3; ++axis) {
        metadata.physicalDomain.lower[axis] = 0.0;
        metadata.physicalDomain.upper[axis] = 1.0;
    }
    amrvis::LevelMetadata level;
    level.level = 0;
    level.cellSize = amrvis::Real3{{0.25, 0.25, 0.25}};
    level.domain.upper = amrvis::Int3{{3, 3, 3}};
    level.storedComponents = 3;
    metadata.levels.push_back(level);
    metadata.fields = {
        FieldMetadata{.name = "density", .centering = Centering::Cell, .componentNames = {}},
        FieldMetadata{.name = "temperature", .centering = Centering::Cell, .componentNames = {}},
        FieldMetadata{.name = "x-momentum", .centering = Centering::Cell, .componentNames = {}},
    };
    return metadata;
}

// Strict is the default and what every assertion below except the Skip
// section expects.
std::vector<amrvis::DerivedFieldProgram> install(DatasetMetadata& metadata,
    const std::vector<DerivedFieldDefinition>& definitions)
{
    auto installation = amrvis::installDerivedFields(metadata, definitions);
    require(installation.skipped.empty(),
        "a strict install reported a skipped definition");
    return std::move(installation.programs);
}

// The rejection, with the definition it belongs to and a fragment of the
// reason. Asserting the index matters: an editor selects the failing row by it.
void requireRejected(const std::vector<DerivedFieldDefinition>& definitions,
    std::size_t definitionIndex, std::string_view message,
    DatasetMetadata metadata = makeMetadata())
{
    const auto fieldsBefore = metadata.fields.size();
    try {
        static_cast<void>(install(metadata, definitions));
    } catch (const DerivedFieldError& error) {
        require(error.definitionIndex() == definitionIndex,
            std::string("wrong definition index for '") + std::string(message)
                + "': " + std::to_string(error.definitionIndex()));
        require(std::string_view(error.what()).find(message)
                != std::string_view::npos,
            std::string("wrong message: ") + error.what());
        require(metadata.fields.size() == fieldsBefore + definitionIndex,
            "a rejected install kept more or fewer fields than the "
            "definitions before it");
        return;
    }
    throw std::runtime_error(
        "definitions were unexpectedly accepted: " + std::string(message));
}

} // namespace

int main()
{
    try {
        {
            // The ordinary case: two derived fields, the second reading the
            // first, both appended after the stored fields in list order.
            auto metadata = makeMetadata();
            const auto programs = install(metadata,
                {{"speed", "sqrt(density**2 + temperature**2)"},
                    {"scaled", "2*speed + ${x-momentum}"}});
            require(programs.size() == 2, "wrong program count");
            require(metadata.fields.size() == 5, "fields were not appended");
            require(metadata.fields[3].name == "speed"
                    && metadata.fields[4].name == "scaled",
                "derived fields are not in definition order");
            require(metadata.fields[3].centering == Centering::Cell,
                "derived centering was not taken from its inputs");
            require(amrvis::validateMetadata(metadata).empty(),
                "installed metadata no longer validates");

            const auto& first = programs[0].inputs;
            require(first.size() == 2 && first[0].field.value == 0
                    && first[1].field.value == 1
                    && !first[0].isCoordinate() && !first[1].isCoordinate(),
                "stored inputs did not resolve in symbol order");
            // The second reads the first derived field by its new id (3), and
            // the braced name resolves to the same field a bare name would.
            const auto& second = programs[1].inputs;
            require(second.size() == 2 && second[0].field.value == 3
                    && second[1].field.value == 2,
                "a derived field did not resolve as an input");
        }
        {
            // Coordinates: resolved by axis, and mixable with fields.
            auto metadata = makeMetadata();
            const auto programs = install(metadata,
                {{"radius", "sqrt(x**2 + y**2 + z**2)"},
                    {"column", "density*z"}});
            const auto& radius = programs[0].inputs;
            require(radius.size() == 3 && radius[0].isCoordinate()
                    && radius[0].axis == 0 && radius[1].axis == 1
                    && radius[2].axis == 2,
                "coordinate symbols did not resolve to axes");
            require(metadata.fields[3].centering == Centering::Cell,
                "a coordinates-only field is not cell-centered");
            const auto& column = programs[1].inputs;
            require(column.size() == 2 && !column[0].isCoordinate()
                    && column[1].isCoordinate() && column[1].axis == 2,
                "a mixed field/coordinate expression did not resolve");
        }
        {
            // A field named like a coordinate shadows it.
            auto metadata = makeMetadata();
            metadata.fields.push_back(
                FieldMetadata{.name = "x", .centering = Centering::Cell, .componentNames = {}});
            const auto programs = install(metadata, {{"shadowed", "x + 1"}});
            require(programs[0].inputs[0].isCoordinate() == false
                    && programs[0].inputs[0].field.value == 3,
                "a field named x did not shadow the coordinate");
        }
        {
            // Non-cell inputs are fine as long as they agree, and the derived
            // field inherits their centering.
            auto metadata = makeMetadata();
            metadata.fields = {
                FieldMetadata{.name = "bx", .centering = Centering::FaceX, .componentNames = {}},
                FieldMetadata{.name = "cx", .centering = Centering::FaceX, .componentNames = {}},
            };
            const auto programs = install(metadata, {{"sum", "bx + cx"}});
            require(programs.size() == 1, "a non-cell install failed");
            require(metadata.fields[2].centering == Centering::FaceX,
                "derived centering was not inherited from face-centered "
                "inputs");
        }
        {
            // An expression reporting a parse error carries the offset into
            // what the user typed, unmapped.
            auto metadata = makeMetadata();
            try {
                static_cast<void>(
                    install(metadata, {{"bad", "density + sin(1)"}}));
                throw std::runtime_error("a bad expression was accepted");
            } catch (const DerivedFieldError& error) {
                require(error.sourceOffset().has_value()
                        && *error.sourceOffset() == 10,
                    "the parse error offset does not point at the source");
            }
        }

        requireRejected({{"", "density"}}, 0, "needs a name");
        requireRejected({{"speed", ""}}, 0, "needs an expression");
        requireRejected({{"density", "1"}}, 0, "already in use");
        requireRejected({{"a", "1"}, {"a", "2"}}, 1, "already in use");
        requireRejected({{"a", "b"}}, 0, "no field or coordinate is named 'b'");
        // A self-reference is just an unknown name: the field is installed only
        // once its own expression has resolved, which is what keeps the
        // dependency graph acyclic.
        requireRejected({{"a", "a + 1"}}, 0, "no field or coordinate");
        // ...and so is a forward reference to a later definition.
        requireRejected({{"a", "b + 1"}, {"b", "density"}}, 0,
            "no field or coordinate");
        requireRejected({{"a", "density + z"}}, 0, "not an axis of a 2-D",
            makeMetadata(2));
        {
            auto metadata = makeMetadata();
            metadata.hasPhysicalGeometry = false;
            requireRejected(
                {{"a", "density*x"}}, 0, "physical coordinates", metadata);
        }
        {
            auto metadata = makeMetadata();
            metadata.fields[1].centering = Centering::Node;
            requireRejected({{"a", "density + temperature"}}, 0,
                "not centered like", metadata);
        }
        {
            // The list length is bounded too, and past the bound the ones that
            // fit are still installed -- Skip's promise -- while the rest are
            // reported.
            const auto limit = amrvis::maximumDerivedFieldCount;
            std::vector<DerivedFieldDefinition> many;
            for (std::size_t index = 0; index <= limit; ++index) {
                many.push_back(
                    {"f" + std::to_string(index), "density + 1"});
            }
            auto metadata = makeMetadata();
            const auto installation = amrvis::installDerivedFields(
                metadata, many, amrvis::DerivedFieldPolicy::Skip);
            require(installation.programs.size() == limit,
                "the definition cap installed the wrong number");
            require(installation.skipped.size() == 1
                    && installation.skipped[0].definitionIndex == limit
                    && installation.skipped[0].reason.find("at most")
                        != std::string::npos,
                "the definition past the cap was not reported");
            requireRejected(many, limit, "at most");

            // The cap counts what was installed, not how far down the list we
            // are: definitions the dataset cannot resolve take no slot, so the
            // ones after them still fit.
            std::vector<DerivedFieldDefinition> mixed;
            for (std::size_t index = 0; index < 8; ++index) {
                mixed.push_back({"skip" + std::to_string(index), "absent"});
            }
            for (std::size_t index = 0; index < limit; ++index) {
                mixed.push_back({"keep" + std::to_string(index), "density"});
            }
            auto second = makeMetadata();
            const auto capped = amrvis::installDerivedFields(
                second, mixed, amrvis::DerivedFieldPolicy::Skip);
            require(capped.programs.size() == limit,
                "unresolvable definitions consumed slots from the cap");
            require(capped.skipped.size() == 8,
                "the cap rejected definitions that had room");
            // The reason is the problem alone; the name lives beside it, and a
            // caller composing the two should not say it twice.
            require(capped.skipped[0].name == "skip0"
                    && capped.skipped[0].reason.rfind("derived field", 0) != 0,
                "the skip reason repeats the name it is stored with");
        }
        {
            // A chain of derived fields, each reading the one before it.
            // Evaluating the last one recurses once per link, so the chain is
            // bounded at installation; without that a long enough list is a
            // stack overflow in whichever worker evaluates it.
            const auto depth = amrvis::maximumDerivedFieldDepth;
            const auto chain = [](std::size_t links) {
                std::vector<DerivedFieldDefinition> definitions{
                    {"d0", "density"}};
                for (std::size_t index = 1; index < links; ++index) {
                    definitions.push_back({"d" + std::to_string(index),
                        "d" + std::to_string(index - 1) + " + 1"});
                }
                return definitions;
            };
            {
                auto metadata = makeMetadata();
                const auto programs = install(metadata, chain(depth));
                require(programs.size() == depth,
                    "a chain at the depth limit was not installed");
            }
            // ...and one link too many, which names the definition that
            // crossed the line rather than the whole list.
            requireRejected(chain(depth + 1), depth, "chain of more than");
            // Depth is the longest path, not the count: a definition may read
            // any number of shallow ones.
            {
                auto metadata = makeMetadata();
                std::vector<DerivedFieldDefinition> wide{{"a", "density"},
                    {"b", "temperature"}, {"c", "a + b"}};
                require(install(metadata, wide).size() == 3,
                    "a wide but shallow list was rejected");
            }
        }
        {
            // Coordinates are computed rather than read, so they do not count
            // against the cap that bounds how many blocks one evaluation
            // holds pinned -- and the message says "fields" because that is
            // what it counts.
            std::string expression = "x + y + z";
            auto metadata = makeMetadata();
            for (std::size_t index = 0; index < amrvis::maximumDerivedFieldInputs;
                ++index) {
                const auto name = "f" + std::to_string(index);
                expression += " + " + name;
                metadata.fields.push_back(FieldMetadata{
                    .name = name, .centering = Centering::Cell,
                    .componentNames = {}});
            }
            const auto programs = install(metadata, {{"a", expression}});
            require(programs.size() == 1,
                "three coordinates and the maximum fields were rejected");
            require(programs[0].inputs.size()
                    == amrvis::maximumDerivedFieldInputs + 3,
                "the coordinates did not resolve alongside the fields");
        }
        {
            // One field past the input cap.
            std::string expression = "density";
            for (std::size_t index = 0; index <= amrvis::maximumDerivedFieldInputs;
                ++index) {
                expression += " + ${f" + std::to_string(index) + "}";
            }
            auto metadata = makeMetadata();
            for (std::size_t index = 0; index <= amrvis::maximumDerivedFieldInputs;
                ++index) {
                metadata.fields.push_back(
                    FieldMetadata{.name = "f" + std::to_string(index),
                        .centering = Centering::Cell, .componentNames = {}});
            }
            requireRejected({{"a", expression}}, 0, "at most", metadata);
        }

        {
            // Skip: a definition that does not resolve is left out, the ones
            // around it are still installed, and the metadata stays usable.
            // The ids of the installed ones therefore follow the definition
            // order with the skipped ones removed.
            auto metadata = makeMetadata();
            auto installation = amrvis::installDerivedFields(metadata,
                std::vector<DerivedFieldDefinition>{
                    {"good", "density*2"},
                    {"missing", "absent + 1"},
                    {"alsogood", "good + temperature"},
                    {"chained", "missing + 1"},
                },
                amrvis::DerivedFieldPolicy::Skip);
            require(installation.programs.size() == 2,
                "Skip installed the wrong number of definitions");
            require(metadata.fields.size() == 5
                    && metadata.fields[3].name == "good"
                    && metadata.fields[4].name == "alsogood",
                "Skip left the metadata in the wrong shape");
            require(amrvis::validateMetadata(metadata).empty(),
                "Skip left the metadata invalid");
            require(installation.skipped.size() == 2,
                "Skip did not report both unresolvable definitions");
            require(installation.skipped[0].definitionIndex == 1
                    && installation.skipped[0].name == "missing"
                    && installation.skipped[0].reason.find("absent")
                        != std::string::npos,
                "the first skip does not name what was missing");
            // A definition reading a skipped one is skipped in turn, for
            // naming a field that is not there.
            require(installation.skipped[1].definitionIndex == 3
                    && installation.skipped[1].reason.find("'missing'")
                        != std::string::npos,
                "a definition reading a skipped one was not skipped too");
            // The one that read an installed derived field resolved to its id,
            // which is the id after the skip removed a slot.
            require(installation.programs[1].inputs[0].field.value == 3,
                "Skip left an installed definition pointing at the wrong id");
        }

        std::cout << "derived field tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "test_derived_field failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
