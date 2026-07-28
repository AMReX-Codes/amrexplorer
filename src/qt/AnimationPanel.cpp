#include "AnimationPanel.hpp"

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace amrvis::qt {

AnimationPanel::AnimationPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    // Plane sweep: steps or continuously advances the slice position of one
    // axis. Only meaningful for 3-D datasets; MainWindow toggles visibility.
    m_sweepGroup = new QGroupBox(tr("Plane Sweep"), this);
    auto* sweepLayout = new QGridLayout(m_sweepGroup);
    sweepLayout->addWidget(new QLabel(tr("Axis:"), m_sweepGroup), 0, 0);
    m_sweepAxisCombo = new QComboBox(m_sweepGroup);
    m_sweepAxisCombo->addItem(tr("X"), 0);
    m_sweepAxisCombo->addItem(tr("Y"), 1);
    m_sweepAxisCombo->addItem(tr("Z"), 2);
    m_sweepAxisCombo->setCurrentIndex(2);
    sweepLayout->addWidget(m_sweepAxisCombo, 0, 1, 1, 3);
    m_sweepBack = new QToolButton(m_sweepGroup);
    m_sweepBack->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    m_sweepBack->setToolTip(tr("Step back one cell"));
    m_sweepPlay = new QToolButton(m_sweepGroup);
    m_sweepPlay->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_sweepPlay->setToolTip(tr("Continuously sweep along the axis"));
    m_sweepForward = new QToolButton(m_sweepGroup);
    m_sweepForward->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
    m_sweepForward->setToolTip(tr("Step forward one cell"));
    sweepLayout->addWidget(m_sweepBack, 1, 1);
    sweepLayout->addWidget(m_sweepPlay, 1, 2);
    sweepLayout->addWidget(m_sweepForward, 1, 3);
    m_sweepGroup->setVisible(false);
    layout->addWidget(m_sweepGroup);

    // Plotfile sequence: the legacy file animation controls. The frame row
    // mirrors the legacy File slider; the name and time of the current frame
    // are shown below it.
    m_sequenceGroup = new QGroupBox(tr("Plotfile Sequence"), this);
    auto* sequenceLayout = new QGridLayout(m_sequenceGroup);
    m_frameSlider = new QSlider(Qt::Horizontal, m_sequenceGroup);
    m_frameSlider->setRange(0, 0);
    sequenceLayout->addWidget(m_frameSlider, 0, 0, 1, 2);
    m_frameSpin = new QSpinBox(m_sequenceGroup);
    m_frameSpin->setRange(0, 0);
    // Defer typed input to Enter/focus-out (matching ScientificDoubleSpinBox
    // and the frame slider's sliderReleased): each intermediate keystroke
    // otherwise emits valueChanged, which reloads a plotfile per key.
    m_frameSpin->setKeyboardTracking(false);
    sequenceLayout->addWidget(m_frameSpin, 0, 2);
    m_frameCountLabel = new QLabel(tr("/ 0"), m_sequenceGroup);
    sequenceLayout->addWidget(m_frameCountLabel, 0, 3);
    m_frameName = new QLabel(tr("—"), m_sequenceGroup);
    // The directory name and time string vary in width from frame to frame,
    // and the animation dock sits on the right edge, so an unconstrained label
    // would grow the dock -- and thus the whole main window -- whenever a
    // longer value appears. Ignored horizontal keeps the dock width driven by
    // the fixed slider/spin/buttons instead.
    m_frameName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    sequenceLayout->addWidget(m_frameName, 1, 0, 1, 4);
    m_frameTime = new QLabel(tr("—"), m_sequenceGroup);
    m_frameTime->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    sequenceLayout->addWidget(m_frameTime, 2, 0, 1, 4);
    m_sequenceBack = new QToolButton(m_sequenceGroup);
    m_sequenceBack->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    m_sequenceBack->setToolTip(tr("Step back one frame"));
    m_sequencePlay = new QToolButton(m_sequenceGroup);
    m_sequencePlay->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_sequencePlay->setToolTip(tr("Play the plotfile sequence"));
    m_sequenceForward = new QToolButton(m_sequenceGroup);
    m_sequenceForward->setIcon(
        style()->standardIcon(QStyle::SP_MediaSkipForward));
    m_sequenceForward->setToolTip(tr("Step forward one frame"));
    sequenceLayout->addWidget(m_sequenceBack, 3, 1);
    sequenceLayout->addWidget(m_sequencePlay, 3, 2);
    sequenceLayout->addWidget(m_sequenceForward, 3, 3);
    m_sequenceGroup->setVisible(false);
    layout->addWidget(m_sequenceGroup);

    // Shared playback speed. Legacy maps slider value v to a frame delay of
    // (600 - v) ms for v in 0..599; this slider runs 1..600 with
    // delay = 601 - v, so right is faster and the default 300 gives 301 ms.
    auto* speedRow = new QWidget(this);
    auto* speedLayout = new QHBoxLayout(speedRow);
    speedLayout->setContentsMargins(0, 0, 0, 0);
    speedLayout->addWidget(new QLabel(tr("Speed:"), speedRow));
    m_speedSlider = new QSlider(Qt::Horizontal, speedRow);
    m_speedSlider->setRange(1, 600);
    m_speedSlider->setValue(300);
    speedLayout->addWidget(m_speedSlider, 1);
    m_delayLabel = new QLabel(speedRow);
    m_delayLabel->setMinimumWidth(52);
    speedLayout->addWidget(m_delayLabel);
    layout->addWidget(speedRow);
    layout->addStretch(1);
    updateDelayLabel();

    connect(m_sweepBack, &QToolButton::clicked, this,
        [this] { emit sweepStepRequested(-1); });
    connect(m_sweepForward, &QToolButton::clicked, this,
        [this] { emit sweepStepRequested(1); });
    connect(m_sweepPlay, &QToolButton::clicked, this,
        [this] { emit sweepPlayToggled(); });
    connect(m_sequenceBack, &QToolButton::clicked, this,
        [this] { emit sequenceStepRequested(-1); });
    connect(m_sequenceForward, &QToolButton::clicked, this,
        [this] { emit sequenceStepRequested(1); });
    connect(m_sequencePlay, &QToolButton::clicked, this,
        [this] { emit sequencePlayToggled(); });
    // Dragging the slider updates the spinbox live but only jumps to the
    // frame on release; keyboard moves (no drag) jump immediately.
    connect(m_frameSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_updatingFrame) {
            return;
        }
        {
            const QSignalBlocker blocker(m_frameSpin);
            m_frameSpin->setValue(value);
        }
        if (!m_frameSlider->isSliderDown()) {
            emit sequenceFrameRequested(value);
        }
    });
    connect(m_frameSlider, &QSlider::sliderReleased, this, [this] {
        if (!m_updatingFrame) {
            emit sequenceFrameRequested(m_frameSlider->value());
        }
    });
    connect(m_frameSpin, qOverload<int>(&QSpinBox::valueChanged), this,
        [this](int value) {
            if (m_updatingFrame) {
                return;
            }
            const QSignalBlocker blocker(m_frameSlider);
            m_frameSlider->setValue(value);
            emit sequenceFrameRequested(value);
        });
    connect(m_speedSlider, &QSlider::valueChanged, this, [this](int value) {
        updateDelayLabel();
        emit speedChanged(value);
    });
}

