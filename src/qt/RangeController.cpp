#include "RangeController.hpp"

#include "ScientificDoubleSpinBox.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QToolBar>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace amrvis::qt {
namespace {

double defaultSymmetricLogThreshold(double minimum, double maximum) noexcept
{
    const auto magnitude = std::max(std::abs(minimum), std::abs(maximum));
    if (!(magnitude > 0.0) || !std::isfinite(magnitude)) {
        return 1.0;
    }
    const auto threshold = std::pow(
        10.0, std::floor(std::log10(magnitude)) - 2.0);
    return threshold > 0.0 && std::isfinite(threshold)
        ? threshold : std::numeric_limits<double>::min();
}

} // namespace

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
    m_logarithmic->setObjectName(QStringLiteral("logarithmicScale"));
    toolbar->addWidget(m_logarithmic);
    m_symmetricLogarithmic = new QCheckBox(tr("Symlog"), toolbar);
    m_symmetricLogarithmic->setObjectName(
        QStringLiteral("symmetricLogarithmicScale"));
    toolbar->addWidget(m_symmetricLogarithmic);
    m_linearThreshold = new ScientificDoubleSpinBox(toolbar);
    m_linearThreshold->setObjectName(QStringLiteral("symlogLinearThreshold"));
    m_linearThreshold->setPrefix(tr("linthresh "));
    m_linearThreshold->setRange(std::numeric_limits<double>::min(),
        std::numeric_limits<double>::max());
    m_linearThreshold->setValue(1.0);
    m_linearThreshold->setMinimumWidth(140);
    toolbar->addWidget(m_linearThreshold);
    m_linearThreshold->setVisible(false);
    m_mode->setEnabled(false);
    m_logarithmic->setEnabled(false);
    m_symmetricLogarithmic->setEnabled(false);
    m_linearThreshold->setEnabled(false);

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
    connect(m_logarithmic, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) {
            const QSignalBlocker blocker(m_symmetricLogarithmic);
            m_symmetricLogarithmic->setChecked(false);
        }
        m_linearThreshold->setVisible(
            m_symmetricLogarithmic->isChecked());
        m_linearThreshold->setEnabled(
            m_controlsReady && m_symmetricLogarithmic->isChecked());
        emit logarithmicChanged();
    });
    connect(m_symmetricLogarithmic, &QCheckBox::toggled, this,
        [this](bool checked) {
            if (checked) {
                const QSignalBlocker blocker(m_logarithmic);
                m_logarithmic->setChecked(false);
                auto threshold = defaultSymmetricLogThreshold(
                    m_minimum->value(), m_maximum->value());
                if (!m_trackedField.isEmpty()) {
                    const auto saved
                        = m_symlogThresholds.constFind(m_trackedField);
                    if (saved != m_symlogThresholds.constEnd()) {
                        threshold = saved.value();
                    } else {
                        m_symlogThresholds.insert(m_trackedField, threshold);
                    }
                }
                const QSignalBlocker thresholdBlocker(m_linearThreshold);
                m_linearThreshold->setValue(threshold);
            }
            m_linearThreshold->setVisible(checked);
            m_linearThreshold->setEnabled(m_controlsReady && checked);
            emit logarithmicChanged();
        });
    connect(m_linearThreshold, qOverload<double>(&QDoubleSpinBox::valueChanged),
        this, [this](double) {
            if (m_symmetricLogarithmic->isChecked()) {
                if (!m_trackedField.isEmpty()) {
                    m_symlogThresholds.insert(
                        m_trackedField, m_linearThreshold->value());
                }
                emit logarithmicChanged();
            }
        });
}

