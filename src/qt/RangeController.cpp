#include "RangeController.hpp"

#include "ScientificDoubleSpinBox.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QToolBar>

#include <limits>
#include <utility>

namespace amrvis::qt {

RangeController::RangeController(QObject* parent)
    : QObject(parent)
{
}

void RangeController::createToolbarWidgets(QToolBar* toolbar)
{
    m_mode = new QComboBox(toolbar);
    m_mode->setObjectName(QStringLiteral("rangeModeSelector"));
    m_mode->addItem(tr("File"), static_cast<int>(RangeMode::File));
    m_mode->addItem(tr("Level"), static_cast<int>(RangeMode::Level));
    m_mode->addItem(tr("Visible"), static_cast<int>(RangeMode::Visible));
    m_mode->addItem(tr("User"), static_cast<int>(RangeMode::User));
    toolbar->addWidget(m_mode);
    m_minimum = new ScientificDoubleSpinBox(toolbar);
    m_maximum = new ScientificDoubleSpinBox(toolbar);
    for (auto* range : {m_minimum, m_maximum}) {
        range->setRange(-std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max());
        range->setMinimumWidth(110);
        range->setEnabled(false);
        toolbar->addWidget(range);
    }
    m_minimum->setPrefix(tr("min "));
    m_maximum->setPrefix(tr("max "));
    m_maximum->setValue(1.0);
    // Separate the Range group (mode + min/max) from Log, matching the
    // per-group separators on the Slice Controls toolbar.
    toolbar->addSeparator();
    m_logarithmic = new QCheckBox(tr("Log"), toolbar);
    toolbar->addWidget(m_logarithmic);
    m_mode->setEnabled(false);
    m_logarithmic->setEnabled(false);

    connect(m_mode, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int) {
            updateUserRangeEnabled();
            emit modeChanged();
        });
    connect(m_minimum, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
        [this](double) {
            if (mode() == RangeMode::User) {
                emit userRangeChanged();
            }
        });
    connect(m_maximum, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
        [this](double) {
            if (mode() == RangeMode::User) {
                emit userRangeChanged();
            }
        });
    connect(m_logarithmic, &QCheckBox::toggled, this,
        [this](bool) { emit logarithmicChanged(); });
}

RangeController::Selection RangeController::selection() const
{
    Selection selection;
    selection.mode = mode();
    if (selection.mode == RangeMode::User) {
        selection.userRange = std::pair{m_minimum->value(), m_maximum->value()};
    }
    selection.logarithmic = logarithmic();
    return selection;
}

RangeMode RangeController::mode() const
{
    return static_cast<RangeMode>(m_mode->currentData().toInt());
}

bool RangeController::logarithmic() const
{
    return m_logarithmic->isChecked();
}

void RangeController::setMode(RangeMode mode)
{
    const QSignalBlocker blocker(m_mode);
    m_mode->setCurrentIndex(m_mode->findData(static_cast<int>(mode)));
}

void RangeController::updateUserRangeEnabled()
{
    const bool user = mode() == RangeMode::User;
    m_minimum->setEnabled(user && m_controlsReady);
    m_maximum->setEnabled(user && m_controlsReady);
}

void RangeController::setSelection(const Selection& selection)
{
    const QSignalBlocker minBlocker(m_minimum);
    const QSignalBlocker maxBlocker(m_maximum);
    const QSignalBlocker logBlocker(m_logarithmic);
    setMode(selection.mode);
    m_logarithmic->setChecked(selection.logarithmic);
    if (selection.userRange) {
        m_minimum->setValue(selection.userRange->first);
        m_maximum->setValue(selection.userRange->second);
    }
    updateUserRangeEnabled();
}

void RangeController::showDisplayRange(double minimum, double maximum)
{
    const QSignalBlocker minBlocker(m_minimum);
    const QSignalBlocker maxBlocker(m_maximum);
    m_minimum->setValue(minimum);
    m_maximum->setValue(maximum);
}

void RangeController::showLogarithmic(bool logarithmic)
{
    if (m_logarithmic->isChecked() != logarithmic) {
        const QSignalBlocker blocker(m_logarithmic);
        m_logarithmic->setChecked(logarithmic);
    }
}

void RangeController::setControlsReady(bool ready)
{
    m_controlsReady = ready;
    m_mode->setEnabled(ready);
    m_logarithmic->setEnabled(ready);
    updateUserRangeEnabled();
}

void RangeController::setNumberFormat(const QString& format)
{
    m_minimum->setNumberFormat(format);
    m_maximum->setNumberFormat(format);
}

void RangeController::switchField(const QString& field)
{
    if (field == m_trackedField) {
        return;
    }
    // Nothing tracked yet -- before the first setTrackedField, or where a
    // selection could not come to rest on a field -- so there is no field
    // whose range this is. Committing would file the widgets' current state
    // under the empty name and hand it back to whatever asked for it next.
    if (!m_trackedField.isEmpty()) {
        commitFieldRange(m_trackedField);
    }
    m_trackedField = field;
    applyFieldRange(field);
}

void RangeController::commitFieldRange(const QString& field)
{
    FieldRange range;
    range.mode = mode();
    if (range.mode == RangeMode::User) {
        range.userRange = std::pair{m_minimum->value(), m_maximum->value()};
    }
    m_fieldRanges[field] = std::move(range);
}

void RangeController::applyFieldRange(const QString& field)
{
    const auto it = m_fieldRanges.constFind(field);
    const auto range = it != m_fieldRanges.constEnd() ? it.value() : FieldRange{};
    {
        const QSignalBlocker minBlocker(m_minimum);
        const QSignalBlocker maxBlocker(m_maximum);
        setMode(range.mode);
        if (range.userRange.has_value()) {
            m_minimum->setValue(range.userRange->first);
            m_maximum->setValue(range.userRange->second);
        }
    }
    updateUserRangeEnabled();
}

void RangeController::reset()
{
    m_fieldRanges.clear();
    m_trackedField.clear();
    const QSignalBlocker minBlocker(m_minimum);
    const QSignalBlocker maxBlocker(m_maximum);
    setMode(RangeMode::File);
    m_minimum->setValue(0.0);
    m_maximum->setValue(1.0);
    m_minimum->setEnabled(false);
    m_maximum->setEnabled(false);
}

void RangeController::updateAvailability(
    const Availability& availability, const QString& field)
{
    auto* model = qobject_cast<QStandardItemModel*>(m_mode->model());
    if (model == nullptr) {
        return;
    }
    const auto unavailableText = tr(
        "Unavailable because this data does not provide complete range statistics.");
    const auto setAvailable = [&](RangeMode mode, bool available) {
        const auto index = m_mode->findData(static_cast<int>(mode));
        if (index < 0) {
            return;
        }
        if (auto* item = model->item(index)) {
            item->setEnabled(available);
            item->setToolTip(available ? QString() : unavailableText);
        }
    };
    setAvailable(RangeMode::File, availability.file);
    setAvailable(RangeMode::Level, availability.level);

    const auto current = mode();
    const auto currentAvailable = (current != RangeMode::File || availability.file)
        && (current != RangeMode::Level || availability.level);
    if (currentAvailable) {
        return;
    }
    setMode(RangeMode::Visible);
    m_minimum->setEnabled(false);
    m_maximum->setEnabled(false);
    m_fieldRanges[field].mode = RangeMode::Visible;
    emit statusMessage(
        tr("Metadata range unavailable; using the visible-data range."), 0);
}

} // namespace amrvis::qt