void AnimationPanel::setSweepVisible(bool visible)
{
    m_sweepGroup->setVisible(visible);
}

void AnimationPanel::setSequenceVisible(bool visible)
{
    m_sequenceGroup->setVisible(visible);
}

int AnimationPanel::sweepAxis() const
{
    return m_sweepAxisCombo->currentData().toInt();
}

void AnimationPanel::setSweepAxis(int axis)
{
    const auto index = m_sweepAxisCombo->findData(std::clamp(axis, 0, 2));
    if (index >= 0) {
        m_sweepAxisCombo->setCurrentIndex(index);
    }
}

void AnimationPanel::setSweepPlaying(bool playing)
{
    m_sweepPlay->setIcon(style()->standardIcon(
        playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}

void AnimationPanel::setSequencePlaying(bool playing)
{
    m_sequencePlay->setIcon(style()->standardIcon(
        playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}

void AnimationPanel::setSequenceFrameCount(int count)
{
    const auto last = std::max(count - 1, 0);
    const QSignalBlocker sliderBlocker(m_frameSlider);
    const QSignalBlocker spinBlocker(m_frameSpin);
    m_frameSlider->setRange(0, last);
    m_frameSpin->setRange(0, last);
    m_frameCountLabel->setText(tr("/ %1").arg(count));
}

void AnimationPanel::setSequenceFrame(int index)
{
    m_updatingFrame = true;
    m_frameSlider->setValue(index);
    m_frameSpin->setValue(index);
    m_updatingFrame = false;
}

void AnimationPanel::setSequenceInfo(const QString& directoryName, double time)
{
    m_frameName->setText(directoryName);
    m_frameTime->setText(tr("T = %1").arg(time, 0, 'g', 12));
}

bool AnimationPanel::sequenceControlsVisible() const
{
    return !m_sequenceGroup->isHidden();
}

int AnimationPanel::sequenceFrameCount() const
{
    return m_frameSlider->maximum() + 1;
}

int AnimationPanel::speedValue() const
{
    return m_speedSlider->value();
}

void AnimationPanel::setSpeedValue(int value)
{
    const QSignalBlocker blocker(m_speedSlider);
    m_speedSlider->setValue(value);
    updateDelayLabel();
}

int AnimationPanel::frameDelayMs() const
{
    return 601 - m_speedSlider->value();
}

void AnimationPanel::updateDelayLabel()
{
    m_delayLabel->setText(tr("%1 ms").arg(frameDelayMs()));
}

} // namespace amrvis::qt