RangeController::Selection RangeController::selection() const
{
    Selection selection;
    selection.mode = mode();
    if (selection.mode == RangeMode::User) {
        selection.userRange = std::pair{m_minimum->value(), m_maximum->value()};
    }
    selection.logarithmic = logarithmic();
    selection.scale = colorScale();
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

ColorScaleConfig RangeController::colorScale() const
{
    if (m_symmetricLogarithmic->isChecked()) {
        return {ColorScale::SymLogarithmic, m_linearThreshold->value()};
    }
    return {
        m_logarithmic->isChecked() ? ColorScale::Logarithmic : ColorScale::Linear,
        m_linearThreshold->value()};
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
    const QSignalBlocker symlogBlocker(m_symmetricLogarithmic);
    const QSignalBlocker thresholdBlocker(m_linearThreshold);
    setMode(selection.mode);
    const auto scale = selection.scale.scale != ColorScale::Linear
        ? selection.scale
        : ColorScaleConfig{
            selection.logarithmic ? ColorScale::Logarithmic : ColorScale::Linear,
            selection.scale.linearThreshold};
    m_logarithmic->setChecked(scale.scale == ColorScale::Logarithmic);
    m_symmetricLogarithmic->setChecked(scale.scale == ColorScale::SymLogarithmic);
    m_linearThreshold->setValue(scale.linearThreshold);
    m_linearThreshold->setVisible(scale.scale == ColorScale::SymLogarithmic);
    if (selection.userRange) {
        m_minimum->setValue(selection.userRange->first);
        m_maximum->setValue(selection.userRange->second);
    }
    updateUserRangeEnabled();
    m_linearThreshold->setEnabled(
        m_controlsReady && m_symmetricLogarithmic->isChecked());
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
    showColorScale({logarithmic ? ColorScale::Logarithmic : ColorScale::Linear,
        m_linearThreshold->value()});
}

void RangeController::showColorScale(ColorScaleConfig scale)
{
    const QSignalBlocker logarithmicBlocker(m_logarithmic);
    const QSignalBlocker symlogBlocker(m_symmetricLogarithmic);
    const QSignalBlocker thresholdBlocker(m_linearThreshold);
    m_logarithmic->setChecked(scale.scale == ColorScale::Logarithmic);
    m_symmetricLogarithmic->setChecked(scale.scale == ColorScale::SymLogarithmic);
    m_linearThreshold->setValue(scale.linearThreshold);
    m_linearThreshold->setVisible(scale.scale == ColorScale::SymLogarithmic);
    m_linearThreshold->setEnabled(
        m_controlsReady && scale.scale == ColorScale::SymLogarithmic);
}

void RangeController::setControlsReady(bool ready)
{
    m_controlsReady = ready;
    m_mode->setEnabled(ready);
    m_logarithmic->setEnabled(ready);
    m_symmetricLogarithmic->setEnabled(ready);
    m_linearThreshold->setEnabled(ready && m_symmetricLogarithmic->isChecked());
    updateUserRangeEnabled();
}

void RangeController::setNumberFormat(const QString& format)
{
    m_minimum->setNumberFormat(format);
    m_maximum->setNumberFormat(format);
    m_linearThreshold->setNumberFormat(format);
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
    // The empty name is not a field. Its callers can reach here with nothing
    // tracked -- a restored spec commits whatever trackedField() says, and
    // that is empty until a field is selected -- and a snapshot filed under
    // it is handed back to the next switch as if it belonged to something.
    if (field.isEmpty()) {
        return;
    }
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
    if (const auto threshold = m_symlogThresholds.constFind(field);
        threshold != m_symlogThresholds.constEnd()) {
        const QSignalBlocker blocker(m_linearThreshold);
        m_linearThreshold->setValue(threshold.value());
    }
    updateUserRangeEnabled();
}

void RangeController::reset()
{
    m_fieldRanges.clear();
    m_symlogThresholds.clear();
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
    if (!field.isEmpty()) {
        // As commitFieldRange: nothing is remembered for a name that is not
        // a field's, or the next switch to one inherits it.
        m_fieldRanges[field].mode = RangeMode::Visible;
    }
    emit statusMessage(
        tr("Metadata range unavailable; using the visible-data range."), 0);
}

} // namespace amrvis::qt
