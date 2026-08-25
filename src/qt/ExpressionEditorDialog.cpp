#include "ExpressionEditorDialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace amrvis::qt {
namespace {

QString displayName(const DerivedFieldDefinition& definition)
{
    return definition.name.empty()
        ? ExpressionEditorDialog::tr("(unnamed)")
        : QString::fromStdString(definition.name);
}

} // namespace

ExpressionEditorDialog::ExpressionEditorDialog(
    std::vector<DerivedFieldDefinition> definitions, QWidget* parent)
    : QDialog(parent)
    , m_draft(std::move(definitions))
    , m_committed(m_draft)
{
    setObjectName(QStringLiteral("expressionEditor"));
    setWindowTitle(tr("Expression Editor"));

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("expressionList"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setMinimumWidth(180);

    auto* add = new QPushButton(tr("&New"), this);
    add->setObjectName(QStringLiteral("newExpressionButton"));
    m_remove = new QPushButton(tr("&Delete"), this);
    m_remove->setObjectName(QStringLiteral("deleteExpressionButton"));
    auto* importButton = new QPushButton(tr("&Import..."), this);
    importButton->setObjectName(QStringLiteral("importExpressionsButton"));
    auto* exportButton = new QPushButton(tr("E&xport..."), this);
    exportButton->setObjectName(QStringLiteral("exportExpressionsButton"));

    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("expressionName"));
    m_name->setPlaceholderText(tr("e.g. speed"));
    m_expression = new QPlainTextEdit(this);
    m_expression->setObjectName(QStringLiteral("expressionSource"));
    m_expression->setPlaceholderText(
        tr("e.g. sqrt(x_velocity**2 + y_velocity**2)"));
    m_expression->setTabChangesFocus(true);
    m_expression->setMinimumWidth(360);
    m_expression->setMaximumHeight(110);

    auto* help = new QLabel(
        tr("Algebra over the dataset's fields with + - * / and ** (or "
           "pow(a,b)), and abs, sqrt, exp, log, exp10, log10. Write a name "
           "that is not a plain identifier as ${name}. x, y and z are the "
           "sample coordinates. An expression may also use the fields defined "
           "above it in the list."),
        this);
    help->setObjectName(QStringLiteral("expressionHelp"));
    help->setWordWrap(true);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("expressionError"));
    m_error->setWordWrap(true);
    m_error->setVisible(false);
    // The message quotes what the user typed, and a QLabel left on AutoText
    // renders anything Qt takes for markup: an expression holding `<` would be
    // shown with a piece missing.
    m_error->setTextFormat(Qt::PlainText);
    // As SetContoursDialog styles its own warning.
    m_error->setStyleSheet(QStringLiteral("QLabel { color: red; }"));

    m_applied = new QLabel(this);
    m_applied->setObjectName(QStringLiteral("expressionApplied"));
    m_applied->setWordWrap(true);
    m_applied->setVisible(false);

    m_notice = new QLabel(this);
    m_notice->setObjectName(QStringLiteral("expressionNotice"));
    m_notice->setWordWrap(true);
    m_notice->setVisible(false);

    auto* buttons = new QDialogButtonBox(this);
    m_apply = buttons->addButton(QDialogButtonBox::Apply);
    m_apply->setObjectName(QStringLiteral("applyExpressionsButton"));
    auto* close = buttons->addButton(QDialogButtonBox::Close);
    close->setObjectName(QStringLiteral("closeExpressionsButton"));

    auto* sidebarButtons = new QHBoxLayout;
    sidebarButtons->addWidget(add);
    sidebarButtons->addWidget(m_remove);
    auto* fileButtons = new QHBoxLayout;
    fileButtons->addWidget(importButton);
    fileButtons->addWidget(exportButton);
    auto* sidebar = new QVBoxLayout;
    sidebar->addWidget(new QLabel(tr("Derived fields"), this));
    sidebar->addWidget(m_list, 1);
    sidebar->addLayout(sidebarButtons);
    sidebar->addLayout(fileButtons);

    auto* form = new QFormLayout;
    form->addRow(tr("&Name:"), m_name);
    form->addRow(tr("&Expression:"), m_expression);
    auto* editor = new QVBoxLayout;
    editor->addLayout(form);
    editor->addWidget(help);
    editor->addWidget(m_error);
    editor->addWidget(m_applied);
    editor->addWidget(m_notice);
    editor->addStretch(1);

    auto* columns = new QHBoxLayout;
    columns->addLayout(sidebar);
    columns->addLayout(editor, 1);
    auto* root = new QVBoxLayout(this);
    root->addLayout(columns, 1);
    root->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, [this] {
        clearError();
        showSelected();
    });
    connect(add, &QPushButton::clicked, this, [this] { addDefinition(); });
    connect(
        m_remove, &QPushButton::clicked, this, [this] { removeSelected(); });
    connect(importButton, &QPushButton::clicked, this,
        [this] { emit importRequested(); });
    connect(exportButton, &QPushButton::clicked, this,
        [this] { emit exportRequested(); });
    connect(m_name, &QLineEdit::textChanged, this, [this](const QString& text) {
        const auto index = selectedIndex();
        if (m_loading || !index) {
            return;
        }
        m_draft[*index].name = text.trimmed().toStdString();
        m_list->item(static_cast<int>(*index))
            ->setText(displayName(m_draft[*index]));
    });
    connect(m_expression, &QPlainTextEdit::textChanged, this, [this] {
        const auto index = selectedIndex();
        if (m_loading || !index) {
            return;
        }
        m_draft[*index].expression =
            m_expression->toPlainText().trimmed().toStdString();
    });
    connect(m_apply, &QPushButton::clicked, this, [this] {
        clearError();
        emit applyRequested();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);

    rebuildList(m_draft.empty() ? std::nullopt : std::optional<std::size_t>{0});
    resize(720, 420);
}

