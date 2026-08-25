#include "DerivedFieldController.hpp"
#include "ExpressionEditorDialog.hpp"

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
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
    QString settingsPath;
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
            .settings =
                [this] {
                    return std::make_unique<QSettings>(
                        settingsPath, QSettings::IniFormat);
                },
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

    // Validation, commit, persistence and the reload, which are the whole of
    // what committing a list does.
    {
        Fixture fixture;
        fixture.settingsPath = dir.filePath(QStringLiteral("apply.ini"));
        DerivedFieldController controller(fixture.hooks());

        // A refusal changes nothing: not the committed list, not the settings,
        // and no reload.
        const auto refusal = controller.apply({{"speed", "sqrt(absent)"}});
        require(refusal.has_value(), "an unresolvable definition was accepted");
        require(refusal->definitionIndex.has_value()
                && *refusal->definitionIndex == 0,
            "the refusal did not name the definition");
        require(refusal->message.contains(QStringLiteral("absent")),
            "the refusal does not say what was missing");
        require(controller.definitions().empty()
                && fixture.reloads == 0,
            "a refused apply still changed the committed list");
        require(!QFile::exists(fixture.settingsPath),
            "a refused apply wrote settings");

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

        // The reverse order cannot resolve: a definition may only read the
        // ones before it.
        const auto reversed = controller.apply({good[1], good[0]});
        require(reversed.has_value() && reversed->definitionIndex == 0,
            "a forward reference was accepted");
        require(controller.definitions() == good,
            "a refused apply replaced the committed list");

        // Persisted, and restored into a fresh controller as the same list.
        {
            Fixture other;
            other.settingsPath = fixture.settingsPath;
            DerivedFieldController restored(other.hooks());
            const QSettings settings(
                fixture.settingsPath, QSettings::IniFormat);
            restored.restore(settings);
            require(restored.definitions() == good,
                "the committed list did not survive a restore");
            require(other.reloads == 0,
                "restoring reloaded, which would reload nothing at startup");
        }

        // An empty list is a legitimate commit -- it clears the derived fields
        // -- and takes the stored key with it.
        require(!controller.apply({}).has_value(),
            "clearing the list was refused");
        require(controller.definitions().empty() && fixture.reloads == 2,
            "clearing the list did not commit and reload");
        {
            const QSettings settings(
                fixture.settingsPath, QSettings::IniFormat);
            DerivedFieldController restored(fixture.hooks());
            restored.restore(settings);
            require(restored.definitions().empty(),
                "a cleared list came back from the settings");
        }
    }

    // With no dataset that can take derived fields, the action is disabled and
    // apply refuses without touching anything.
    {
        Fixture fixture;
        fixture.datasetOpen = false;
        fixture.settingsPath = dir.filePath(QStringLiteral("closed.ini"));
        DerivedFieldController controller(fixture.hooks());
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
        fixture.settingsPath = dir.filePath(QStringLiteral("io.ini"));
        DerivedFieldController controller(fixture.hooks());
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

        // And a refusal from the editor shows in place, keeping the committed
        // list.
        dialog->findChild<QPlainTextEdit*>(
                   QStringLiteral("expressionSource"))
            ->setPlainText(QStringLiteral("absent + 1"));
        dialog->findChild<QPushButton*>(
                   QStringLiteral("applyExpressionsButton"))
            ->click();
        require(error->isVisible() && !error->text().isEmpty(),
            "a refusal was not shown in the editor");
        require(controller.definitions()[0].expression == "temperature",
            "a refused apply from the editor changed the committed list");
        delete parent;
    }

    // The report the host puts in the status bar when a dataset could not
    // provide some of the committed list.
    {
        Fixture fixture;
        fixture.settingsPath = dir.filePath(QStringLiteral("skip.ini"));
        DerivedFieldController controller(fixture.hooks());
        require(controller.skippedReport({}).isEmpty(),
            "nothing skipped still produced a report");
        const auto report = controller.skippedReport(
            {DerivedFieldSkip{0, "speed", "no field named 'u'"}});
        require(report.contains(QStringLiteral("speed"))
                && report.contains(QStringLiteral("no field named 'u'")),
            "the skip report names neither the field nor the reason");
    }

    std::cout << "derived field controller tests passed\n";
    return 0;
}
