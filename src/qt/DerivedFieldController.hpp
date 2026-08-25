#pragma once

#include "DerivedFieldStore.hpp"

#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/core/Metadata.hpp>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <cstddef>
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

[[nodiscard]] ExpressionListFile parseExpressionList(const QByteArray& json);
[[nodiscard]] QByteArray writeExpressionList(
    const std::vector<DerivedFieldDefinition>& definitions);

// The derived-field definitions this window carries: the committed list, the
// Variable menu's Expression Editor action and its modeless dialog, and the
// import/export of an expression list.
//
// The list lives as long as the window and is deliberately not persisted. A
// definition is written against the fields of a particular dataset, so one
// restored into a later session showed up against unrelated plotfiles, where
// it could only be reported as unavailable. Export writes one to a file for
// anyone who wants it back.
//
// The list is installed when a dataset is opened, so committing a change means
// asking the host to reopen what is on screen with the new list (the `reload`
// hook, which the host answers the way it answers a sequence frame switch).
// Validation happens here, before any of that: an editor's Apply is checked
// strictly against the open dataset's stored fields so the user is told which
// definition is wrong and why, while a dataset opening with the committed list
// skips what it cannot resolve (DerivedFieldPolicy) rather than refusing to
// open -- a list written for one plotfile must not make another unopenable.
class DerivedFieldController final : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        // Whether a dataset that can take derived fields is open. Asked on
        // every dataset load, which is why it is not storedMetadata's
        // has_value(): that copies a whole field and block list to answer a
        // question about one pointer.
        std::function<bool()> available;
        // The fields a definition may read: the open dataset's metadata with
        // the derived fields removed, or nullopt when none is open. Asked only
        // when a list is validated.
        std::function<std::optional<DatasetMetadata>()> storedMetadata;
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
