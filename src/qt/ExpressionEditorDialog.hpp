#pragma once

#include <amrexplorer/core/DerivedField.hpp>

#include <QDialog>
#include <QString>

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
    // Replaces the draft: an accepted apply, or an import. Selects the row
    // named by `select` when it is still in range, otherwise the first.
    void setDraft(std::vector<DerivedFieldDefinition> definitions,
        std::optional<std::size_t> select = std::nullopt);

    // Reports a refusal in place rather than over a second dialog, and selects
    // the definition it belongs to so the user is looking at what failed.
    void showError(
        const QString& message, std::optional<std::size_t> definitionIndex);
    // Confirms an accepted Apply in the same place a refusal appears. Said
    // here rather than in the status bar, which the reload this announces
    // clears as soon as its slices arrive.
    void showApplied(std::size_t count);
    // The row being edited, so a host that replaces the draft with an
    // equivalent list can leave the user where they were.
    [[nodiscard]] std::optional<std::size_t> selectedIndex() const;

signals:
    // The draft is offered for validation; the host accepts it (setDraft, and
    // usually accept()) or refuses it (showError).
    void applyRequested();
    void importRequested();
    void exportRequested();

private:
    void clearError();
    void rebuildList(std::optional<std::size_t> select);
    void showSelected();
    void addDefinition();
    void removeSelected();

    std::vector<DerivedFieldDefinition> m_draft;
    QListWidget* m_list = nullptr;
    QLineEdit* m_name = nullptr;
    QPlainTextEdit* m_expression = nullptr;
    QLabel* m_error = nullptr;
    // Kept apart from m_error so "is something wrong?" stays a question the
    // dialog -- and a test -- can answer by looking at one widget.
    QLabel* m_applied = nullptr;
    QPushButton* m_remove = nullptr;
    QPushButton* m_apply = nullptr;
    // Set while the widgets are being written from the draft, so the edit
    // signals do not write straight back into it.
    bool m_loading = false;
};

} // namespace amrvis::qt
