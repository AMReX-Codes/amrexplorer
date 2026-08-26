#include "DerivedFieldController.hpp"

#include "ExpressionEditorDialog.hpp"

#include <QAction>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <span>
#include <utility>

namespace amrvis::qt {
namespace {

constexpr auto kExpressionListFormat = "amrexplorer-expression-list";
constexpr int kExpressionListVersion = 1;

} // namespace

QString escapedExpression(const std::string& expression)
{
    // One line: an expression may be laid out over several, and a tooltip
    // showing the breaks would be as tall as the editor it was typed in.
    // simplified() folds every run of whitespace into a single space. Escaped
    // because it is the user's own bytes; richTooltip is what makes the
    // escaping show through as the characters they stand for.
    return QString::fromStdString(expression).simplified().toHtmlEscaped();
}

QString richTooltip(const QString& escaped)
{
    // Qt renders a tooltip as rich text only when it thinks it might be some:
    // `&lt;` decides it, `&amp;` on its own does not, so escaped text is shown
    // either as the user wrote it or with the escapes visible, depending on
    // which characters they used. The wrapper settles it for every string.
    return QStringLiteral("<qt>%1</qt>").arg(escaped);
}

ExpressionListFile parseExpressionList(const QByteArray& json)
{
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return {{},
            DerivedFieldController::tr("not a valid expression-list JSON "
                                       "file: %1")
                .arg(parseError.errorString())};
    }
    if (!document.isObject()) {
        // Well-formed JSON that is not an object: errorString() would say "no
        // error occurred", which is not a reason anyone can act on.
        return {{},
            DerivedFieldController::tr(
                "not an expression list: the file's top level is not an "
                "object")};
    }
    const auto root = document.object();
    const auto format = root.value(QStringLiteral("format"));
    const auto version = root.value(QStringLiteral("version"));
    const auto entries = root.value(QStringLiteral("expressions"));
    if (!format.isString()
        || format.toString() != QLatin1String(kExpressionListFormat)
        || !version.isDouble() || version.toInt() != kExpressionListVersion
        || !entries.isArray()) {
        return {{},
            DerivedFieldController::tr(
                "not a supported expression list (expected format \"%1\" "
                "version %2)")
                .arg(QLatin1String(kExpressionListFormat))
                .arg(kExpressionListVersion)};
    }

    const auto array = entries.toArray();
    // Refused before a row is built for any of it: the draft goes into a list
    // widget an item at a time, so a file with a hundred thousand entries
    // would freeze the window long before Apply could refuse its length.
    if (static_cast<std::size_t>(array.size()) > maximumDerivedFieldCount) {
        return {{},
            DerivedFieldController::tr(
                "an expression list may define at most %1 derived fields")
                .arg(static_cast<qulonglong>(maximumDerivedFieldCount))};
    }
    std::vector<DerivedFieldDefinition> definitions;
    definitions.reserve(static_cast<std::size_t>(array.size()));
    for (const auto& value : array) {
        if (!value.isObject()) {
            return {{},
                DerivedFieldController::tr(
                    "every expression-list entry must be an object")};
        }
        const auto entry = value.toObject();
        const auto name = entry.value(QStringLiteral("name"));
        const auto expression = entry.value(QStringLiteral("expression"));
        if (!name.isString() || !expression.isString()) {
            return {{},
                DerivedFieldController::tr(
                    "every expression-list entry needs string \"name\" and "
                    "\"expression\" values")};
        }
        definitions.push_back({name.toString().trimmed().toStdString(),
            expression.toString().trimmed().toStdString()});
    }
    return {std::move(definitions), {}};
}

