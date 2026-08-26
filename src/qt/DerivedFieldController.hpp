#pragma once

#include "DerivedFieldStore.hpp"

#include <amrexplorer/core/DerivedField.hpp>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <string>
#include <functional>
#include <optional>
#include <vector>

class QAction;
class QByteArray;
class QTimer;
class QWidget;

namespace amrvis::qt {

class ExpressionEditorDialog;

// The `amrexplorer-expression-list` JSON the Expression Editor exports and
// imports:
// {"format": "amrexplorer-expression-list", "version": 1,
//  "expressions": [{"name": ..., "expression": ...}, ...]}
//
// Both directions live here rather than in the dialog so the format is
// testable without a widget.
struct ExpressionListFile {
    std::vector<DerivedFieldDefinition> definitions;
    // Empty when the parse succeeded. Nothing is returned in definitions
    // otherwise: an expression list is imported whole or not at all.
    QString error;
};

// An expression as a tooltip: one line whatever its layout, and escaped.
[[nodiscard]] QString escapedExpression(const std::string& expression);
// Escaped text as a tooltip Qt is certain to render as rich text. Without the
// wrapper, an expression holding `<` decides it and one holding only `&` does
// not, so the escapes show up as themselves.
[[nodiscard]] QString richTooltip(const QString& escaped);

[[nodiscard]] ExpressionListFile parseExpressionList(const QByteArray& json);
[[nodiscard]] QByteArray writeExpressionList(
    const std::vector<DerivedFieldDefinition>& definitions);

// The derived-field definitions this window carries: the committed list, the
// Variable menu's Expression Editor action and its modeless dialog, and the
// import/export of an expression list.
//
// The list lives in the session's DerivedFieldStore, shared by every window,
// and is deliberately not persisted: a definition is written against the
// fields of a particular dataset, so one restored into a later session showed
// up against unrelated plotfiles it could only be unavailable for. Export
// writes one to a file for anyone who wants it back.
//
// The list is installed when a dataset is opened, so committing a change means
// asking the host to reopen what is on screen with the new list (the `reload`
// hook, which the host answers the way it answers a sequence frame switch).
// Validation happens here, before any of that, and only of what is wrong
// whatever the data (see apply): a dataset opening with the committed list
// skips what it cannot resolve (DerivedFieldPolicy) rather than refusing to
// open -- a list written for one plotfile must not make another unopenable --
// and the window shows what was skipped greyed out.
class DerivedFieldController final : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        // Empty while a dataset that can take derived fields is open, and
        // otherwise why not, in the words the greyed action shows. A reason
        // rather than a bool because there is more than one: no dataset, a FAB
        // drilled out of a MultiFab, or a server too old for them -- and
        // telling a remote user their dataset is not local would be a lie.
        // Asked on every dataset load, so it answers without copying anything.
        std::function<QString()> unavailableReason;
        // Reopen the open dataset with the committed definitions.
        std::function<void()> reload;
        // Reopen it only if it is not already showing them. What an Apply that
        // changed nothing asks for: a reload that failed leaves the list
        // committed and uninstalled, and the store emits nothing to ask again
        // with, so without this the only ways back are editing the list to
        // something else and back, or reopening the dataset. A window whose
        // session already carries the list does nothing here, which is what
        // keeps a merely redundant Apply from reloading anything.
        std::function<void()> reloadIfMissing;
        // A path to import from (forSaving false) or export to (true); empty
        // cancels. The host runs the file dialog, as it does for palettes.
        std::function<QString(QWidget* parent, bool forSaving)> chooseFile;
        // The names of the fields the open dataset stores, in its own order,
        // for the editor to list beside the expression being written. Empty
        // with no dataset open.
        std::function<QStringList()> storedFieldNames;
        // Resolves `definitions` against the open dataset the way opening it
        // would (DerivedFieldPolicy::Skip) and answers with what could not be
        // installed and why. The host does it because only it holds the
        // dataset; this is asked again whenever the draft changes, so it must
        // not be expensive enough to be felt while typing. Empty where every
        // definition resolves, and where there is no dataset to resolve
        // against -- the editor has nothing to say about either.
        std::function<std::vector<DerivedFieldSkip>(
            const std::vector<DerivedFieldDefinition>&)>
            resolveAgainstOpenDataset;
    };

    // `store` outlives the controller: the session's own, or a test's. Every
    // window's controller shares one, which is how a definition written in one
    // window reaches the others.
    DerivedFieldController(
        Hooks hooks, DerivedFieldStore& store, QObject* parent = nullptr);

    // The Variable menu's "Expression Editor..." action, enabled while the
    // open dataset can take derived fields. Owned by `parent`.
    QAction* createAction(QWidget* parent);
    // Re-derives the action's enablement from the hooks; the host calls it
    // when a dataset opens or closes.
    void refreshAvailability();

    // Whether a dataset that can take derived fields is open. The host asks
    // it when one of its own loads lands: a window counts as unavailable for
    // the whole of an open, so a list committed meanwhile reached every other
    // window but not the session it was opening.
    [[nodiscard]] bool available() const
    {
        return m_hooks.unavailableReason
            && m_hooks.unavailableReason().isEmpty();
    }

    [[nodiscard]] const std::vector<DerivedFieldDefinition>& definitions()
        const noexcept
    {
        return m_store.definitions();
    }

    struct Refusal {
        QString message;
        // The definition the refusal belongs to, when it is about one.
        std::optional<std::size_t> definitionIndex;
        // Whether the user may overrule it. True only for "this dataset
        // cannot provide it", which is a fact about the data rather than
        // about the definition: the list is shared, and one written for the
        // plotfile they are about to open is worth committing. Everything
        // else is wrong wherever it is installed and is not offered.
        bool confirmable = false;
    };

    // Checks the list and, if it holds, commits it to the store -- from which
    // this window and every other one takes it. Returns the refusal otherwise,
    // having changed nothing; a list equal to the committed one changes and
    // reloads nothing.
    //
    // Only what is wrong whatever the data is refused: a name that is empty or
    // used twice, and an expression that does not parse. Whether a *particular*
    // dataset can provide the fields a definition reads is not this list's
    // business -- one list is shared by windows showing different data, so a
    // definition simply does not apply in some of them, and is shown greyed
    // out there rather than refused everywhere.
    //
    // `mustResolveHere` names the definitions the caller vouches for as the
    // user's own writing -- what the editor has been typed into. Those are
    // additionally required to resolve against the open dataset, because
    // writing one is a claim that it works on the data in front of you.
    // Everything else is committed whether this dataset can provide it or
    // not: a list carried from another plotfile, or imported whole, is not
    // that claim, and greying it out in the field list is the whole of what
    // should happen to it.
    [[nodiscard]] std::optional<Refusal> apply(
        std::vector<DerivedFieldDefinition> definitions,
        std::vector<std::size_t> mustResolveHere = {});

    // Opens the editor (or raises it if it is already open) on the committed
    // list. Modeless: the reload an Apply triggers is an ordinary dataset load,
    // so there is nothing a nested event loop would have to be held back from.
    void showEditor(QWidget* parent);

