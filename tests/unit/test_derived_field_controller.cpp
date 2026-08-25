#include "DerivedFieldController.hpp"
#include "DerivedFieldStore.hpp"
#include "ExpressionEditorDialog.hpp"

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

namespace {

using amrvis::DerivedFieldDefinition;
using amrvis::DerivedFieldSkip;
using amrvis::qt::DerivedFieldController;
using amrvis::qt::DerivedFieldStore;
using amrvis::qt::ExpressionEditorDialog;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// The stored fields a definition is validated against.
amrvis::DatasetMetadata storedMetadata()
{
    amrvis::DatasetMetadata metadata;
    metadata.dimension = 2;
    metadata.finestLevel = 0;
    metadata.hasPhysicalGeometry = true;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        metadata.physicalDomain.lower[axis] = 0.0;
        metadata.physicalDomain.upper[axis] = 1.0;
    }
    amrvis::LevelMetadata level;
    level.cellSize = amrvis::Real3{{0.25, 0.25, 0.25}};
    level.domain.upper = amrvis::Int3{{3, 3, 3}};
    metadata.levels.push_back(level);
    metadata.fields = {
        amrvis::FieldMetadata{
            .name = "density", .centering = {}, .componentNames = {}},
        amrvis::FieldMetadata{
            .name = "temperature", .centering = {}, .componentNames = {}},
    };
    return metadata;
}

struct Fixture {
    // Set to false to stand for a session that cannot take derived fields (a
    // remote one) or for no dataset at all.
    bool datasetOpen = true;
    int reloads = 0;
    // What the next chooseFile answers, and what it was asked for.
    QString chosenPath;
    std::optional<bool> lastChooseForSaving;