void ExpressionEditorDialog::setDraft(
    std::vector<DerivedFieldDefinition> definitions,
    std::optional<std::size_t> select)
{
    m_draft = std::move(definitions);
    clearError();
    rebuildList(select);
}

void ExpressionEditorDialog::setCommitted(
    std::vector<DerivedFieldDefinition> definitions,
    std::optional<std::size_t> select)
{
    setDraft(std::move(definitions), select);
    m_committed = m_draft;
}

void ExpressionEditorDialog::showError(
    const QString& message, std::optional<std::size_t> definitionIndex)
{
    m_applied->clear();
    m_applied->setVisible(false);
    if (definitionIndex && *definitionIndex < m_draft.size()) {
        m_list->setCurrentRow(static_cast<int>(*definitionIndex));
    }
    m_error->setText(message);
    m_error->setVisible(true);
}

void ExpressionEditorDialog::showApplied(std::size_t count)
{
    clearError();
    m_applied->setText(count == 0
            ? tr("Applied: no derived fields.")
            : tr("Applied %n derived field(s).", nullptr,
                  static_cast<int>(count)));
    m_applied->setVisible(true);
}

void ExpressionEditorDialog::showListChangedElsewhere()
{
    clearError();
    m_notice->setText(
        tr("The derived fields were changed in another window. The edits here "
           "are unapplied and have been kept; Apply replaces the shared list "
           "with them."));
    m_notice->setVisible(true);
}

void ExpressionEditorDialog::clearError()
{
    m_error->clear();
    m_error->setVisible(false);
    m_applied->clear();
    m_applied->setVisible(false);
    m_notice->clear();
    m_notice->setVisible(false);
}

void ExpressionEditorDialog::rebuildList(std::optional<std::size_t> select)
{
    {
        const QSignalBlocker blocker(m_list);
        m_list->clear();
        for (const auto& definition : m_draft) {
            m_list->addItem(displayName(definition));
        }
    }
    const auto row = select && *select < m_draft.size()
        ? static_cast<int>(*select)
        : (m_draft.empty() ? -1 : 0);
    // Unblocked, so the row change loads the selection through the same path
    // every other selection change takes.
    m_list->setCurrentRow(row);
    if (row < 0) {
        showSelected();
    }
}

void ExpressionEditorDialog::showSelected()
{
    const auto index = selectedIndex();
    m_loading = true;
    if (index) {
        m_name->setText(QString::fromStdString(m_draft[*index].name));
        m_expression->setPlainText(
            QString::fromStdString(m_draft[*index].expression));
    } else {
        m_name->clear();
        m_expression->clear();
    }
    m_loading = false;
    m_name->setEnabled(index.has_value());
    m_expression->setEnabled(index.has_value());
    m_remove->setEnabled(index.has_value());
}

void ExpressionEditorDialog::addDefinition()
{
    clearError();
    m_draft.push_back({});
    rebuildList(m_draft.size() - 1);
    m_name->setFocus();
}

void ExpressionEditorDialog::removeSelected()
{
    const auto index = selectedIndex();
    if (!index) {
        return;
    }
    clearError();
    m_draft.erase(m_draft.begin() + static_cast<std::ptrdiff_t>(*index));
    rebuildList(*index == 0 ? std::optional<std::size_t>{0}
                            : std::optional<std::size_t>{*index - 1});
}

std::optional<std::size_t> ExpressionEditorDialog::selectedIndex() const
{
    const auto row = m_list->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= m_draft.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(row);
}

} // namespace amrvis::qt
