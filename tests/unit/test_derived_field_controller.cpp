#include "DerivedFieldController.hpp"
#include "DerivedFieldStore.hpp"
#include "ExpressionEditorDialog.hpp"

#include <QApplication>
#include <QEventLoop>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QTemporaryDir>
#include <QTimer>
#include <QTextDocument>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using amrvis::DerivedFieldDefinition;
using amrvis::DerivedFieldSkip;
using amrvis::qt::DerivedFieldController;
using amrvis::qt::DerivedFieldStore;
using amrvis::qt::ExpressionEditorDialog;

// The debounce the controller arms its diagnostics on. The tests wait against
// this rather than against numbers of their own, so raising it cannot quietly
// turn an assertion that nothing fired into one that fired late.
constexpr int kDiagnosticsDebounceMs = 250;

// Runs the event loop for a fixed span without spinning: a single-shot timer
// quits it, so the process idles instead of burning a core on processEvents.
void pump(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

// Long enough for a diagnostic to have fired and been seen not to. Used for the
// assertions that something must *not* appear, which is why it is a fixed span
// rather than a poll: there is no event to wait for.
void quiet()
{
    pump(kDiagnosticsDebounceMs * 3);
}

// Runs until `predicate` holds, or gives up. Returns as soon as it holds, so a
// passing test costs the debounce rather than the ceiling.
template <typename Predicate>
bool waitUntil(Predicate predicate, int ceilingMs = 2000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate()) {
        if (elapsed.elapsed() >= ceilingMs) {
            return false;
        }
        pump(10);
    }
    return true;
}

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

struct Fixture {
    // Set to false to stand for a session that cannot take derived fields (a
    // remote one) or for no dataset at all.
    bool datasetOpen = true;
    int reloads = 0;
    // The list this window's session is showing. A reload installs whatever is
    // committed, unless reloadFails stands in for the reopen a server refuses
    // or a connection that has gone -- which leaves the list committed here
    // and installed nowhere.
    std::vector<DerivedFieldDefinition> installed;
    bool reloadFails = false;
    // What the next chooseFile answers, and what it was asked for.
    QString chosenPath;
    std::optional<bool> lastChooseForSaving;
    // The fields the open dataset stores. Empty stands for no dataset open,
    // which the editor shows no field list and no warning for.
    QStringList storedFields{
        QStringLiteral("density"), QStringLiteral("temperature")};
    // The rest of the vocabulary: two datasets sharing every field name still
    // differ here if one of them has no z to speak of.
    QString geometry{QStringLiteral("2d+geo")};

    [[nodiscard]] DerivedFieldController::Hooks hooks(
        const DerivedFieldStore& store)
    {
        return DerivedFieldController::Hooks{
            .unavailableReason = [this]() -> QString {
                return datasetOpen ? QString()
                                   : QStringLiteral("no dataset here");
            },
            .reload = [this, &store] { reload(store); },
            // As the host does: reopen only where the session is not already
            // showing the list. That is what makes an unchanged Apply the
            // retry for a reload that failed, and a no-op everywhere else.
            .reloadIfMissing =
                [this, &store] {
                    if (installed != store.definitions()) {
                        reload(store);
                    }
                },
            .chooseFile =
                [this](QWidget*, bool forSaving) {
                    lastChooseForSaving = forSaving;
                    return chosenPath;
                },
            .storedFieldNames = [this] { return storedFields; },
            .datasetGeometry = [this] { return geometry; },
            // The real resolution against a dataset of those fields, so the
            // reasons the editor shows here are the ones a dataset gives.
            .resolveAgainstOpenDataset =
                [this](const std::vector<DerivedFieldDefinition>& definitions) {
                    std::vector<DerivedFieldSkip> skipped;
                    if (storedFields.isEmpty()) {
                        return skipped;
                    }
                    amrvis::DatasetMetadata metadata;
                    metadata.dimension = 2;
                    for (const auto& name : storedFields) {
                        amrvis::FieldMetadata field;
                        field.name = name.toStdString();
                        metadata.fields.push_back(std::move(field));
                    }
                    return amrvis::installDerivedFields(metadata, definitions,
                        amrvis::DerivedFieldPolicy::Skip)
                        .skipped;
                },
        };
    }