    [[nodiscard]] DerivedFieldController::Hooks hooks()
    {
        return DerivedFieldController::Hooks{
            .available = [this] { return datasetOpen; },
            .storedMetadata =
                [this]() -> std::optional<amrvis::DatasetMetadata> {
                if (!datasetOpen) {
                    return std::nullopt;
                }
                return storedMetadata();
            },
            .reload = [this] { ++reloads; },
            .chooseFile =
                [this](QWidget*, bool forSaving) {
                    lastChooseForSaving = forSaving;
                    return chosenPath;
                },
        };
    }
};

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    QTemporaryDir dir;
    require(dir.isValid(), "could not create a scratch directory");

    // Validation, commit and the reload, which are the whole of what
    // committing a list does.
    {
        Fixture fixture;
        DerivedFieldStore store;
        DerivedFieldController controller(fixture.hooks(), store);

        // A refusal changes nothing: not the committed list, and no reload.
        // What is refused is what is wrong whatever the data -- an expression
        // that does not parse, an empty or repeated name.
        const auto refusal = controller.apply({{"speed", "sqrt(absent"}});
        require(refusal.has_value(), "an unparsable expression was accepted");
        require(refusal->definitionIndex.has_value()
                && *refusal->definitionIndex == 0,
            "the refusal did not name the definition");
        require(controller.definitions().empty()
                && fixture.reloads == 0,
            "a refused apply still changed the committed list");

        // The second definition is what makes this more than a single-entry
        // check: it reads the first, which only resolves because the list is
        // installed in order.
        const std::vector<DerivedFieldDefinition> good{
            {"speed", "sqrt(density**2 + temperature**2)"},
            {"twice", "2*speed"}};
        require(!controller.apply(good).has_value(),
            "a resolvable list was refused");
        require(controller.definitions() == good
                && fixture.reloads == 1,
            "an accepted apply did not commit and reload exactly once");

        // A name this dataset does not have is not a refusal: the list is
        // shared with windows that may have it, and it is shown greyed out
        // where it does not apply.
        auto withStranger = good;
        withStranger.push_back({"stranger", "absent * 2"});
        require(!controller.apply(withStranger).has_value(),
            "a definition for another dataset was refused");
        require(controller.definitions() == withStranger,
            "the definition for another dataset was not committed");
        // A repeated name is wrong whatever the data.
        auto repeated = good;
        repeated.push_back(good[0]);
        const auto duplicate = controller.apply(repeated);
        require(duplicate.has_value() && duplicate->definitionIndex == 2,
            "a repeated name was accepted");
        require(controller.definitions() == withStranger,
            "a refused apply replaced the committed list");
        require(!controller.apply(good).has_value(), "restoring was refused");

        // An empty list is a legitimate commit: it clears the derived fields.
        const auto before = fixture.reloads;
        require(!controller.apply({}).has_value(),
            "clearing the list was refused");
        require(controller.definitions().empty()
                && fixture.reloads == before + 1,
            "clearing the list did not commit and reload");
    }

    // With no dataset that can take derived fields, the action is disabled and
    // apply refuses without touching anything.
    {
        Fixture fixture;
        fixture.datasetOpen = false;
        DerivedFieldStore store;
        DerivedFieldController controller(fixture.hooks(), store);
        auto* parent = new QWidget;
        auto* action = controller.createAction(parent);
        require(action != nullptr && !action->isEnabled(),
            "the action is enabled with no dataset open");
        require(controller.apply({{"speed", "density"}}).has_value(),
            "apply succeeded with no dataset open");
        require(fixture.reloads == 0, "a refused apply reloaded");
        fixture.datasetOpen = true;
        controller.refreshAvailability();
        require(action->isEnabled(),
            "the action stayed disabled after a dataset opened");
        delete parent;
    }

    // The expression-list format, both directions, and the entries it refuses.
    {
        const std::vector<DerivedFieldDefinition> definitions{
            {"speed", "sqrt(u**2 + v**2)"}, {"drag", "-${x-momentum}"}};
        const auto json = amrvis::qt::writeExpressionList(definitions);
        const auto parsed = amrvis::qt::parseExpressionList(json);
        require(parsed.error.isEmpty() && parsed.definitions == definitions,
            "an expression list did not round-trip");
        // The format string is in every exported file, so renaming it is a
        // file-format change rather than a tidy-up. A round trip cannot catch
        // that -- both directions would move together -- so the spelling is
        // pinned here, as test_palette pins the palette settings keys.
        require(json.contains("\"amrexplorer-expression-list\"")
                && json.contains("\"version\": 1"),
            "the exported format identifier changed");
        for (const auto* bad : {"", "[]", "{}",
                 R"({"format":"other","version":1,"expressions":[]})",
                 R"({"format":"amrexplorer-expression-list","version":2,)"
                 R"("expressions":[]})",
                 R"({"format":"amrexplorer-expression-list","version":1,)"
                 R"("expressions":[3]})",
                 R"({"format":"amrexplorer-expression-list","version":1,)"
                 R"("expressions":[{"name":"a"}]})"}) {
            const auto rejected = amrvis::qt::parseExpressionList(bad);
            require(!rejected.error.isEmpty() && rejected.definitions.empty(),
                "a malformed expression list was accepted");
        }
    }

    // Import and export through the editor: an import replaces the draft only,
    // and an export writes the draft rather than the committed list.
    {
        Fixture fixture;
        DerivedFieldStore store;
        DerivedFieldController controller(fixture.hooks(), store);
        require(!controller.apply({{"speed", "density"}}).has_value(),
            "the starting list was refused");

        const auto importPath = dir.filePath(QStringLiteral("in.json"));
        {
            QFile file(importPath);
            require(file.open(QIODevice::WriteOnly), "could not write import");
            const auto json = amrvis::qt::writeExpressionList(
                {{"imported", "temperature"}});
            require(file.write(json) == json.size(), "short import write");
        }

        auto* parent = new QWidget;
        controller.showEditor(parent);
        auto* dialog = parent->findChild<ExpressionEditorDialog*>();
        require(dialog != nullptr, "the editor did not open");
        require(dialog->draft().size() == 1
                && dialog->draft()[0].name == "speed",
            "the editor did not open on the committed list");

        fixture.chosenPath = importPath;
        dialog->findChild<QPushButton*>(
                   QStringLiteral("importExpressionsButton"))
            ->click();
        require(fixture.lastChooseForSaving.has_value()
                && !*fixture.lastChooseForSaving,
            "the import did not ask for a file to read");
        require(dialog->draft().size() == 1
                && dialog->draft()[0].name == "imported",
            "the import did not replace the draft");
        require(controller.definitions().size() == 1
                && controller.definitions()[0].name == "speed",
            "the import committed without an apply");

        const auto exportPath = dir.filePath(QStringLiteral("out.json"));
        fixture.chosenPath = exportPath;
        dialog->findChild<QPushButton*>(
                   QStringLiteral("exportExpressionsButton"))
            ->click();
        require(fixture.lastChooseForSaving.has_value()
                && *fixture.lastChooseForSaving,
            "the export did not ask for a file to write");
        {
            QFile file(exportPath);
            require(file.open(QIODevice::ReadOnly), "the export wrote nothing");
            const auto written = amrvis::qt::parseExpressionList(file.readAll());
            require(written.error.isEmpty() && written.definitions.size() == 1
                    && written.definitions[0].name == "imported",
                "the export wrote the committed list instead of the draft");
        }

        // Apply from the editor: the draft becomes the committed list and the
        // host is asked to reload.
        const auto reloadsBefore = fixture.reloads;
        dialog->findChild<QPushButton*>(
                   QStringLiteral("applyExpressionsButton"))
            ->click();
        require(controller.definitions().size() == 1
                && controller.definitions()[0].name == "imported"
                && fixture.reloads == reloadsBefore + 1,
            "the editor's Apply did not commit the draft");
        const auto* error =
            dialog->findChild<QLabel*>(QStringLiteral("expressionError"));
        require(error != nullptr && !error->isVisible(),
            "an accepted apply left an error showing");

        // Applying leaves the user on the definition they were editing, not
        // back at the first one.
        {
            auto* second = dialog->findChild<QPushButton*>(
                QStringLiteral("newExpressionButton"));
            second->click();
            dialog->findChild<QLineEdit*>(QStringLiteral("expressionName"))
                ->setText(QStringLiteral("later"));
            dialog->findChild<QPlainTextEdit*>(
                       QStringLiteral("expressionSource"))
                ->setPlainText(QStringLiteral("density"));
            require(dialog->selectedIndex().has_value()
                    && *dialog->selectedIndex() == 1,
                "the new definition was not the selected one");
            dialog->findChild<QPushButton*>(
                       QStringLiteral("applyExpressionsButton"))
                ->click();
            require(dialog->selectedIndex().has_value()
                    && *dialog->selectedIndex() == 1,
                "applying moved the editor off the row being edited");
            require(dialog->draft().size() == 2
                    && dialog->draft()[1].name == "later",
                "applying did not keep the draft it committed");
        }

        // And a refusal from the editor -- an expression that does not parse,
        // since a name this dataset lacks is no longer one -- shows in place,
        // keeping the committed list.
        dialog->findChild<QPlainTextEdit*>(
                   QStringLiteral("expressionSource"))
            ->setPlainText(QStringLiteral("absent +"));
        dialog->findChild<QPushButton*>(
                   QStringLiteral("applyExpressionsButton"))
            ->click();
        require(error->isVisible() && !error->text().isEmpty(),
            "a refusal was not shown in the editor");
        require(controller.definitions()[0].expression == "temperature",
            "a refused apply from the editor changed the committed list");
        delete parent;
    }

    // Two windows, one store: a definition written in one is the other's too,
    // and both reload so it reaches both field lists.
    {
        Fixture first;
        Fixture second;
        DerivedFieldStore store;
        DerivedFieldController one(first.hooks(), store);
        DerivedFieldController two(second.hooks(), store);
        const std::vector<DerivedFieldDefinition> list{{"speed", "density"}};
        require(!one.apply(list).has_value(), "the list was refused");
        require(one.definitions() == list && two.definitions() == list,
            "the other window did not see the definition");
        require(first.reloads == 1 && second.reloads == 1,
            "both windows should have reloaded exactly once");
        // Applying what is already there moves nothing and reloads nobody.
        require(!two.apply(list).has_value(), "re-applying was refused");
        require(first.reloads == 1 && second.reloads == 1,
            "an unchanged list reloaded a window");
        // A window that cannot take derived fields is not reloaded, and still
        // sees the list.
        second.datasetOpen = false;
        require(!one.apply({}).has_value(), "clearing was refused");
        require(two.definitions().empty(), "the other window kept the list");
        require(second.reloads == 1,
            "a window with no usable dataset was reloaded");
    }

    std::cout << "derived field controller tests passed\n";
    return 0;
}