signals:
    void statusMessage(const QString& message, int timeoutMs);

private:
    // Re-reads the store: refreshes an open editor, and asks the host to
    // reload so the change reaches this window's field list too.
    void adoptStoreChange();
    // Resolves the open editor's draft against the open dataset and tells it
    // what this dataset cannot make of the definition being typed. Debounced
    // by m_diagnostics: it answers a keystroke, and resolving a list costs
    // time in its own length squared.
    void refreshDraftDiagnostics();
    // What is wrong with a list whatever the data, which is what apply may
    // refuse outright and what the live warning must not blame a dataset for.
    [[nodiscard]] std::optional<Refusal> definitionFault(
        const std::vector<DerivedFieldDefinition>& definitions) const;
    // The same, for one row in the context of the rows before it. definitionFault
    // answers for the list and so reports only its first fault, which is the
    // wrong question when the caller is asking about the definition someone is
    // typing: a broken row above it would otherwise leave that one's own
    // syntax error to be explained as something the dataset lacks.
    // Whether this definition failed *only* because one written above it did.
    // Holding the user to such a row would refuse what they wrote while naming
    // a field they can see defined one line up, and commit the row that
    // actually broke it without comment.
    //
    // Asked of the resolver rather than reasoned about here: the rows above
    // that this dataset could not provide are made trivially resolvable and
    // the prefix is resolved again. If this row comes back installable, those
    // rows were the whole of its problem; if it still fails, the failure is
    // its own and is the user's to answer for. Guessing from the symbol list
    // cannot tell the two apart -- a row naming both a lost definition and a
    // field that was never there reads as inherited and commits unchecked.
    [[nodiscard]] bool failureIsInherited(
        const std::vector<DerivedFieldDefinition>& definitions,
        std::size_t index,
        const std::vector<DerivedFieldSkip>& skipped) const;
    [[nodiscard]] std::optional<Refusal> definitionFaultAt(
        const std::vector<DerivedFieldDefinition>& definitions,
        std::size_t index) const;

    Hooks m_hooks;
    DerivedFieldStore& m_store;
    QPointer<QAction> m_action;
    QPointer<ExpressionEditorDialog> m_dialog;
    QTimer* m_diagnostics = nullptr;
    // The row the pending diagnostic was armed for, and the draft it was armed
    // against. A timer that outlives the edit it belongs to -- the selection
    // moved, an import replaced the draft, a row above was deleted -- would
    // otherwise answer about a definition the user never touched, which is the
    // one thing the warning must never do. The revision is what makes the row
    // number an identity: deleting a row slides its successor into it, and a
    // successor that had been edited too passes every other guard.
    std::optional<std::size_t> m_diagnosticsRow;
    std::uint64_t m_diagnosticsRevision = 0;
};

} // namespace amrvis::qt
