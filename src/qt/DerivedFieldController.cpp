#include "DerivedFieldController.hpp"

#include "ExpressionEditorDialog.hpp"

#include <QAction>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QPointer>
#include <QSaveFile>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace amrvis::qt {
namespace {

constexpr auto kExpressionListFormat = "amrexplorer-expression-list";
constexpr int kExpressionListVersion = 1;

} // namespace

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

    std::vector<DerivedFieldDefinition> definitions;
    const auto array = entries.toArray();
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
    // Nothing to replace in the window whose own Apply made the change: its
    // draft is already exactly this list.
    if (m_dialog && m_dialog->draft() != m_store.definitions()) {
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
    // a remote session, or a FAB -- has nothing to reload, and will not show
    // them either way.
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
    const auto usable = available();
    if (m_action) {
        m_action->setEnabled(usable);
        m_action->setToolTip(usable
                ? QString()
                : tr("Derived fields need a local dataset."));
    }
    // An editor left open over a dataset that has gone (or a remote one that
    // cannot take derived fields) would apply into nothing.
    if (!usable && m_dialog) {
        // Cleared here rather than left to the deleteLater that close()
        // schedules: until that is delivered the pointer is still set, and a
        // showEditor before then would raise the dying dialog instead of
        // opening one.
        auto* dialog = m_dialog.data();
        m_dialog = nullptr;
        dialog->close();
    }
}

std::optional<DerivedFieldController::Refusal> DerivedFieldController::apply(
    std::vector<DerivedFieldDefinition> definitions)
{
    // The same question the action's enablement asks, not a weaker one: the
    // editor is modeless, so the window can enter a state it is disabled for
    // (a FAB drill-down) while it is still open in front of the user.
    if (!available()) {
        return Refusal{tr("No dataset that can take derived fields is open."),
            std::nullopt};
    }

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
        const auto& definition = definitions[index];
        if (definition.name.empty()) {
            return Refusal{tr("A derived field needs a name."), index};
        }
        const auto upto =
            definitions.begin() + static_cast<std::ptrdiff_t>(index);
        if (std::find_if(definitions.begin(), upto,
                [&definition](const DerivedFieldDefinition& earlier) {
                    return earlier.name == definition.name;
                })
            != upto) {
            return Refusal{
                tr("Another derived field is already called \"%1\".")
                    .arg(QString::fromStdString(definition.name)),
                index};
        }
        if (definition.expression.empty()) {
            return Refusal{tr("A derived field needs an expression."), index};
        }
        try {
            static_cast<void>(
                CompiledExpression::compile(definition.expression));
        } catch (const ExpressionError& error) {
            return Refusal{QString::fromUtf8(error.what()), index};
        }
    }

    // set() is what reloads -- here and in every other window -- and does
    // nothing at all when the list has not moved.
    m_store.set(std::move(definitions));
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
    connect(dialog, &ExpressionEditorDialog::applyRequested, dialog,
        [this, dialog] {
            const auto refusal = apply(dialog->draft());
            if (refusal) {
                dialog->showError(
                    refusal->message, refusal->definitionIndex);
                return;
            }
            // The draft is now the committed list, which is what later
            // edits are measured against -- and what a change arriving from
            // another window may replace without asking. Same content, so
            // nothing on screen moves but the row the user was editing, which
            // is kept.
            dialog->setCommitted(
                m_store.definitions(), dialog->selectedIndex());
            // Not a status message: apply() has already started the reload,
            // and showSlice clears the status bar when its slices land. The
            // editor is in front of the user anyway, so it says so itself.
            dialog->showApplied(m_store.definitions().size());
        });
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
