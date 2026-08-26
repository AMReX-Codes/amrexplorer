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
#include <QTextCursor>
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

// A field name as an expression writes it: bare when it is a plain
// identifier, and in ${...} otherwise -- which is the grammar's own escape for
// a name holding anything else, and the rule the help text states.
QString asSymbol(const QString& name)
{
    const auto plain = !name.isEmpty()
        && (name.front().isLetter() || name.front() == QLatin1Char('_'))
        && std::all_of(name.cbegin(), name.cend(), [](QChar character) {
               return character.isLetterOrNumber()
                   || character == QLatin1Char('_');
           });
    // isLetterOrNumber accepts non-ASCII letters, which the grammar's
    // identifier rule does not, so anything outside ASCII takes the escape.
    const auto ascii = std::all_of(name.cbegin(), name.cend(),
        [](QChar character) { return character.unicode() < 128; });
    return plain && ascii ? name : QStringLiteral("${%1}").arg(name);
}

} // namespace

ExpressionEditorDialog::ExpressionEditorDialog(
    std::vector<DerivedFieldDefinition> definitions, QWidget* parent)
    : QDialog(parent)
    , m_draft(std::move(definitions))
    , m_committed(m_draft)
    , m_baseline(m_draft)
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

    m_applyAnyway = new QPushButton(tr("Apply &anyway"), this);
    m_applyAnyway->setObjectName(QStringLiteral("applyAnywayButton"));
    m_applyAnyway->setVisible(false);

    m_applied = new QLabel(this);
    m_applied->setObjectName(QStringLiteral("expressionApplied"));
    m_applied->setWordWrap(true);
    m_applied->setVisible(false);

    m_notice = new QLabel(this);
    m_notice->setObjectName(QStringLiteral("expressionNotice"));
    m_notice->setWordWrap(true);
    m_notice->setVisible(false);

    m_warning = new QLabel(this);
    m_warning->setObjectName(QStringLiteral("expressionWarning"));
    m_warning->setWordWrap(true);
    m_warning->setVisible(false);
    // Quotes the user's own text, as the error does.
    m_warning->setTextFormat(Qt::PlainText);
    // Not the error's red: this one does not stop anything, and colouring the
    // two alike would say the definition had been refused when it has not.
    m_warning->setStyleSheet(QStringLiteral("QLabel { color: #b8860b; }"));

    m_fieldsCaption = new QLabel(tr("Fields in this dataset"), this);
    m_fields = new QListWidget(this);
    m_fields->setObjectName(QStringLiteral("storedFieldList"));
    m_fields->setSelectionMode(QAbstractItemView::SingleSelection);
    m_fields->setMinimumWidth(160);
    m_fields->setToolTip(
        tr("The fields this dataset stores. Double-click one to write it into "
           "the expression."));

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
    {
        // Left-aligned under the refusal it belongs to, rather than stretched
        // across the column as a label would be.
        auto* row = new QHBoxLayout;
        row->addWidget(m_applyAnyway);
        row->addStretch(1);
        editor->addLayout(row);
    }
    editor->addWidget(m_warning);
    editor->addWidget(m_applied);
    editor->addWidget(m_notice);
    editor->addStretch(1);

    auto* fields = new QVBoxLayout;
    fields->addWidget(m_fieldsCaption);
    fields->addWidget(m_fields, 1);

    auto* columns = new QHBoxLayout;
    columns->addLayout(sidebar);
    columns->addLayout(editor, 1);
    columns->addLayout(fields);
    auto* root = new QVBoxLayout(this);
    root->addLayout(columns, 1);
    root->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, [this] {
        clearError();
        showSelected();
        // Cleared, not recomputed. The warning answers "does what you are
        // writing work here", so it belongs to an edit and not to a row: a
        // definition written against another plotfile is unresolvable here by
        // design, and saying so every time the user looks at it would be
        // nagging about something that is not wrong.
        showResolutionWarning({});
    });
    connect(m_fields, &QListWidget::itemDoubleClicked, this,
        [this](QListWidgetItem* item) {
            if (item == nullptr || !selectedIndex()) {
                return;
            }
            // Into the expression at the cursor, and leave the focus there:
            // the point of the list is to save typing a name, not to take the
            // user out of what they were writing.
            // Separated from whatever the cursor sits after: written
            // straight onto an identifier the two lex as one unknown symbol,
            // which is the same silent corruption as inserting at the front.
            // A space is safe after anything, operators included.
            const auto text = m_expression->toPlainText();
            const auto at = m_expression->textCursor().position();
            const auto before = text.left(at);
            const auto after = text.mid(at);
            auto written = asSymbol(item->text());
            if (!before.isEmpty() && !before.back().isSpace()) {
                written.prepend(QLatin1Char(' '));
            }
            // And on the right: dropped into the middle of an identifier the
            // name would merge with its tail instead of its head, which is the
            // same silent corruption facing the other way.
            if (!after.isEmpty() && !after.front().isSpace()) {
                written.append(QLatin1Char(' '));
            }
            m_expression->insertPlainText(written);
            m_expression->setFocus();
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
        // The refusal was about the draft as it stood; it is not about this
        // one. Left up it would sit in red beside the advisory this edit is
        // about to raise, and its offer would commit rows nothing examined.
        clearError();
        ++m_draftRevision;
        m_list->item(static_cast<int>(*index))
            ->setText(displayName(m_draft[*index]));
        // A rename moves what the definitions after this one can read, so the
        // whole draft is re-resolved, not just this row.
        emit draftEdited();
    });
    connect(m_expression, &QPlainTextEdit::textChanged, this, [this] {
        const auto index = selectedIndex();
        if (m_loading || !index) {
            return;
        }
        m_draft[*index].expression =
            m_expression->toPlainText().trimmed().toStdString();
        clearError();
        ++m_draftRevision;
        emit draftEdited();
    });
    connect(m_apply, &QPushButton::clicked, this, [this] {
        clearError();
        emit applyRequested();
    });
    connect(m_applyAnyway, &QPushButton::clicked, this, [this] {
        // Only for the draft the refusal was computed against. Every edit
        // clears the refusal, so this should be unreachable -- it is the
        // check that makes "should" into "cannot", because what it guards is
        // committing rows the dataset was never asked about.
        const auto offered = m_refusalRevision.has_value()
            && *m_refusalRevision == m_draftRevision;
        clearError();
        if (offered) {
            emit applyAnywayRequested();
        }
    });
    // Explicitly: a QDialogButtonBox parented to a dialog does *not* have its
    // rejected() wired to that dialog's reject() -- probed, not assumed --
    // so without this the Close button does nothing at all.
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    rebuildList(m_draft.empty() ? std::nullopt : std::optional<std::size_t>{0});
    // Wide enough for the three columns at their minimums (180 + 360 + 160)
    // plus the layout's own spacing; at 720 the field list arrived squeezing
    // the expression box rather than sitting beside it.
    resize(900, 440);
}