    void reload(const DerivedFieldStore& store)
    {
        ++reloads;
        if (!reloadFails) {
            installed = store.definitions();
        }
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
        DerivedFieldController controller(fixture.hooks(store), store);

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

    // The editor lists the dataset's stored fields, and measures a definition
    // against that dataset as it is *typed* -- and only then. A plotfile of
    // another shape opening under a list written for the last one is not the
    // user's error to correct, so nothing but an edit asks the question.
    {
        Fixture fixture;
        DerivedFieldStore store;
        DerivedFieldController controller(fixture.hooks(store), store);
        require(!controller.apply({{"twice", "density * 2"}}).has_value(),
            "a resolvable definition was refused");

        auto* parent = new QWidget;
        controller.showEditor(parent);
        auto* dialog = parent->findChild<ExpressionEditorDialog*>();
        require(dialog != nullptr, "the editor did not open");

        auto* fields =
            dialog->findChild<QListWidget*>(QStringLiteral("storedFieldList"));
        require(fields != nullptr && fields->count() == 2
                && fields->item(0)->text() == QStringLiteral("density")
                && fields->item(1)->text() == QStringLiteral("temperature"),
            "the editor does not list the dataset's stored fields");

        auto* warning =
            dialog->findChild<QLabel*>(QStringLiteral("expressionWarning"));
        auto* source = dialog->findChild<QPlainTextEdit*>(
            QStringLiteral("expressionSource"));
        require(warning != nullptr && source != nullptr,
            "the editor is missing its expression box or warning");
        require(warning->text().isEmpty(),
            "a definition this dataset can provide warned about itself");

        source->setPlainText(QStringLiteral("nonesuch * 2"));
        quiet();
        require(!warning->text().isEmpty(),
            "a hand-written expression naming no field of this dataset said "
            "nothing");
        // Named as the dataset's limitation rather than as a mistake, and
        // still committable: the list is shared with windows and plotfiles
        // that may have the field.
        require(!controller.apply(dialog->draft()).has_value(),
            "a definition this dataset cannot provide was refused");

        source->setPlainText(QStringLiteral("density * 3"));
        quiet();
        require(warning->text().isEmpty(),
            "the warning outlived the expression it was about");

        // What the user did not type is left alone. An import replaces every
        // row at once and is tolerated whole; the field list greys out what
        // this dataset cannot provide.
        dialog->setDraft({{"stranger", "absent * 2"}});
        quiet();
        require(warning->text().isEmpty(),
            "an imported definition was complained about");

        // And Apply takes it: it was never a claim about this dataset, so it
        // is committed and greyed out in the field list, as a list carried
        // from another plotfile is.
        auto* applyButton = dialog->findChild<QPushButton*>(
            QStringLiteral("applyExpressionsButton"));
        auto* error =
            dialog->findChild<QLabel*>(QStringLiteral("expressionError"));
        require(applyButton != nullptr && error != nullptr,
            "the editor is missing its Apply button or error label");
        applyButton->click();
        require(controller.definitions().size() == 1
                && controller.definitions()[0].name == "stranger"
                && error->text().isEmpty(),
            "an imported definition this dataset cannot provide was refused");

        // Typed here, the same kind of definition is held to the data in
        // front of the user, and Apply says so rather than committing it to
        // be discovered greyed out later.
        source->setPlainText(QStringLiteral("absent * 3"));
        quiet();
        require(!warning->text().isEmpty(),
            "the edited definition did not warn");
        applyButton->click();
        require(!error->text().isEmpty(),
            "a hand-written definition this dataset cannot provide was "
            "accepted");
        require(controller.definitions().size() == 1
                && controller.definitions()[0].expression == "absent * 2",
            "the refused edit was committed anyway");

        // And a dataset arriving raises nothing about a definition the user
        // has *not* touched -- carried here from another plotfile, which this
        // one may well be unable to provide through nobody's fault. (A row
        // they did write is a different matter and is answered again; that is
        // covered below.) setDraft is what makes this list carried rather
        // than written: it takes the baseline with it.
        dialog->setDraft({{"stranger", "absent * 2"}});
        require(!dialog->handEdited(0),
            "a freshly imported definition counted as written here");
        fixture.storedFields = QStringList{QStringLiteral("other")};
        controller.refreshAvailability();
        quiet();
        require(warning->text().isEmpty(),
            "opening another dataset raised a warning about an untouched "
            "definition");
        require(fields->count() == 1
                && fields->item(0)->text() == QStringLiteral("other"),
            "the field list did not follow the dataset");
        delete parent;
    }

    // The warning belongs to the row being written: not to the first thing
    // wrong in the list, and not to whatever slides into that row's number
    // afterwards. A refusal replaces it, and a deletion takes it away.
    {
        Fixture fixture;
        DerivedFieldStore store;
        DerivedFieldController controller(fixture.hooks(store), store);
        auto* parent = new QWidget;
        controller.showEditor(parent);
        auto* dialog = parent->findChild<ExpressionEditorDialog*>();
        require(dialog != nullptr, "the editor did not open");
        auto* warning =
            dialog->findChild<QLabel*>(QStringLiteral("expressionWarning"));
        auto* error =
            dialog->findChild<QLabel*>(QStringLiteral("expressionError"));
        auto* source = dialog->findChild<QPlainTextEdit*>(
            QStringLiteral("expressionSource"));
        auto* name =
            dialog->findChild<QLineEdit*>(QStringLiteral("expressionName"));
        auto* add = dialog->findChild<QPushButton*>(
            QStringLiteral("newExpressionButton"));
        auto* remove = dialog->findChild<QPushButton*>(
            QStringLiteral("deleteExpressionButton"));
        auto* applyButton = dialog->findChild<QPushButton*>(
            QStringLiteral("applyExpressionsButton"));
        require(warning != nullptr && error != nullptr && source != nullptr
                && name != nullptr && add != nullptr && remove != nullptr
                && applyButton != nullptr,
            "the editor is missing a widget this block drives");

        // Row 0 is left unnamed, which is a fault of its own and the first in
        // the list; row 1 is where the user is typing. The warning must be
        // about row 1's syntax, not about the data.
        add->click();
        source->setPlainText(QStringLiteral("density"));
        add->click();
        name->setText(QStringLiteral("bad"));
        source->setPlainText(QStringLiteral("sqrt("));
        waitUntil([&] { return !warning->text().isEmpty(); });
        require(!warning->text().isEmpty()
                && !warning->text().contains(QStringLiteral("dataset")),
            "a syntax error in the row being written was blamed on the "
            "dataset");

        // A refusal supersedes the advisory rather than standing beside it.
        dialog->setDraft({{"twice", "density * 2"}});
        source->setPlainText(QStringLiteral("nonesuch * 2"));
        waitUntil([&] { return !warning->text().isEmpty(); });
        require(!warning->text().isEmpty(), "the edited row did not warn");
        applyButton->click();
        require(!error->text().isEmpty() && warning->text().isEmpty(),
            "a refusal left the same sentence showing twice");

        // Deleting the only definition takes the warning with it: nothing is
        // selected afterwards, so no row change would.
        dialog->setDraft({{"twice", "nonesuch * 2"}});
        source->setPlainText(QStringLiteral("nonesuch * 3"));
        waitUntil([&] { return !warning->text().isEmpty(); });
        require(!warning->text().isEmpty(), "the edited row did not warn");
        remove->click();
        require(warning->text().isEmpty(),
            "deleting the definition left its warning on screen");

        // And a diagnostic queued for a row that is then deleted must not land
        // on whatever takes its number -- here a row that was edited too, so
        // every other guard passes.
        dialog->setDraft({{"first", "density"}, {"second", "density"}});
        auto* list =
            dialog->findChild<QListWidget*>(QStringLiteral("expressionList"));
        require(list != nullptr, "the editor has no definition list");
        // The successor is edited *and* unresolvable, so a diagnostic that
        // landed on it would have something to say -- which is what makes
        // this a test rather than a coincidence.
        list->setCurrentRow(1);
        source->setPlainText(QStringLiteral("absent * 2"));
        list->setCurrentRow(0);
        source->setPlainText(QStringLiteral("nonesuch * 2"));
        remove->click();
        quiet();
        require(warning->text().isEmpty(),
            "a diagnostic queued for a deleted row landed on its successor");

        // Apply overtakes a diagnostic still in flight: clicking it inside the
        // debounce must not have the advisory reappear beside the refusal a
        // moment later. Deliberately without waiting -- waiting is what lets
        // the timer fire first and hides this.
        dialog->setDraft({{"twice", "density * 2"}});
        source->setPlainText(QStringLiteral("nonesuch * 2"));
        applyButton->click();
        quiet();
        require(!error->text().isEmpty() && warning->text().isEmpty(),
            "a diagnostic in flight when Apply was refused came back beside "
            "the refusal");

        // A fault that belongs to the list and names a *different* row says
        // nothing about this one: reporting what the dataset then makes of it
        // would blame the data for a consequence of that other fault.
        {
            QStringList many;
            QStringList terms;
            for (int field = 0; field <= 16; ++field) {
                many.append(QStringLiteral("f%1").arg(field));
                terms.append(QStringLiteral("f%1").arg(field));
            }
            fixture.storedFields = many;
            controller.refreshAvailability();
            const auto wide = terms.join(QStringLiteral(" + "));
            dialog->setDraft({{"a", wide.toStdString()},
                {"b", wide.toStdString()}});
            list->setCurrentRow(1);
            source->setPlainText(wide + QStringLiteral(" + f0"));
            quiet();
            require(!warning->text().contains(QStringLiteral("dataset")),
                "a list fault in another row was reported as this dataset's "
                "doing");
        }

        // Typing something and taking it back is not writing a definition, so
        // it must not hold every later Apply to a list carried from another
        // plotfile -- there is nothing inside the editor that could clear it.
        dialog->setCommitted({{"carried", "absent * 2"}});
        source->setPlainText(QStringLiteral("absent * 3"));
        require(dialog->handEdited(0), "an edit did not count as one");
        // Let the warning actually appear, so the revert has something to
        // take back: without this the second edit merely restarts the timer
        // and nothing was ever on screen.
        waitUntil([&] { return !warning->text().isEmpty(); });
        require(!warning->text().isEmpty(), "the edit did not warn");
        source->setPlainText(QStringLiteral("absent * 2"));
        quiet();
        require(!dialog->handEdited(0),
            "a definition typed into and put back was still counted as "
            "written here");
        require(warning->text().isEmpty(),
            "the warning outlived the edit it was about");
        applyButton->click();
        require(error->text().isEmpty(),
            "Apply was deadlocked by a definition the user only brushed "
            "against");

        // The same, for a list that arrived by import rather than by commit:
        // its rows are measured against what was imported, not against a
        // committed list they were never part of.
        dialog->setDraft({{"imported", "absent * 4"}});
        source->setPlainText(QStringLiteral("absent * 5"));
        source->setPlainText(QStringLiteral("absent * 4"));
        require(!dialog->handEdited(0),
            "an imported definition put back was counted as written here");
        applyButton->click();
        require(error->text().isEmpty(),
            "Apply was deadlocked by an imported definition");

        // And a row typed from scratch is held to the dataset even when it
        // happens to match what used to sit at that index.
        dialog->setCommitted({{"a", "density"}, {"ghost", "absent * 2"}});
        list->setCurrentRow(1);
        remove->click();
        add->click();
        name->setText(QStringLiteral("ghost"));
        source->setPlainText(QStringLiteral("absent * 2"));
        require(dialog->handEdited(1),
            "a definition typed from scratch passed as one carried here");
        applyButton->click();
        require(!error->text().isEmpty(),
            "a hand-written definition this dataset cannot provide slipped "
            "through");

        // Which the user may overrule: the refusal offers it, and taking the
        // offer commits the same draft.
        auto* anyway = dialog->findChild<QPushButton*>(
            QStringLiteral("applyAnywayButton"));
        require(anyway != nullptr && anyway->isVisible(),
            "the refusal did not offer to commit anyway");
        anyway->click();
        require(error->text().isEmpty()
                && controller.definitions().size() == 2
                && controller.definitions()[1].name == "ghost",
            "Apply anyway did not commit the definition");

        // A field written in from the list lands where the user is typing,
        // which for a row they just selected is the end of what is there.
        fixture.storedFields = QStringList{
            QStringLiteral("density"), QStringLiteral("temperature")};
        controller.refreshAvailability();
        dialog->setDraft({{"twice", "density"}});
        list->setCurrentRow(0);
        auto* storedFields =
            dialog->findChild<QListWidget*>(QStringLiteral("storedFieldList"));
        require(storedFields != nullptr && storedFields->count() > 1,
            "the editor lists no stored fields to write in");
        const auto written = storedFields->item(1)->text();
        emit storedFields->itemDoubleClicked(storedFields->item(1));
        require(dialog->draft()[0].expression
                == (QStringLiteral("density ") + written).toStdString(),
            "a field written in from the list did not land at the end, "
            "separated from what was there");

        // A refusal that names a row other than the selected one still
        // carries its offer: showError selects that row, and the selection
        // handler clears the error box on its way past.
        dialog->setCommitted({{"a", "density"}, {"b", "temperature"}});
        list->setCurrentRow(0);
        source->setPlainText(QStringLiteral("nonesuch * 2"));
        list->setCurrentRow(1);
        source->setPlainText(QStringLiteral("temperature * 3"));
        applyButton->click();
        require(!error->text().isEmpty() && anyway->isVisible(),
            "a refusal on an unselected row arrived without its offer");

        // Editing after a refusal takes it down, so the advisory never paints
        // under it -- and the offer cannot commit rows nothing examined.
        source->setPlainText(QStringLiteral("nonesuch * 3"));
        require(error->text().isEmpty() && !anyway->isVisible(),
            "a refusal outlived the draft it was about");

        // Nor does it survive the dataset it named being replaced -- its
        // "Apply anyway" would otherwise offer to overrule a verdict computed
        // against data that is no longer open. A session installed *without*
        // the vocabulary moving leaves it alone, because the verdict still
        // describes what is open.
        applyButton->click();
        require(!error->text().isEmpty(), "the edit was not refused");
        controller.refreshAvailability();
        require(!error->text().isEmpty() && anyway->isVisible(),
            "an unchanged dataset took down a verdict still true of it");
        fixture.storedFields = QStringList{QStringLiteral("elsewhere")};
        controller.refreshAvailability();
        require(error->text().isEmpty() && !anyway->isVisible(),
            "a refusal about the previous dataset was left standing");
        fixture.storedFields = QStringList{
            QStringLiteral("density"), QStringLiteral("temperature")};
        controller.refreshAvailability();

        // A fault Apply will refuse is not painted in the colour that
        // promises nothing was stopped.
        dialog->setDraft({{"t", "density"}});
        source->setPlainText(QStringLiteral("sqrt("));
        waitUntil([&] { return !warning->text().isEmpty(); });
        require(warning->styleSheet().contains(QStringLiteral("red")),
            "a fault Apply will refuse was shown as advice");

        // A field written into the middle of a name is separated from both
        // sides, not just the one behind the cursor.
        dialog->setDraft({{"t", "density"}});
        list->setCurrentRow(0);
        {
            auto cursor = source->textCursor();
            cursor.setPosition(4);
            source->setTextCursor(cursor);
        }
        emit storedFields->itemDoubleClicked(storedFields->item(1));
        require(dialog->draft()[0].expression == "dens temperature ity",
            "a field written in mid-name merged with the tail");

        // The confirmation survives the reload the Apply itself started: that
        // load comes back through refreshAvailability, which is where a
        // verdict about the outgoing dataset is taken down.
        dialog->setDraft({{"fine", "density * 2"}});
        applyButton->click();
        auto* applied =
            dialog->findChild<QLabel*>(QStringLiteral("expressionApplied"));
        require(applied != nullptr && !applied->text().isEmpty(),
            "Apply did not confirm what it applied");
        controller.refreshAvailability();
        require(!applied->text().isEmpty(),
            "the reload the Apply started wiped its own confirmation");

        // A definition that fails only because one written above it could not
        // be provided is not the user's row to answer for: refusing it would
        // name a field they can see defined one line up, while the row that
        // actually broke it committed without comment.
        dialog->setDraft({{"base", "nonesuch"}});
        add->click();
        name->setText(QStringLiteral("twice"));
        source->setPlainText(QStringLiteral("base * 2"));
        quiet();
        require(warning->text().isEmpty(),
            "a definition was blamed for the failure of the one it reads");
        applyButton->click();
        require(error->text().isEmpty()
                && controller.definitions().size() == 2,
            "a definition was refused for the failure of the one it reads");

        // A row that fails for reasons of its own *as well* is still the
        // user's to answer for: naming a lost definition alongside a field
        // that was never there must not buy it a pass. Both orders, because
        // the first symbol scanned should not decide it.
        dialog->setDraft({{"base", "nonesuch"}});
        add->click();
        name->setText(QStringLiteral("mixed"));
        source->setPlainText(QStringLiteral("also_absent + base"));
        applyButton->click();
        require(!error->text().isEmpty(),
            "a row failing on a missing field slipped through behind a lost "
            "definition it also read");

        dialog->setDraft({{"base", "nonesuch"}});
        add->click();
        name->setText(QStringLiteral("mixed"));
        source->setPlainText(QStringLiteral("base + also_absent"));
        applyButton->click();
        require(!error->text().isEmpty(),
            "the order the symbols are written in decided whether the row "
            "was checked");

        // And a chain of them is still inherited all the way down.
        dialog->setDraft({{"base", "nonesuch"}, {"mid", "base * 2"}});
        add->click();
        name->setText(QStringLiteral("leaf"));
        source->setPlainText(QStringLiteral("mid + 1"));
        applyButton->click();
        require(error->text().isEmpty()
                && controller.definitions().size() == 3,
            "a definition two rows below the lost one was blamed for it");

        // A symbol that names an earlier lost definition *and* a stored field
        // resolved to the stored one, so the earlier row is not the reason
        // and the user's row is still theirs to answer for.
        dialog->setDraft({{"density", "nonesuch"}});
        add->click();
        name->setText(QStringLiteral("b"));
        source->setPlainText(QStringLiteral("density * absent"));
        applyButton->click();
        require(!error->text().isEmpty(),
            "a name collision with a stored field passed a genuine failure "
            "off as inherited");

        // The baseline tracks the draft through deletions at either end, so
        // "did the user write this row" keeps answering about the row it is
        // asked about and not about whatever used to sit at that number.
        dialog->setCommitted({{"a", "density"}, {"b", "temperature"},
            {"c", "density + 1"}});
        require(!dialog->handEdited(0) && !dialog->handEdited(1)
                && !dialog->handEdited(2),
            "a freshly committed list counted as written here");
        list->setCurrentRow(0);
        remove->click();
        require(dialog->draft().size() == 2 && !dialog->handEdited(0)
                && !dialog->handEdited(1),
            "deleting the first row made a survivor look written here");
        list->setCurrentRow(1);
        remove->click();
        require(dialog->draft().size() == 1 && !dialog->handEdited(0),
            "deleting the last row made the survivor look written here");
        source->setPlainText(QStringLiteral("temperature * 4"));
        require(dialog->handEdited(0),
            "an edit after deletions did not count as written here");
        remove->click();
        require(dialog->draft().empty() && !dialog->handEdited(0),
            "an empty draft still answered about a row");

        // A fault of the list that names a row *above* this one is still said
        // out loud: Apply will refuse it, and staying silent because it
        // belongs to another row leaves the advisory quiet about the thing
        // that is about to stop the user.
        {
            QStringList many;
            QStringList terms;
            for (int field = 0; field <= 16; ++field) {
                many.append(QStringLiteral("f%1").arg(field));
                terms.append(QStringLiteral("f%1").arg(field));
            }
            fixture.storedFields = many;
            controller.refreshAvailability();
            dialog->setDraft({{"wide", terms.join(QStringLiteral(" + ")).toStdString()},
                {"narrow", "f0"}});
            list->setCurrentRow(1);
            source->setPlainText(QStringLiteral("f0 + 1"));
            require(waitUntil([&] { return !warning->text().isEmpty(); }),
                "a list fault in the row above was never mentioned");
            require(!warning->text().contains(QStringLiteral("dataset")),
                "a fault of the list was blamed on the data");
            fixture.storedFields = QStringList{
                QStringLiteral("density"), QStringLiteral("temperature")};
            controller.refreshAvailability();
        }

        // A session installed without the vocabulary moving -- a sequence
        // frame, or the reload an Apply started -- leaves standing what the
        // editor has already said, and does not rebuild the field list under
        // the user's cursor.
        dialog->setDraft({{"twice", "density * 2"}});
        source->setPlainText(QStringLiteral("nonesuch * 2"));
        require(waitUntil([&] { return !warning->text().isEmpty(); }),
            "the edited row did not warn");
        {
            auto* storedList = dialog->findChild<QListWidget*>(
                QStringLiteral("storedFieldList"));
            require(storedList != nullptr && storedList->count() == 2,
                "the stored fields are not listed");
            storedList->setCurrentRow(1);
            controller.refreshAvailability();
            require(!warning->text().isEmpty(),
                "an unchanged dataset took down what the editor had said");
            require(storedList->currentRow() == 1,
                "an unchanged dataset rebuilt the field list under the user");
        }

        // A vocabulary that did move takes the verdict down -- and asks
        // again, so the row the user wrote is answered against the data now
        // open rather than left silent until their next keystroke. The new
        // dataset cannot provide it either, so the answer coming back is what
        // distinguishes a re-ask from a cancellation: without it the label
        // simply stays empty.
        require(dialog->handEdited(0),
            "the row under test is not one the user wrote");
        fixture.storedFields = QStringList{QStringLiteral("other"),
            QStringLiteral("another")};
        controller.refreshAvailability();
        require(waitUntil([&] { return !warning->text().isEmpty(); }),
            "the question was not asked again against the dataset now open");

        // Geometry alone counts as a move, even with every name unchanged.
        dialog->setDraft({{"twice", "density * 2"}});
        source->setPlainText(QStringLiteral("absent * 2"));
        require(waitUntil([&] { return !warning->text().isEmpty(); }),
            "the edited row did not warn");
        fixture.geometry = QStringLiteral("3d+geo");
        controller.refreshAvailability();
        require(waitUntil([&] { return warning->text().isEmpty(); }, 400),
            "a dimensionality change was mistaken for the same dataset");
        fixture.geometry = QStringLiteral("2d+geo");
        fixture.storedFields = QStringList{
            QStringLiteral("density"), QStringLiteral("temperature")};
        controller.refreshAvailability();

        // A row still being written is not complained about.
        dialog->setDraft({});
        add->click();
        name->setText(QStringLiteral("s"));
        quiet();
        require(warning->text().isEmpty(),
            "a half-typed definition was complained about");
        delete parent;
    }

    // A reload that fails leaves the list committed here and installed
    // nowhere, and the store emits nothing for a list that has not moved -- so
    // the Apply pressed again is the only thing left that can ask for it.
    {
        Fixture fixture;
        DerivedFieldStore store;
        DerivedFieldController controller(fixture.hooks(store), store);
        const std::vector<DerivedFieldDefinition> list{{"twice", "2*density"}};

        fixture.reloadFails = true;
        require(!controller.apply(list).has_value(),
            "a resolvable list was refused");
        require(fixture.reloads == 1 && fixture.installed.empty(),
            "a failed reload installed the list");

        fixture.reloadFails = false;
        require(!controller.apply(list).has_value(), "the retry was refused");
        require(fixture.reloads == 2 && fixture.installed == list,
            "an unchanged apply did not retry the reload that failed");

        // And once it is installed, pressing Apply again asks for nothing:
        // the ask goes to the one hook that drops it where the session
        // already carries the list.
        require(!controller.apply(list).has_value(),
            "a redundant apply was refused");
        require(fixture.reloads == 2,
            "an unchanged apply reloaded a window already showing the list");
    }

    // A list longer than a dataset can install is refused whole. Committing
    // it would install the first maximumDerivedFieldCount against every
    // dataset and skip the rest, so every window would list what it had
    // greyed out -- and an imported file is where such a length comes from.
    {
        Fixture fixture;
        DerivedFieldStore store;
        DerivedFieldController controller(fixture.hooks(store), store);
        std::vector<DerivedFieldDefinition> many;
        many.reserve(amrvis::maximumDerivedFieldCount + 1);
        for (std::size_t index = 0; index <= amrvis::maximumDerivedFieldCount;
            ++index) {
            many.push_back({"f" + std::to_string(index), "density"});
        }
        const auto refusal = controller.apply(many);
        require(refusal.has_value() && !refusal->definitionIndex.has_value(),
            "a list past the cap was accepted");
        require(controller.definitions().empty() && fixture.reloads == 0,
            "a refused list was committed");
        many.pop_back();
        require(!controller.apply(many).has_value(),
            "a list at the cap was refused");
    }

    // With no dataset that can take derived fields, the action is disabled and
    // apply refuses without touching anything.
    {
        Fixture fixture;
        fixture.datasetOpen = false;
        DerivedFieldStore store;
        DerivedFieldController controller(fixture.hooks(store), store);
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

        // Close closes it. Nothing asserted this, which is how removing the
        // button's connection -- on the belief that a parented
        // QDialogButtonBox wires rejected() to its dialog, which it does not
        // -- shipped as a dialog whose Close button did nothing.
        controller.showEditor(parent);
        auto* closable = parent->findChild<ExpressionEditorDialog*>();
        require(closable != nullptr, "the editor did not open");
        int finished = 0;
        QObject::connect(closable, &QDialog::finished,
            closable, [&finished](int) { ++finished; });
        closable->findChild<QPushButton*>(
                     QStringLiteral("closeExpressionsButton"))
            ->click();
        require(finished == 1 && !closable->isVisible(),
            "the editor's Close button did not close it");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        // An editor open when the dataset goes stays open: it edits the
        // session's list, not the dataset's, and File > Open leaves the
        // window without one for the whole of the load. Closing it would
        // take an unapplied draft -- an import, say -- with it.
        controller.showEditor(parent);
        require(parent->findChild<ExpressionEditorDialog*>() != nullptr,
            "the editor did not open");
        fixture.datasetOpen = false;
        controller.refreshAvailability();
        // The editor is WA_DeleteOnClose, so a close() would only schedule the
        // deletion: without delivering that, this passes whether the editor
        // was closed or not.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(parent->findChild<ExpressionEditorDialog*>() != nullptr,
            "losing the dataset closed the editor and its draft");
        delete parent;
    }

