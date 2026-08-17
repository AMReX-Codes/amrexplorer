// RangeController: the range toolbar widgets and the per-field memory behind
// them. What a user's edits emit, what blocked writes do not, how snapshots
// swap with the field, and how availability disables modes and falls back.

#include "RangeController.hpp"
#include "ScientificDoubleSpinBox.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QStandardItemModel>
#include <QToolBar>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

struct Observed {
    int mode = 0;
    int userRange = 0;
    int logarithmic = 0;
    QStringList statuses;
};

void observe(amrvis::qt::RangeController& controller, Observed& observed)
{
    QObject::connect(&controller, &amrvis::qt::RangeController::modeChanged,
        &controller, [&observed] { ++observed.mode; });
    QObject::connect(&controller,
        &amrvis::qt::RangeController::userRangeChanged, &controller,
        [&observed] { ++observed.userRange; });
    QObject::connect(&controller,
        &amrvis::qt::RangeController::logarithmicChanged, &controller,
        [&observed] { ++observed.logarithmic; });
    QObject::connect(&controller, &amrvis::qt::RangeController::statusMessage,
        &controller, [&observed](const QString& message, int) {
            observed.statuses << message;
        });
}

// The widgets, found by what they are: the one combo, the two spin boxes in
// toolbar order, the one checkbox.
struct Widgets {
    QComboBox* mode = nullptr;
    amrvis::qt::ScientificDoubleSpinBox* minimum = nullptr;
    amrvis::qt::ScientificDoubleSpinBox* maximum = nullptr;
    QCheckBox* logarithmic = nullptr;
};

Widgets widgetsOf(QToolBar& toolbar)
{
    Widgets widgets;
    widgets.mode = toolbar.findChild<QComboBox*>();
    // ScientificDoubleSpinBox has no Q_OBJECT; find the QDoubleSpinBoxes and
    // downcast (there are exactly the two).
    const auto boxes = toolbar.findChildren<QDoubleSpinBox*>();
    if (boxes.size() == 2) {
        widgets.minimum
            = dynamic_cast<amrvis::qt::ScientificDoubleSpinBox*>(boxes[0]);
        widgets.maximum
            = dynamic_cast<amrvis::qt::ScientificDoubleSpinBox*>(boxes[1]);
    }
    widgets.logarithmic = toolbar.findChild<QCheckBox*>();
    return widgets;
}