void ExpressionEditorDialog::setDraft(
    std::vector<DerivedFieldDefinition> definitions,
    std::optional<std::size_t> select)
{
    m_draft = std::move(definitions);
    // None of it is the user's writing yet: an import is carried, and a commit
    // is already answered for. Whatever they change from here is measured
    // against this.
    m_baseline = m_draft;
    ++m_draftRevision;
    clearError();
    rebuildList(select);
    // Explicitly, rather than leaving it to the row change rebuildList makes:
    // an import that lands on the row already selected changes no row at all.
    // Cleared and not recomputed -- an imported list is tolerated whole, and
    // what this dataset cannot provide of it is shown greyed in the field
    // list rather than complained about here.
    showResolutionWarning({});
}

void ExpressionEditorDialog::setStoredFields(const QStringList& names)
{
    m_fields->clear();
    m_fields->addItems(names);
    // Hidden rather than shown empty: an empty box beside the expression
    // reads as "this dataset stores no fields", which is not what no open
    // dataset means.
    const auto any = !names.isEmpty();
    m_fields->setVisible(any);
    m_fieldsCaption->setVisible(any);
}

void ExpressionEditorDialog::clearRefusal()
{
    m_error->clear();
    m_error->setVisible(false);
    m_applyAnyway->setVisible(false);
    m_refusalRevision.reset();
}

void ExpressionEditorDialog::showResolutionWarning(
    const QString& message, bool blocking)
{
    // Red for what Apply will refuse, amber for what only this dataset cannot
    // do. Set with the text rather than once at construction, because the one
    // label carries both and the colour is the whole of what tells them apart.
    m_warning->setStyleSheet(blocking
            ? QStringLiteral("QLabel { color: red; }")
            : QStringLiteral("QLabel { color: #b8860b; }"));
    m_warning->setText(message);
    m_warning->setVisible(!message.isEmpty());
}

