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
#include <QWidget>

#include <algorithm>
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
}

std::optional<DerivedFieldController::Refusal> DerivedFieldController::apply(
    std::vector<DerivedFieldDefinition> definitions)
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
        return Refusal{reason, std::nullopt};
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

    // What is wrong with the list as a list, whatever the data: too many
    // fields read at once, or a dependency chain deeper than evaluation may
    // recurse. Left to installation these would be skipped per dataset, so
    // every window would grey the same row and say why, over a fault no data
    // could fix.
    if (const auto fault = validateDerivedFieldGraph(definitions)) {
        return Refusal{QString::fromStdString(fault->message),
            fault->definitionIndex};
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