QByteArray writeExpressionList(
    const std::vector<DerivedFieldDefinition>& definitions)
{
    QJsonArray entries;
    for (const auto& definition : definitions) {
        entries.append(QJsonObject{
            {QStringLiteral("name"), QString::fromStdString(definition.name)},
            {QStringLiteral("expression"),
                QString::fromStdString(definition.expression)}});
    }
    const QJsonObject root{
        {QStringLiteral("format"), QLatin1String(kExpressionListFormat)},
        {QStringLiteral("version"), kExpressionListVersion},
        {QStringLiteral("expressions"), entries}};
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

DerivedFieldController::DerivedFieldController(
    Hooks hooks, DerivedFieldStore& store, QObject* parent)
    : QObject(parent)
    , m_hooks(std::move(hooks))
    , m_store(store)
{
    connect(&m_store, &DerivedFieldStore::changed, this,
        [this] { adoptStoreChange(); });
}

void DerivedFieldController::adoptStoreChange()
{
    if (m_dialog && m_dialog->draft() == m_store.definitions()) {
        // Already editing exactly this list: nothing to replace, but it is now
        // the committed one -- whether this window's own Apply made the change
        // or a peer committed the same thing -- and saying otherwise leaves
        // the editor reporting unapplied edits it does not have.
        m_dialog->markDraftCommitted();
    } else if (m_dialog) {
        if (m_dialog->hasUnappliedEdits()) {
            // Written here and not applied. One list is shared by every
            // window, so adopting would take an editor out from under the user
            // mid-sentence, with nothing said and nothing to undo it with.
            m_dialog->showListChangedElsewhere();
        } else {
            m_dialog->setCommitted(
                m_store.definitions(), m_dialog->selectedIndex());
        }
    }
    // Including the window whose Apply made the change: one path installs the
    // list, whoever asked for it. A window that cannot take derived fields --
    // a FAB, or a session whose peer predates 1.4 -- has nothing to reload,
    // and will not show them either way. (A remote session on a 1.4 peer very
    // much does reload; that is what this protocol version is for.)
    if (m_hooks.reload && available()) {
        m_hooks.reload();
    }
}

QAction* DerivedFieldController::createAction(QWidget* parent)
{
    m_action = new QAction(tr("&Expression Editor..."), parent);
    m_action->setObjectName(QStringLiteral("expressionEditorAction"));
    connect(m_action, &QAction::triggered, this,
        [this, parent] { showEditor(parent); });
    refreshAvailability();
    return m_action;
}

void DerivedFieldController::refreshAvailability()
{
    const auto reason = m_hooks.unavailableReason
        ? m_hooks.unavailableReason()
        : tr("Derived fields need a dataset.");
    const auto usable = reason.isEmpty();
    if (m_action) {
        m_action->setEnabled(usable);
        // Never empty: the Variable menu shows tooltips (for the derived
        // rows), and QAction falls back to its own text, so an empty one pops
        // a tooltip repeating the entry's label.
        m_action->setToolTip(usable
                ? tr("Define fields computed from the stored ones")
                : reason);
    }
    // The editor stays open when the dataset does not. It edits the session's
    // list rather than the dataset's, and closing it would take with it a
    // draft the user has typed or imported and not yet applied -- during an
    // ordinary File > Open, which resets the dataset for the whole of the
    // load. Apply refuses meanwhile, saying why, and starts working again by
    // itself when a dataset it can install into arrives.
    if (m_dialog) {
        // Asked on every installed session -- every frame of a sequence, and
        // the reload an Apply of its own started -- so what follows must cost
        // nothing when the dataset's vocabulary has not moved. Which it has
        // not, for either of those.
        const auto moved = m_dialog->setStoredFields(
            m_hooks.storedFieldNames ? m_hooks.storedFieldNames()
                                     : QStringList{},
            datasetKey(reason));
        if (moved) {
            // Everything standing described a vocabulary that has gone: the
            // advisory, and the refusal whose "Apply anyway" would otherwise
            // offer to overrule a verdict computed against data that is not
            // open any more.
            m_dialog->showResolutionWarning({});
            // Only a verdict about the data. A fault in the definition itself
            // is still true whatever is open, and nothing would say it again:
            // the advisory speaks only about the selected row, and says
            // nothing at all about one still being written -- so wiping "a
            // derived field needs a name" leaves a clean-looking editor whose
            // Apply still refuses.
            m_dialog->clearDataRefusal();
            // And the answer is owed again. Re-armed rather than left to the
            // next keystroke, because a File > Open passes through a moment
            // with no dataset at all: a diagnostic armed before it would fire
            // during that gap, resolve against nothing, and take itself down,
            // leaving a row the user wrote with nothing said about it.
            if (m_diagnostics != nullptr) {
                m_diagnosticsRow = m_dialog->selectedIndex();
                m_diagnosticsRevision = m_dialog->draftRevision();
                m_diagnostics->start();
            }
        }
    }
}

QString DerivedFieldController::datasetKey(const QString& reason) const
{
    return reason + QLatin1Char('\n')
        + (m_hooks.datasetShape ? m_hooks.datasetShape() : QString{});
}

void DerivedFieldController::refreshDraftDiagnostics()
{
    if (!m_dialog) {
        return;
    }
    const auto index = m_dialog->selectedIndex();
    // Only the row this was armed for, and only while it is still the user's
    // own writing. A selection that moved on, or a draft an import replaced,
    // leaves a timer running whose answer is about something they never
    // touched -- and the one thing the warning must not do is complain about
    // a definition they merely carried here.
    if (!index || m_diagnosticsRow != index
        || m_diagnosticsRevision != m_dialog->draftRevision()) {
        // Stale: it belongs to a moment that has passed, and whatever is on
        // screen now was put there by that moment's own answer.
        return;
    }
    if (!m_dialog->handEdited(*index)) {
        // Put back the way it arrived while this was in flight. There is no
        // claim left to answer, so the answer to the old one goes too.
        m_dialog->showResolutionWarning({});
        return;
    }
    const auto& draft = m_dialog->draft();
    // Nothing to say about a row still being written. An empty name or
    // expression is not a verdict on anything, and saying so a quarter of a
    // second after the first character is a complaint about the user not
    // having finished a sentence. Apply asks for both, where it belongs.
    if (draft[*index].name.empty() || draft[*index].expression.empty()) {
        m_dialog->showResolutionWarning({});
        return;
    }
    // A fault in the definition first, and in its own words. It is wrong
    // wherever it is installed, so calling it unavailable *in this dataset*
    // would send the user looking for a plotfile that has the field when what
    // they have is a typo.
    if (const auto fault = definitionFaultAt(draft, *index)) {
        m_dialog->showResolutionWarning(fault->message, true);
        return;
    }
    // A row *above* this one that cannot be installed makes any verdict below
    // it unreliable: installation is ordered, so this definition may read the
    // ones before it, and what a dataset then makes of this row follows from
    // that fault rather than from the data. Rows after it cannot affect it.
    for (std::size_t earlier = 0; earlier < *index; ++earlier) {
        if (definitionFaultAt(draft, earlier)) {
            m_dialog->showResolutionWarning({});
            return;
        }
    }
    // The faults that belong to the list rather than to a row -- an expression
    // reading more fields than one evaluation may pin, a chain deeper than it
    // may recurse -- asked of the rows up to and including this one. Of the
    // prefix and not the whole list, because the check reports only its first
    // fault and gives up entirely on the first row that will not compile: a
    // half-written row *below* this one would otherwise answer for it, and
    // this row's own fault would reach the dataset wording it must never
    // wear. Everything above is known installable by the loop just above, so
    // a fault here is this row's.
    if (const auto fault = validateDerivedFieldGraph(
            std::span(draft).first(*index + 1))) {
        // Whichever row it names. It is a fault of the list, so the wording
        // does not blame the data, and Apply will refuse it -- saying nothing
        // because it belongs to a row above would leave the advisory silent
        // about something that is about to stop the user.
        m_dialog->showResolutionWarning(
            QString::fromStdString(fault->message), true);
        return;
    }
    if (!m_hooks.resolveAgainstOpenDataset) {
        m_dialog->showResolutionWarning({});
        return;
    }
    // The whole draft, not the one definition: a definition may read the ones
    // written above it, so what this one resolves to depends on them, and one
    // of them failing is why this one cannot be had either.
    const auto skipped = m_hooks.resolveAgainstOpenDataset(draft);
    const auto refusal = datasetRefusalFor(draft, *index, skipped);
    m_dialog->showResolutionWarning(refusal.value_or(QString{}));
}

std::optional<QString> DerivedFieldController::datasetRefusalFor(
    const std::vector<DerivedFieldDefinition>& definitions, std::size_t index,
    const std::vector<DerivedFieldSkip>& skipped) const
{
    const auto entry = std::find_if(skipped.begin(), skipped.end(),
        [index](const DerivedFieldSkip& skip) {
            return skip.definitionIndex == index;
        });
    if (entry == skipped.end()
        || failureIsInherited(definitions, index, skipped)) {
        return std::nullopt;
    }
    // An empty reason takes the wording that promises none: a skip decoded off
    // the wire may carry one, and "...: " with nothing after the colon tells
    // the user a reason exists while showing it to them blank.
    return entry->reason.empty()
        ? tr("This dataset cannot provide this field.")
        : tr("Unavailable in this dataset: %1")
              .arg(QString::fromStdString(entry->reason));
}

bool DerivedFieldController::failureIsInherited(
    const std::vector<DerivedFieldDefinition>& definitions, std::size_t index,
    const std::vector<DerivedFieldSkip>& skipped) const
{
    if (index >= definitions.size() || !m_hooks.resolveAgainstOpenDataset) {
        return false;
    }
    const auto lost = [&skipped](std::size_t row) {
        return std::find_if(skipped.begin(), skipped.end(),
                   [row](const DerivedFieldSkip& skip) {
                       return skip.definitionIndex == row;
                   })
            != skipped.end();
    };
    // The rows above that this dataset could not provide, made into something
    // it always can. A constant resolves wherever anything does, so what comes
    // back is this row's standing with its dependencies restored and nothing
    // else about the list changed.
    std::vector<DerivedFieldDefinition> probe(
        definitions.begin(), definitions.begin() + static_cast<std::ptrdiff_t>(index) + 1);
    bool substituted = false;
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
        if (lost(earlier)) {
            probe[earlier].expression = "0";
            substituted = true;
        }
    }
    if (!substituted) {
        // Nothing above it was lost, so nothing above it can be the reason.
        return false;
    }
    // Still skipped with its dependencies made good: the fault is its own.
    // A probe that somehow fails for a reason of its own leaves this false,
    // which refuses the row -- the safe direction, since the alternative is
    // committing a definition nothing checked.
    return !lost(index)
        || [&] {
               const auto again = m_hooks.resolveAgainstOpenDataset(probe);
               return std::find_if(again.begin(), again.end(),
                          [index](const DerivedFieldSkip& skip) {
                              return skip.definitionIndex == index;
                          })
                   == again.end();
           }();
}