void ExpressionEditorDialog::markDraftCommitted()
{
    m_committed = m_draft;
    // As setDraft does for the path that replaces the rows: what was the
    // user's own writing has been answered for, and holding it to this
    // dataset a second time would refuse a list that is already installed.
    m_baseline = m_draft;
    // The draft's meaning moved even though its rows did not, and the
    // revision is what an asynchronous reader compares against.
    ++m_draftRevision;
    // Nothing standing about the old reading survives it: the refusal, its
    // offer, and the advisory were all about a draft that was the user's.
    clearError();
    showResolutionWarning({});
    m_notice->clear();
    m_notice->setVisible(false);
}

void ExpressionEditorDialog::setCommitted(
    std::vector<DerivedFieldDefinition> definitions,
    std::optional<std::size_t> select)
{
    setDraft(std::move(definitions), select);
    m_committed = m_draft;
    // The draft and the shared list agree again, which is the only thing that
    // ends the conflict the notice is about -- whether that came of adopting
    // the change or of committing over it.
    m_notice->clear();
    m_notice->setVisible(false);
}

void ExpressionEditorDialog::showError(const QString& message,
    std::optional<std::size_t> definitionIndex, bool offerAnyway)
{
    // Before anything is shown: selecting a different row runs the selection
    // handler, which clears the error box -- so a refusal set up first would
    // be taken down again on its way in, and the offer with it.
    if (definitionIndex && *definitionIndex < m_draft.size()) {
        m_list->setCurrentRow(static_cast<int>(*definitionIndex));
    }
    m_applied->clear();
    m_applied->setVisible(false);
    m_error->setText(message);
    m_error->setVisible(true);
    m_applyAnyway->setVisible(offerAnyway);
    m_refusalRevision = m_draftRevision;
    // The refusal supersedes the advisory: they carry the same sentence when
    // Apply refuses what the warning was about, and showing it twice -- once
    // in red, once in amber -- says the advisory did not stop anything at the
    // moment it did.
    showResolutionWarning({});
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
    m_notice->setText(
        tr("The derived fields were changed in another window. The edits here "
           "are unapplied and have been kept; Apply replaces the shared list "
           "with them."));
    m_notice->setVisible(true);
}

void ExpressionEditorDialog::clearError()
{
    // Not the notice: a peer's commit is a standing conflict, not a message
    // about the last thing done here. Selecting another row, adding a
    // definition or pressing Apply all come through here, and any of them
    // taking the warning away would leave a draft that still overwrites the
    // shared list with nothing on screen saying so. setCommitted ends it.
    clearRefusal();
    // And the confirmation, which every caller of this one wants gone: they
    // are the acts that make "Applied 3 derived fields." describe something
    // the user has since moved on from. clearRefusal is the narrower door,
    // for a host taking down a verdict without touching what was applied --
    // the reload an Apply starts comes back through here, and this label
    // exists because the status bar could not survive it either.
    m_applied->clear();
    m_applied->setVisible(false);
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
        // setPlainText parks the cursor at the start of the document, so a
        // field written in from the list would go *before* what is already
        // there -- silently, and reading as one unknown symbol. The end is
        // where someone who just selected a row would carry on.
        m_expression->moveCursor(QTextCursor::End);
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
    // Nothing to carry: a row that did not arrive from anywhere is the user's
    // the moment they write in it.
    m_baseline.push_back({});
    ++m_draftRevision;
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
    m_baseline.erase(
        m_baseline.begin() + static_cast<std::ptrdiff_t>(*index));
    ++m_draftRevision;
    // Deleting the last definition leaves nothing selected, and rebuildList
    // reaches that by writing -1 over a current row the clear() already set
    // to -1 -- no row changes, so the selection path that would have taken
    // the warning down never runs. Said here rather than relied on there.
    showResolutionWarning({});
    rebuildList(*index == 0 ? std::optional<std::size_t>{0}
                            : std::optional<std::size_t>{*index - 1});
}

bool ExpressionEditorDialog::handEdited(std::size_t index) const
{
    if (index >= m_draft.size()) {
        return false;
    }
    // Computed rather than remembered: there is no flag to fall out of step
    // with the rows, and putting a definition back the way it arrived takes
    // the claim back with it.
    return index >= m_baseline.size() || m_draft[index] != m_baseline[index];
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
