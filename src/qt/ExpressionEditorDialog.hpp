#pragma once

#include <amrexplorer/core/DerivedField.hpp>

#include <QDialog>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <optional>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace amrvis::qt {

// The Expression Editor: the list of derived-field definitions, the selected
// one's name and expression, and the New / Delete / Import / Export / Apply
// buttons. Pure presentation -- it validates nothing and knows nothing about
// the open dataset. It edits a *draft* of the list and asks the host to accept
// it (applyRequested), which either replaces the draft with what was accepted
// or shows a reason in place (showError); Import and Export are the host's too,
// since only it can run a file dialog and parse the format.
//
// Names are kept as typed. An empty one is shown as "(unnamed)" in the list
// and refused by the host, which is where every rule about a definition lives.
//
// It shows two things about the open dataset without knowing anything about
// it: the stored field names an expression may use (setStoredFields) and,
// while a definition is being written, why this dataset could not provide it
// (showResolutionWarning). Both are handed in by the host, which resolves the
// draft as it changes -- draftEdited says when that is.
class ExpressionEditorDialog final : public QDialog {
    Q_OBJECT

public:
    ExpressionEditorDialog(
        std::vector<DerivedFieldDefinition> definitions, QWidget* parent);

    // The draft as it stands, which is what applyRequested refers to.
    [[nodiscard]] const std::vector<DerivedFieldDefinition>& draft()
        const noexcept
    {
        return m_draft;
    }
    // Replaces the draft with content the user has not committed -- an
    // import. Selects the row named by `select` when it is still in range,
    // otherwise the first.
    void setDraft(std::vector<DerivedFieldDefinition> definitions,
        std::optional<std::size_t> select = std::nullopt);
    // Replaces the draft with the committed list, which later edits are then
    // measured against: an accepted apply, or a list committed in another
    // window.
    void setCommitted(std::vector<DerivedFieldDefinition> definitions,
        std::optional<std::size_t> select = std::nullopt);
    // Takes the draft as committed without touching the widgets, for a draft
    // that already equals the list someone else committed.
    void markDraftCommitted();
    // Whether the draft has moved since it was last committed. One list is
    // shared by every window, so a commit made elsewhere asks this before
    // replacing what is on screen here.
    [[nodiscard]] bool hasUnappliedEdits() const noexcept
    {
        return m_draft != m_committed;
    }

    // Reports a refusal in place rather than over a second dialog, and selects
    // the definition it belongs to so the user is looking at what failed.
    void showError(
        const QString& message, std::optional<std::size_t> definitionIndex);
    // Confirms an accepted Apply in the same place a refusal appears. Said
    // here rather than in the status bar, which the reload this announces
    // clears as soon as its slices arrive.
    void showApplied(std::size_t count);
    // Says that the shared list was committed in another window and that this
    // editor's own edits were kept rather than replaced by it.
    void showListChangedElsewhere();
    // The row being edited, so a host that replaces the draft with an
    // equivalent list can leave the user where they were.
    [[nodiscard]] std::optional<std::size_t> selectedIndex() const;

    // Whether the user has typed into this definition's name or expression
    // since the draft was last replaced -- by a commit, or by an import.
    //
    // This is what separates a definition the user is writing from one they
    // are merely carrying: a list written for another plotfile, or imported
    // whole, holds definitions this dataset may be unable to provide through
    // no fault of anyone's, and those are committed and greyed out. One being
    // written is a different claim -- that it works on the data in front of
    // them -- and the host holds it to that.
    [[nodiscard]] bool handEdited(std::size_t index) const;

    // The fields the open dataset stores, in its own order, listed beside the
    // expression so they need not be hunted for in the Variable menu. Double
    // -clicking one writes it into the expression at the cursor. Empty hides
    // the list, which is what no open dataset looks like.
    void setStoredFields(const QStringList& names);
    // Why the open dataset could not provide the definition the user is
    // *writing*, as the host's resolution of the draft reports it. An empty
    // message clears it.
    //
    // Advisory, not a refusal: the list is shared by windows showing different
    // data, and a session may open a plotfile of another shape entirely, so a
    // definition that means nothing here is still committed -- and shown
    // greyed in the field list -- rather than blocked. That is also why it is
    // cleared and never recomputed when the selection moves, when a list is
    // imported, or when a dataset opens: only a definition being typed is
    // measured against the data, because only then is the user asking.
    void showResolutionWarning(const QString& message);

signals:
    // The draft is offered for validation; the host accepts it (setDraft, and
    // usually accept()) or refuses it (showError).
    void applyRequested();
    void importRequested();
    void exportRequested();
    // The user typed into the selected definition's name or expression. What
    // the host resolves on to answer with showResolutionWarning. Only hand
    // editing: an import, a row change and a dataset opening all clear the
    // warning instead, for the reason given there.
    void draftEdited();

private:
    void clearError();
    void rebuildList(std::optional<std::size_t> select);
    void showSelected();
    void addDefinition();
    void removeSelected();

    std::vector<DerivedFieldDefinition> m_draft;
    // The draft as it was last committed, which is the whole of what makes an
    // edit unapplied: compared rather than flagged, so typing something and
    // taking it back leaves nothing to protect.
    std::vector<DerivedFieldDefinition> m_committed;
    // One flag per draft row: has the user typed into it. Kept beside m_draft
    // rather than derived from m_committed, because "differs from what was
    // committed" is also true of every row of an imported list, which is the
    // case this has to tell apart. Cleared whenever the draft is replaced.
    std::vector<bool> m_handEdited;
    QListWidget* m_list = nullptr;
    // The stored fields, and the caption over them: both hidden together when
    // there is nothing to list.
    QListWidget* m_fields = nullptr;
    QLabel* m_fieldsCaption = nullptr;
    QLineEdit* m_name = nullptr;
    QPlainTextEdit* m_expression = nullptr;
    QLabel* m_error = nullptr;
    // Kept apart from m_error so "is something wrong?" stays a question the
    // dialog -- and a test -- can answer by looking at one widget.
    QLabel* m_applied = nullptr;
    // Neither a refusal nor a confirmation of this window's own work: a shared
    // list that moved elsewhere. Its own widget for the same reason m_applied
    // is one.
    QLabel* m_notice = nullptr;
    // What this dataset cannot make of the definition being edited. Standing
    // rather than momentary -- it describes the draft, not the last thing
    // done -- so clearError leaves it alone, as it does the notice.
    QLabel* m_warning = nullptr;
    QPushButton* m_remove = nullptr;
    QPushButton* m_apply = nullptr;
    // Set while the widgets are being written from the draft, so the edit
    // signals do not write straight back into it.
    bool m_loading = false;
};

} // namespace amrvis::qt