std::optional<DerivedFieldController::Refusal>
DerivedFieldController::definitionFaultAt(
    const std::vector<DerivedFieldDefinition>& definitions,
    std::size_t index) const
{
    if (index >= definitions.size()) {
        return std::nullopt;
    }
    const auto& definition = definitions[index];
    if (definition.name.empty()) {
        return Refusal{tr("A derived field needs a name."), index};
    }
    // Against the rows before it only, which is the order installation
    // resolves in: a name is a duplicate of an earlier one, never of a later.
    const auto upto = definitions.begin() + static_cast<std::ptrdiff_t>(index);
    if (std::find_if(definitions.begin(), upto,
            [&definition](const DerivedFieldDefinition& earlier) {
                return earlier.name == definition.name;
            })
        != upto) {
        return Refusal{tr("Another derived field is already called \"%1\".")
                           .arg(QString::fromStdString(definition.name)),
            index};
    }
    if (definition.expression.empty()) {
        return Refusal{tr("A derived field needs an expression."), index};
    }
    try {
        static_cast<void>(CompiledExpression::compile(definition.expression));
    } catch (const ExpressionError& error) {
        return Refusal{QString::fromUtf8(error.what()), index};
    }
    return std::nullopt;
}

// What is wrong with a list whatever the data it is installed against. Asked
// by apply before it commits, and by the live diagnostics to tell a fault in
// the definition from a field this dataset happens not to have -- the two read
// alike in a DerivedFieldSkip's reason, and only the first is the user's to
// fix wherever they are.
std::optional<DerivedFieldController::Refusal>
DerivedFieldController::definitionFault(
    const std::vector<DerivedFieldDefinition>& definitions) const
{
    // Bounded here as well as at installation, because this is where an
    // imported file's length is first seen: past the cap every dataset would
    // install the first 256 and skip the rest, so each window would list
    // thousands of definitions it has greyed out, and re-install the list
    // frame by frame through a sequence.
    if (definitions.size() > maximumDerivedFieldCount) {
        return Refusal{tr("A list may define at most %1 derived fields.")
                           .arg(static_cast<qulonglong>(
                               maximumDerivedFieldCount)),
            std::nullopt};
    }

    // Checked without reference to any dataset: whether a definition applies
    // *here* is decided when a dataset installs it, and shown by greying the
    // field out. One list is shared by windows showing different data, so only
    // what is wrong whatever the data is can be refused.
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        if (auto fault = definitionFaultAt(definitions, index)) {
            return fault;
        }
    }

    // What is wrong with the list as a list, whatever the data: too many
    // fields read at once, or a dependency chain deeper than evaluation may
    // recurse. Left to installation these would be skipped per dataset, so
    // every window would grey the same row and say why, over a fault no data
    // could fix.
    if (const auto fault = validateDerivedFieldGraph(definitions)) {
        return Refusal{QString::fromStdString(fault->message),
            fault->definitionIndex};
    }

    return std::nullopt;
}

