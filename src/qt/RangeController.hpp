#pragma once

#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <QObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

class QCheckBox;
class QComboBox;
class QToolBar;

namespace amrvis::qt {

class ScientificDoubleSpinBox;

// The range controls of the Color and Overlay toolbar -- the range mode
// (File / Level / Visible / User), the User min/max, and Log -- with the
// per-field memory behind them: each field remembers its own mode and User
// range for the current dataset, swapped in and out as the field selector
// changes. The host reads selection() into its slice requests and frame
// specs, and writes back through blocked setters (restore, colour controls,
// sync completions) that emit nothing; only a user's edit emits a change
// signal. The visible-range sync that keeps three 3-D panels on
// one range is the host's slice-result coordination, not this class's.
class RangeController final : public QObject {
    Q_OBJECT

public:
    struct Selection {
        RangeMode mode = RangeMode::File;
        // The User min/max, present when mode is User.
        std::optional<std::pair<double, double>> userRange;
        bool logarithmic = false;
    };
    // Which metadata-backed modes the current field/level can offer.
    struct Availability {
        bool file = true;
        bool level = true;
    };

    explicit RangeController(QObject* parent = nullptr);

    // Builds the mode combo, the min/max spin boxes and the Log checkbox into
    // the toolbar, in that order (Log after a separator). Owned by the
    // toolbar; call once.
    void createToolbarWidgets(QToolBar* toolbar);

    [[nodiscard]] Selection selection() const;
    [[nodiscard]] RangeMode mode() const;
    [[nodiscard]] bool logarithmic() const;

    // Blocked writes: none of these emits a change signal.
    // The whole selection, as a restore does; the min/max are written only
    // when a User range is given.
    void setSelection(const Selection& selection);
    // The active view's display range and log flag, mirrored into the boxes
    // and checkbox (the min/max unconditionally: the caller decides whether
    // a User range must be left alone).
    void showDisplayRange(double minimum, double maximum);
    void showLogarithmic(bool logarithmic);
    // Whether a dataset is open: mode and Log enabled iff ready, min/max iff
    // ready and the mode is User.
    void setControlsReady(bool ready);
    void setNumberFormat(const QString& format);

    // Per-field memory. The widgets represent trackedField(); switchField
    // snapshots them for that field and loads the new field's snapshot (or
    // the default: File, no range) -- a no-op for the same field. commit
    // snapshots the widgets for a field outright (a restore, after
    // setSelection). reset forgets every field for a fresh dataset and puts
    // the widgets back to File, 0..1, min/max disabled.
    void switchField(std::uint32_t field);
    void commitFieldRange(std::uint32_t field);
    void reset();
    [[nodiscard]] std::uint32_t trackedField() const noexcept
    {
        return m_trackedField;
    }
    void setTrackedField(std::uint32_t field) noexcept
    {
        m_trackedField = field;
    }
    // Enables or disables the File and Level entries for what `field` at the
    // current level can offer. A selected mode that became unavailable falls
    // back to Visible (recorded in the field's snapshot) with a status
    // message; that fallback is silent otherwise -- no modeChanged.
    void updateAvailability(const Availability& availability, std::uint32_t field);

signals:
    // A user's edit of the mode, of a User bound, or of Log. Blocked writes
    // emit none of these.
    void modeChanged();
    void userRangeChanged();
    void logarithmicChanged();
    void statusMessage(const QString& message, int timeoutMs);

private:
    struct FieldRange {
        RangeMode mode = RangeMode::File;
        std::optional<std::pair<double, double>> userRange;
    };

    void setMode(RangeMode mode);
    void applyFieldRange(std::uint32_t field);
    void updateUserRangeEnabled();

    QComboBox* m_mode = nullptr;
    ScientificDoubleSpinBox* m_minimum = nullptr;
    ScientificDoubleSpinBox* m_maximum = nullptr;
    QCheckBox* m_logarithmic = nullptr;
    bool m_controlsReady = false;
    std::unordered_map<std::uint32_t, FieldRange> m_fieldRanges;
    std::uint32_t m_trackedField = 0;
};

} // namespace amrvis::qt