void selectMode(QComboBox& combo, amrvis::RangeMode mode)
{
    combo.setCurrentIndex(combo.findData(static_cast<int>(mode)));
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    using amrvis::RangeMode;
    using amrvis::qt::RangeController;

    // Creation and initial state; a user's edits emit, per widget, and only
    // when they mean something (a bound edit outside User mode is silent);
    // enablement follows readiness and mode.
    {
        QToolBar toolbar;
        Observed observed;
        RangeController controller;
        observe(controller, observed);
        controller.createToolbarWidgets(&toolbar);
        const auto widgets = widgetsOf(toolbar);
        require(widgets.mode != nullptr && widgets.minimum != nullptr
                && widgets.maximum != nullptr && widgets.logarithmic != nullptr,
            "the toolbar widgets were not created");
        require(widgets.mode->objectName() == QStringLiteral("rangeModeSelector")
                && widgets.mode->count() == 4,
            "the mode combo is not the one the smoke tests look for");
        const auto initial = controller.selection();
        require(initial.mode == RangeMode::File && !initial.userRange
                && !initial.logarithmic && widgets.minimum->value() == 0.0
                && widgets.maximum->value() == 1.0,
            "the initial selection is not File, 0..1, linear");
        require(!widgets.mode->isEnabled() && !widgets.logarithmic->isEnabled()
                && !widgets.minimum->isEnabled() && !widgets.maximum->isEnabled(),
            "the controls start enabled without a dataset");

        widgets.minimum->setValue(0.25);
        require(observed.userRange == 0,
            "a bound edit outside User mode was announced");
        selectMode(*widgets.mode, RangeMode::User);
        require(observed.mode == 1 && controller.mode() == RangeMode::User
                && !widgets.minimum->isEnabled(),
            "selecting User did not announce, or enabled the bounds while not "
            "ready");
        controller.setControlsReady(true);
        require(widgets.mode->isEnabled() && widgets.logarithmic->isEnabled()
                && widgets.minimum->isEnabled() && widgets.maximum->isEnabled(),
            "readiness did not enable the controls for User mode");
        widgets.maximum->setValue(4.0);
        require(observed.userRange == 1
                && controller.selection().userRange
                    == std::pair{0.25, 4.0},
            "a User bound edit was not announced with the new range");
        widgets.logarithmic->setChecked(true);
        require(observed.logarithmic == 1 && controller.logarithmic()
                && controller.selection().logarithmic,
            "Log was not announced");
        selectMode(*widgets.mode, RangeMode::Visible);
        require(observed.mode == 2 && !widgets.minimum->isEnabled()
                && !controller.selection().userRange,
            "leaving User kept the bounds enabled or in the selection");
        controller.setControlsReady(false);
        require(!widgets.mode->isEnabled() && !widgets.logarithmic->isEnabled(),
            "un-readiness left the controls enabled");
        require(observed.mode == 2 && observed.userRange == 1
                && observed.logarithmic == 1,
            "readiness changes announced something");
    }

    // Blocked writes: setSelection, showDisplayRange, showLogarithmic emit
    // nothing and land in the widgets; showDisplayRange writes even in User
    // mode (the caller decides).
    {
        QToolBar toolbar;
        Observed observed;
        RangeController controller;
        observe(controller, observed);
        controller.createToolbarWidgets(&toolbar);
        controller.setControlsReady(true);
        const auto widgets = widgetsOf(toolbar);
        controller.setSelection({RangeMode::User, std::pair{-1.0, 2.0}, true});
        require(controller.mode() == RangeMode::User
                && widgets.minimum->value() == -1.0
                && widgets.maximum->value() == 2.0
                && widgets.logarithmic->isChecked()
                && widgets.minimum->isEnabled(),
            "setSelection did not land in the widgets");
        controller.setSelection({RangeMode::Level, std::nullopt, false});
        require(controller.mode() == RangeMode::Level
                && widgets.minimum->value() == -1.0
                && !widgets.logarithmic->isChecked()
                && !widgets.minimum->isEnabled(),
            "setSelection without a range touched the bounds, or kept them "
            "enabled");
        controller.showDisplayRange(3.0, 5.0);
        controller.showLogarithmic(true);
        require(widgets.minimum->value() == 3.0 && widgets.maximum->value() == 5.0
                && widgets.logarithmic->isChecked(),
            "the display range or log flag was not mirrored");
        controller.setSelection({RangeMode::User, std::nullopt, true});
        controller.showDisplayRange(6.0, 7.0);
        require(widgets.minimum->value() == 6.0,
            "showDisplayRange skipped User mode on its own");
        require(observed.mode == 0 && observed.userRange == 0
                && observed.logarithmic == 0,
            "a blocked write announced a change");
        controller.setNumberFormat(QStringLiteral("%.3f"));
        require(widgets.minimum->text().contains(QStringLiteral("6.000"))
                && widgets.maximum->text().contains(QStringLiteral("7.000")),
            "the number format did not reach both bounds");
    }

    // Per-field memory: switching fields snapshots the old field and loads
    // the new one's (default File for an unknown field); switching back
    // restores; the same field is a no-op; commit records outright; reset
    // forgets and restores the defaults.
    {
        QToolBar toolbar;
        Observed observed;
        RangeController controller;
        observe(controller, observed);
        controller.createToolbarWidgets(&toolbar);
        controller.setControlsReady(true);
        const auto widgets = widgetsOf(toolbar);
        require(controller.trackedField() == 0, "tracked field does not start at 0");
        selectMode(*widgets.mode, RangeMode::User);
        widgets.minimum->setValue(10.0);
        widgets.maximum->setValue(20.0);
        controller.switchField(3);
        require(controller.trackedField() == 3
                && controller.mode() == RangeMode::File
                && !controller.selection().userRange
                && !widgets.minimum->isEnabled(),
            "an unknown field did not load the default");
        controller.switchField(3);
        require(controller.mode() == RangeMode::File,
            "switching to the same field changed something");
        // Field 3 gets its own User range, different from field 0's, so each
        // swap really rewrites the boxes -- and must do so silently: a swap
        // that announced userRangeChanged would re-slice on every field
        // change.
        selectMode(*widgets.mode, RangeMode::User);
        widgets.minimum->setValue(-5.0);
        widgets.maximum->setValue(5.0);
        const auto edits = observed.userRange;
        controller.switchField(0);
        require(controller.trackedField() == 0
                && controller.mode() == RangeMode::User
                && controller.selection().userRange == std::pair{10.0, 20.0}
                && widgets.minimum->isEnabled(),
            "switching back did not restore the field's User range");
        controller.switchField(3);
        require(controller.selection().userRange == std::pair{-5.0, 5.0},
            "the other field's snapshot was not kept");
        require(observed.userRange == edits && observed.mode == 2,
            "a field switch announced a change");
        controller.setSelection({RangeMode::Visible, std::nullopt, false});
        controller.setTrackedField(7);
        controller.commitFieldRange(7);
        controller.switchField(0);
        controller.switchField(7);
        require(controller.mode() == RangeMode::Visible,
            "commit did not record the field's mode");
        controller.reset();
        require(controller.trackedField() == 0
                && controller.mode() == RangeMode::File
                && widgets.minimum->value() == 0.0
                && widgets.maximum->value() == 1.0
                && !widgets.minimum->isEnabled(),
            "reset did not restore the defaults");
        controller.switchField(3);
        require(controller.mode() == RangeMode::File,
            "reset did not forget the fields");
    }

    // Availability: File/Level entries disabled with a tooltip; a selected
    // mode that becomes unavailable falls back to Visible silently (status
    // message, no modeChanged), recorded in the field's snapshot.
    {
        QToolBar toolbar;
        Observed observed;
        RangeController controller;
        observe(controller, observed);
        controller.createToolbarWidgets(&toolbar);
        controller.setControlsReady(true);
        const auto widgets = widgetsOf(toolbar);
        auto* model = qobject_cast<QStandardItemModel*>(widgets.mode->model());
        require(model != nullptr, "the mode combo has no item model");
        const auto item = [&](RangeMode mode) {
            return model->item(widgets.mode->findData(static_cast<int>(mode)));
        };
        selectMode(*widgets.mode, RangeMode::Level);
        controller.updateAvailability({.file = true, .level = false}, 0);
        require(!item(RangeMode::Level)->isEnabled()
                && !item(RangeMode::Level)->toolTip().isEmpty()
                && item(RangeMode::File)->isEnabled()
                && item(RangeMode::File)->toolTip().isEmpty(),
            "availability did not disable Level with a tooltip");
        require(controller.mode() == RangeMode::Visible
                && observed.statuses.size() == 1
                && observed.statuses.front().contains(
                    QStringLiteral("visible-data range")),
            "an unavailable mode did not fall back to Visible with a message");
        require(observed.mode == 1,
            "the fallback announced a mode change (only the user's Level pick "
            "should have)");
        controller.switchField(1);
        controller.switchField(0);
        require(controller.mode() == RangeMode::Visible,
            "the fallback was not recorded in the field's snapshot");
        controller.updateAvailability({.file = true, .level = true}, 0);
        require(item(RangeMode::Level)->isEnabled()
                && item(RangeMode::Level)->toolTip().isEmpty()
                && controller.mode() == RangeMode::Visible
                && observed.statuses.size() == 1,
            "restored availability changed the mode or re-announced");
        selectMode(*widgets.mode, RangeMode::User);
        controller.updateAvailability({.file = false, .level = false}, 0);
        require(controller.mode() == RangeMode::User
                && widgets.minimum->isEnabled(),
            "User was disturbed by metadata modes going away");
    }

    std::cout << "range controller tests passed\n";
    return 0;
}