std::optional<DerivedFieldController::Refusal> DerivedFieldController::apply(
    std::vector<DerivedFieldDefinition> definitions,
    std::vector<std::size_t> mustResolveHere)
{
    // The same question the action's enablement asks, not a weaker one: the
    // editor is modeless, so the window can enter a state it is disabled for
    // (a FAB drill-down) while it is still open in front of the user.
    if (const auto reason = m_hooks.unavailableReason
            ? m_hooks.unavailableReason()
            : tr("Derived fields need a dataset.");
        !reason.isEmpty()) {
        // The same words the greyed action carries, so the editor and the menu
        // give one answer rather than two.
        return Refusal{reason, std::nullopt, false, true};
    }

    if (const auto fault = definitionFault(definitions)) {
        return *fault;
    }

    // What the user has written must work on the data in front of them. Only
    // what they wrote: the rest of the list may be theirs from another
    // plotfile or somebody else's from a file, and neither is a claim about
    // this dataset -- those install where they can and grey out where they
    // cannot, which is what makes one list usable across plotfiles at all.
    if (!mustResolveHere.empty() && m_hooks.resolveAgainstOpenDataset) {
        const auto skipped = m_hooks.resolveAgainstOpenDataset(definitions);
        std::sort(mustResolveHere.begin(), mustResolveHere.end());
        for (const auto index : mustResolveHere) {
            if (auto refusal = datasetRefusalFor(definitions, index, skipped)) {
                return Refusal{*std::move(refusal), index, true, true};
            }
        }
    }

    // set() is what reloads -- here and in every other window -- and does
    // nothing at all when the list has not moved. That last part is deliberate
    // and tested ("an unchanged list reloaded a window"), so a list that has
    // not moved is asked for again here instead, and only of this window: the
    // reload that failed is the one the user is looking at, and the hook drops
    // the ask where the session already carries the list, which is every
    // window an unchanged Apply has nothing to do in.
    const bool moved = definitions != m_store.definitions();
    m_store.set(std::move(definitions));
    if (!moved && m_hooks.reloadIfMissing) {
        m_hooks.reloadIfMissing();
    }
    return std::nullopt;
}

