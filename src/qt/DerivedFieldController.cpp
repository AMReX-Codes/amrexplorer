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
#include <QSaveFile>
#include <QSettings>
#include <QWidget>

#include <utility>

namespace amrvis::qt {
namespace {

constexpr auto kExpressionListFormat = "amrexplorer-expression-list";
constexpr int kExpressionListVersion = 1;

QString settingsKey()
{
    return QStringLiteral("derivedFields/list");
}

} // namespace

ExpressionListFile parseExpressionList(const QByteArray& json)
{
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {{},
            DerivedFieldController::tr("not a valid expression-list JSON "
                                       "file: %1")
                .arg(parseError.errorString())};
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

DerivedFieldController::DerivedFieldController(Hooks hooks, QObject* parent)
    : QObject(parent)
    , m_hooks(std::move(hooks))
{
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
    const auto available = m_hooks.available && m_hooks.available();
    if (m_action) {
        m_action->setEnabled(available);
        m_action->setToolTip(available
                ? QString()
                : tr("Derived fields need a local dataset."));
    }
    // An editor left open over a dataset that has gone (or a remote one that
    // cannot take derived fields) would apply into nothing.
    if (!available && m_dialog) {
        m_dialog->close();
    }
}

std::optional<DerivedFieldController::Refusal> DerivedFieldController::apply(
    std::vector<DerivedFieldDefinition> definitions)
{
    auto stored = m_hooks.storedMetadata ? m_hooks.storedMetadata()
                                         : std::nullopt;
    if (!stored) {
        return Refusal{tr("No dataset that can take derived fields is open."),
            std::nullopt};
    }
    try {
        // Strict, on a copy: the user is being told whether what they typed
        // works, so the first problem is the answer -- and the metadata this
        // validates against is thrown away either way.
        static_cast<void>(installDerivedFields(
            *stored, definitions, DerivedFieldPolicy::Strict));
    } catch (const DerivedFieldError& error) {
        return Refusal{QString::fromUtf8(error.what()),
            error.definitionIndex()};
    }

    m_definitions = std::move(definitions);
    if (m_hooks.settings) {
        if (auto settings = m_hooks.settings()) {
            save(*settings);
        }
    }
    emit definitionsChanged();
    if (m_hooks.reload) {
        m_hooks.reload();
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
    auto* dialog = new ExpressionEditorDialog(m_definitions, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &ExpressionEditorDialog::applyRequested, dialog,
        [this, dialog] {
            const auto refusal = apply(dialog->draft());
            if (refusal) {
                dialog->showError(
                    refusal->message, refusal->definitionIndex);
                return;
            }
            // The committed list is what the editor should now be editing:
            // apply() may have trimmed names, and the draft must not drift
            // from what the dataset was reopened with.
            dialog->setDraft(m_definitions);
            emit statusMessage(
                m_definitions.empty()
                    ? tr("Derived fields cleared.")
                    : tr("Applied %n derived field(s).", nullptr,
                          static_cast<int>(m_definitions.size())),
                4000);
        });
    connect(dialog, &ExpressionEditorDialog::importRequested, dialog,
        [this, dialog] {
            if (!m_hooks.chooseFile) {
                return;
            }
            const auto path = m_hooks.chooseFile(dialog, false);
            if (path.isEmpty()) {
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
            auto path = m_hooks.chooseFile(dialog, true);
            if (path.isEmpty()) {
                return;
            }
            if (QFileInfo(path).suffix().isEmpty()) {
                path += QStringLiteral(".json");
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

QString DerivedFieldController::skippedReport(
    const std::vector<DerivedFieldSkip>& skipped) const
{
    if (skipped.empty()) {
        return {};
    }
    QStringList names;
    for (const auto& entry : skipped) {
        names.append(entry.name.empty()
                ? tr("(unnamed)")
                : QString::fromStdString(entry.name));
    }
    // One reason in full, the first: with several skips they are usually the
    // same missing field, and the status bar has room for one.
    const auto reason = QString::fromStdString(skipped.front().reason);
    return skipped.size() == 1
        ? tr("Derived field \"%1\" is unavailable for this dataset: %2")
              .arg(names.front(), reason)
        : tr("%1 derived fields are unavailable for this dataset: %2 (%3)")
              .arg(skipped.size())
              .arg(names.join(QStringLiteral(", ")), reason);
}

void DerivedFieldController::restore(const QSettings& settings)
{
    const auto stored = settings.value(settingsKey()).toString();
    if (stored.isEmpty()) {
        return;
    }
    auto parsed = parseExpressionList(stored.toUtf8());
    if (!parsed.error.isEmpty()) {
        return;
    }
    m_definitions = std::move(parsed.definitions);
}

void DerivedFieldController::save(QSettings& settings) const
{
    if (m_definitions.empty()) {
        settings.remove(settingsKey());
        return;
    }
    settings.setValue(settingsKey(),
        QString::fromUtf8(writeExpressionList(m_definitions)));
}

} // namespace amrvis::qt
