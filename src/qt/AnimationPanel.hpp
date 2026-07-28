#pragma once

#include <QWidget>

class QComboBox;
class QGroupBox;
class QLabel;
class QSlider;
class QSpinBox;
class QToolButton;

namespace amrvis::qt {

// The Animation dock's contents: the legacy plane-sweep controls (3-D
// datasets), the plotfile-sequence controls (file animation), and the shared
// Speed slider. The panel is pure UI — MainWindow owns the playback timer and
// all dataset logic and drives the panel through the setters below.
class AnimationPanel final : public QWidget {
    Q_OBJECT

public:
    explicit AnimationPanel(QWidget* parent = nullptr);

    void setSweepVisible(bool visible);
    void setSequenceVisible(bool visible);

    // Sweep axis as a dataset axis index: 0 = X, 1 = Y, 2 = Z.
    [[nodiscard]] int sweepAxis() const;
    void setSweepAxis(int axis);
    void setSweepPlaying(bool playing);
    void setSequencePlaying(bool playing);

    void setSequenceFrameCount(int count);
    void setSequenceFrame(int index);
    void setSequenceInfo(const QString& directoryName, double time);
    [[nodiscard]] bool sequenceControlsVisible() const;
    [[nodiscard]] int sequenceFrameCount() const;

    // Legacy speed mapping: the legacy slider ran 0..599 with a frame delay
    // of (600 - value) ms; this slider runs 1..600 with delay = 601 - value,
    // so the default 300 plays one frame every 301 ms and sliding right
    // shortens the delay down to 1 ms.
    [[nodiscard]] int speedValue() const;
    void setSpeedValue(int value);
    [[nodiscard]] int frameDelayMs() const;

signals:
    void sweepStepRequested(int direction);
    void sweepPlayToggled();
    void sequenceStepRequested(int direction);
    void sequencePlayToggled();
    void sequenceFrameRequested(int index);
    void speedChanged(int value);

private:
    void updateDelayLabel();

    QGroupBox* m_sweepGroup = nullptr;
    QComboBox* m_sweepAxisCombo = nullptr;
    QToolButton* m_sweepBack = nullptr;
    QToolButton* m_sweepPlay = nullptr;
    QToolButton* m_sweepForward = nullptr;
    QGroupBox* m_sequenceGroup = nullptr;
    QSlider* m_frameSlider = nullptr;
    QSpinBox* m_frameSpin = nullptr;
    QLabel* m_frameCountLabel = nullptr;
    QLabel* m_frameName = nullptr;
    QLabel* m_frameTime = nullptr;
    QToolButton* m_sequenceBack = nullptr;
    QToolButton* m_sequencePlay = nullptr;
    QToolButton* m_sequenceForward = nullptr;
    QSlider* m_speedSlider = nullptr;
    QLabel* m_delayLabel = nullptr;
    bool m_updatingFrame = false;
};

} // namespace amrvis::qt