void DerivedFieldController::showEditor(QWidget* parent)
{
    if (m_dialog) {
        m_dialog->raise();
        m_dialog->activateWindow();
        return;
    }
    auto* dialog =
        new ExpressionEditorDialog(m_store.definitions(), parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    // The same key refreshAvailability composes. Spelled differently here, the
    // first session installed after the editor opened read as a change of
    // dataset and took down a verdict that was still true of it.
    dialog->setStoredFields(
        m_hooks.storedFieldNames ? m_hooks.storedFieldNames() : QStringList{},
        datasetKey(m_hooks.unavailableReason ? m_hooks.unavailableReason()
                                             : QString{}));
    // Owned by the dialog, so it goes when the dialog does and cannot fire
    // into a m_dialog that has been destroyed. Single-shot and restarted by
    // each edit: what the user wants is the verdict on what they have
    // finished typing, not one per keystroke.
    if (m_diagnostics == nullptr) {
        m_diagnostics = new QTimer(dialog);
        m_diagnostics->setSingleShot(true);
        m_diagnostics->setInterval(250);
        connect(m_diagnostics, &QTimer::timeout, this,
            [this] { refreshDraftDiagnostics(); });
    }
    connect(dialog, &ExpressionEditorDialog::draftEdited, dialog,
        [this] {
            m_diagnosticsRow = m_dialog ? m_dialog->selectedIndex()
                                        : std::nullopt;
            m_diagnosticsRevision =
                m_dialog ? m_dialog->draftRevision() : 0;
            m_diagnostics->start();
        });
    connect(dialog, &QObject::destroyed, this, [this] {
        m_diagnostics = nullptr;
    });
    // One path for both buttons: Apply holds what the user wrote to the open
    // dataset, "Apply anyway" commits the same draft without that. Everything
    // after the check is identical, and it is the part that must not drift.
    const auto commit = [this, dialog](bool holdToDataset) {
        // Whatever the debounce was about, this has overtaken it: left armed,
        // a refusal within the 250 ms would be joined a moment later by the
        // advisory it just replaced, in amber beside the red. A commit clears
        // it by moving the draft on; a refusal moves nothing, so it is
        // stopped here for both.
        if (m_diagnostics != nullptr) {
            m_diagnostics->stop();
        }
        // The rows the user has written, which are the ones Apply holds to
        // this dataset. Empty when they have chosen to commit regardless.
        std::vector<std::size_t> written;
        if (holdToDataset) {
            for (std::size_t index = 0; index < dialog->draft().size();
                ++index) {
                if (dialog->handEdited(index)) {
                    written.push_back(index);
                }
            }
        }
        if (const auto refusal = apply(dialog->draft(), std::move(written))) {
            dialog->showError(refusal->message, refusal->definitionIndex,
                refusal->confirmable, refusal->dependsOnDataset);
            return;
        }
        // The draft is now the committed list, which is what later edits are
        // measured against -- and what a change arriving from another window
        // may replace without asking. Same content, so nothing on screen moves
        // but the row the user was editing, which is kept.
        dialog->setCommitted(m_store.definitions(), dialog->selectedIndex());
        // Not a status message: apply() has already started the reload, and
        // showSlice clears the status bar when its slices land. The editor is
        // in front of the user anyway, so it says so itself.
        dialog->showApplied(m_store.definitions().size());
    };
    connect(dialog, &ExpressionEditorDialog::applyRequested, dialog,
        [commit] { commit(true); });
    connect(dialog, &ExpressionEditorDialog::applyAnywayRequested, dialog,
        [commit] { commit(false); });
    connect(dialog, &ExpressionEditorDialog::importRequested, dialog,
        [this, dialog] {
            if (!m_hooks.chooseFile) {
                return;
            }
            // chooseFile runs a nested event loop, which can deliver the
            // deleteLater that closing this editor posts (refreshAvailability
            // closes it when the dataset goes away). Nothing below may assume
            // the dialog outlived the call.
            const QPointer<ExpressionEditorDialog> alive(dialog);
            const auto path = m_hooks.chooseFile(dialog, false);
            if (path.isEmpty() || alive.isNull()) {
                return;
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                dialog->showError(tr("Could not open %1: %2")
                        .arg(QDir::toNativeSeparators(path),
                            file.errorString()),
                    std::nullopt);
                return;
            }
            auto parsed = parseExpressionList(file.readAll());
            if (!parsed.error.isEmpty()) {
                dialog->showError(
                    tr("%1: %2").arg(QDir::toNativeSeparators(path),
                        parsed.error),
                    std::nullopt);
                return;
            }
            // Imported into the draft, not committed: nothing reaches the
            // dataset until the user applies it.
            dialog->setDraft(std::move(parsed.definitions));
        });
    connect(dialog, &ExpressionEditorDialog::exportRequested, dialog,
        [this, dialog] {
            if (!m_hooks.chooseFile) {
                return;
            }
            const QPointer<ExpressionEditorDialog> alive(dialog);
            const auto path = m_hooks.chooseFile(dialog, true);
            if (path.isEmpty() || alive.isNull()) {
                return;
            }
            // The draft, which is what the user is looking at -- exporting
            // only what has been applied would silently drop their edits.
            QSaveFile file(path);
            const auto json = writeExpressionList(dialog->draft());
            if (!file.open(QIODevice::WriteOnly)
                || file.write(json) != json.size() || !file.commit()) {
                dialog->showError(tr("Could not write %1: %2")
                        .arg(QDir::toNativeSeparators(path),
                            file.errorString()),
                    std::nullopt);
                return;
            }
            emit statusMessage(tr("Exported derived fields to %1.")
                    .arg(QDir::toNativeSeparators(path)),
                4000);
        });
    m_dialog = dialog;
    dialog->show();
}

} // namespace amrvis::qt