    // The tooltip format, which a GUI smoke test could only assert through a
    // combo box: an expression is folded onto one line, escaped, and wrapped
    // so Qt renders it as rich text whichever characters it happens to hold.
    {
        require(amrvis::qt::escapedExpression("density *\n    temperature")
                == QStringLiteral("density * temperature"),
            "the expression was not folded onto one line");
        require(amrvis::qt::escapedExpression("${a<b} & ${c}")
                == QStringLiteral("${a&lt;b} &amp; ${c}"),
            "the expression was not escaped");
        require(amrvis::qt::richTooltip(QStringLiteral("a &amp; b"))
                == QStringLiteral("<qt>a &amp; b</qt>"),
            "the tooltip was not wrapped as rich text");
        // Which is the point of the wrapper: on its own, text escaped for
        // markup is only *read* as markup when it holds an escaped `<`.
        require(!Qt::mightBeRichText(QStringLiteral("a &amp; b"))
                && Qt::mightBeRichText(
                    amrvis::qt::richTooltip(QStringLiteral("a &amp; b"))),
            "the wrapper does not make Qt read the tooltip as rich text");
    }

    // An imported list is bounded where its length is first seen, so an
    // oversized file never becomes a row per entry in the editor's list.
    {
        QByteArray entries;
        for (std::size_t index = 0; index <= amrvis::maximumDerivedFieldCount;
            ++index) {
            entries += entries.isEmpty() ? "" : ",";
            entries += QByteArray("{\"name\":\"f")
                + QByteArray::number(static_cast<qulonglong>(index))
                + "\",\"expression\":\"density\"}";
        }
        const auto json = QByteArray(
                              "{\"format\":\"amrexplorer-expression-list\","
                              "\"version\":1,\"expressions\":[")
            + entries + "]}";
        const auto parsed = amrvis::qt::parseExpressionList(json);
        require(!parsed.error.isEmpty() && parsed.definitions.empty(),
            "an expression list past the cap was parsed anyway");
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
        DerivedFieldController controller(fixture.hooks(store), store);
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
        DerivedFieldController one(first.hooks(store), store);
        DerivedFieldController two(second.hooks(store), store);
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

    // An editor open in another window: a definition committed next door
    // reaches it, but never over work started here and not yet applied.
    {
        Fixture first;
        Fixture second;
        DerivedFieldStore store;
        DerivedFieldController one(first.hooks(store), store);
        DerivedFieldController two(second.hooks(store), store);

        auto* parent = new QWidget;
        two.showEditor(parent);
        auto* peer = parent->findChild<ExpressionEditorDialog*>();
        require(peer != nullptr, "the editor did not open");
        const auto* notice =
            peer->findChild<QLabel*>(QStringLiteral("expressionNotice"));
        require(notice != nullptr && !notice->isVisible(),
            "the editor opened on a notice");

        // Nothing typed here, so the change is taken as it stands.
        require(!one.apply({{"speed", "density"}}).has_value(),
            "the list was refused");
        require(peer->draft().size() == 1 && peer->draft()[0].name == "speed",
            "an idle editor did not adopt the shared list");
        require(!peer->hasUnappliedEdits(),
            "an adopted list was left looking like an unapplied edit");
        require(!notice->isVisible(), "an adopted change was announced");

        // Now with a definition written here and not applied.
        peer->findChild<QPushButton*>(QStringLiteral("newExpressionButton"))
            ->click();
        peer->findChild<QLineEdit*>(QStringLiteral("expressionName"))
            ->setText(QStringLiteral("mine"));
        peer->findChild<QPlainTextEdit*>(QStringLiteral("expressionSource"))
            ->setPlainText(QStringLiteral("temperature"));
        require(peer->hasUnappliedEdits(), "typing left no unapplied edit");
        require(!one.apply({{"theirs", "density"}}).has_value(),
            "the second list was refused");
        require(peer->draft().size() == 2 && peer->draft()[1].name == "mine",
            "a commit in another window discarded unapplied work");
        require(notice->isVisible(),
            "the editor was not told the shared list had moved");

        // A standing conflict, not a message about the last thing done here:
        // adding a definition (or selecting another row, or a refused Apply)
        // goes through the same clear as an error would, and must not take
        // the warning away while the draft still overwrites the shared list.
        auto* add =
            peer->findChild<QPushButton*>(QStringLiteral("newExpressionButton"));
        add->click();
        require(notice->isVisible(),
            "adding a definition cleared the peer-change notice");
        peer->findChild<QPushButton*>(QStringLiteral("deleteExpressionButton"))
            ->click();
        require(notice->isVisible(),
            "deleting a definition cleared the peer-change notice");

        // The kept edits are still the ones Apply commits, and committing
        // them ends the notice rather than leaving it standing.
        peer->findChild<QPushButton*>(
                QStringLiteral("applyExpressionsButton"))
            ->click();
        require(store.definitions().size() == 2
                && store.definitions()[1].name == "mine",
            "applying the kept edits did not commit them");
        require(!peer->hasUnappliedEdits(),
            "an applied draft was still an unapplied edit");
        require(!notice->isVisible(), "the notice outlived the apply");
        delete parent;
    }

    std::cout << "derived field controller tests passed\n";
    return 0;
}
