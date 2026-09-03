#pragma once

#include <amrexplorer/pipeline/SlicePipeline.hpp>
#include <amrexplorer/core/ValueMapping.hpp>

#include <QHash>
#include <QObject>
#include <QString>

#include <optional>
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
        ColorScaleConfig scale;
        Selection() = default;
        Selection(RangeMode selectedMode,
            std::optional<std::pair<double, double>> selectedUserRange,
            bool useLogarithmic, ColorScaleConfig scaleConfig = {})
            : mode(selectedMode), userRange(std::move(selectedUserRange)),
              logarithmic(useLogarithmic), scale(scaleConfig)
        {
        }
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
    [[nodiscard]] ColorScaleConfig colorScale() const;

    // Blocked writes: none of these emits a change signal.
    // The whole selection, as a restore does; the min/max are written only
    // when a User range is given.
    void setSelection(const Selection& selection);
    // The active view's display range and log flag, mirrored into the boxes
    // and checkbox (the min/max unconditionally: the caller decides whether
    // a User range must be left alone).
    void showDisplayRange(double minimum, double maximum);
    void showLogarithmic(bool logarithmic);
    void showColorScale(ColorScaleConfig scale);
    // Whether a dataset is open: mode and Log enabled iff ready, min/max iff
    // ready and the mode is User.
    void setControlsReady(bool ready);
    void setNumberFormat(const QString& format);

    // Per-field memory, keyed by the field's *name*. The widgets represent
    // trackedField(); switchField snapshots them for that field and loads the
    // new field's snapshot (or the default: File, no range) -- a no-op for the
    // same field. commit snapshots the widgets for a field outright (a
    // restore, after setSelection). reset forgets every field for a fresh
    // dataset and puts the widgets back to File, 0..1, min/max disabled.
    //
    // By name and not by field id: an id means something only in the field
    // list it came from, and that list moves -- a sequence frame need not
    // agree with the last one, and a derived definition that one frame cannot
    // resolve compacts every id after it. Keyed by id, selecting a field then
    // restored whichever field used to hold that number.
    void switchField(const QString& field);
    void commitFieldRange(const QString& field);
    void reset();
    [[nodiscard]] const QString& trackedField() const noexcept
    {
        return m_trackedField;
    }
    void setTrackedField(const QString& field) { m_trackedField = field; }
    // Enables or disables the File and Level entries for what `field` at the
    // current level can offer. A selected mode that became unavailable falls
    // back to Visible (recorded in the field's snapshot) with a status
    // message; that fallback is silent otherwise -- no modeChanged.
    void updateAvailability(
        const Availability& availability, const QString& field);

signals:
    // A user's edit of the mode, of a User bound, or of the color scale.
    // Blocked writes emit none of these.
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
    void applyFieldRange(const QString& field);
    void updateUserRangeEnabled();

    QComboBox* m_mode = nullptr;
    ScientificDoubleSpinBox* m_minimum = nullptr;
    ScientificDoubleSpinBox* m_maximum = nullptr;
    QCheckBox* m_logarithmic = nullptr;
    QCheckBox* m_symmetricLogarithmic = nullptr;
    ScientificDoubleSpinBox* m_linearThreshold = nullptr;
    bool m_controlsReady = false;
    QHash<QString, FieldRange> m_fieldRanges;
    QHash<QString, double> m_symlogThresholds;
    QString m_trackedField;
};

} // namespace amrvis::qt
