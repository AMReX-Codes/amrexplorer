#pragma once

#include "DerivedFieldStore.hpp"

#include <amrexplorer/core/DerivedField.hpp>

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstddef>
#include <string>
#include <functional>
#include <optional>
#include <vector>

class QAction;
class QByteArray;
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
        // A path to import from (forSaving false) or export to (true); empty
        // cancels. The host runs the file dialog, as it does for palettes.
        std::function<QString(QWidget* parent, bool forSaving)> chooseFile;
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
    [[nodiscard]] std::optional<Refusal> apply(
        std::vector<DerivedFieldDefinition> definitions);

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

    Hooks m_hooks;
    DerivedFieldStore& m_store;
    QPointer<QAction> m_action;
    QPointer<ExpressionEditorDialog> m_dialog;
};

} // namespace amrvis::qt
