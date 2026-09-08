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
    QCheckBox* symlog = nullptr;
    amrvis::qt::ScientificDoubleSpinBox* linearThreshold = nullptr;
};

Widgets widgetsOf(QToolBar& toolbar)
{
    Widgets widgets;
    widgets.mode = toolbar.findChild<QComboBox*>();
    // ScientificDoubleSpinBox has no Q_OBJECT; find the QDoubleSpinBoxes and
    // downcast (there are exactly the two).
    const auto boxes = toolbar.findChildren<QDoubleSpinBox*>();
    if (boxes.size() >= 2) {
        widgets.minimum
            = dynamic_cast<amrvis::qt::ScientificDoubleSpinBox*>(boxes[0]);
        widgets.maximum
            = dynamic_cast<amrvis::qt::ScientificDoubleSpinBox*>(boxes[1]);
    }
    widgets.logarithmic = toolbar.findChild<QCheckBox*>();
    widgets.symlog = toolbar.findChild<QCheckBox*>(QStringLiteral("symmetricLogarithmicScale"));
    widgets.linearThreshold = dynamic_cast<amrvis::qt::ScientificDoubleSpinBox*>(
        toolbar.findChild<QDoubleSpinBox*>(QStringLiteral("symlogLinearThreshold")));
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
                && widgets.maximum != nullptr && widgets.logarithmic != nullptr
                && widgets.symlog != nullptr && widgets.linearThreshold != nullptr,
            "the toolbar widgets were not created");
        require(widgets.mode->objectName() == QStringLiteral("rangeModeSelector")
                && widgets.mode->count() == 4,
            "the mode combo is not the one the smoke tests look for");
        const auto initial = controller.selection();
        require(initial.mode == RangeMode::File && !initial.userRange &&
                    initial.scale.scale != amrvis::ColorScale::Logarithmic &&
                    widgets.minimum->value() == 0.0 && widgets.maximum->value() == 1.0,
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
        require(observed.logarithmic == 1 &&
                    (controller.colorScale().scale == amrvis::ColorScale::Logarithmic) &&
                    (controller.selection().scale.scale == amrvis::ColorScale::Logarithmic),
                "Log was not announced");
        widgets.symlog->setChecked(true);
        require(widgets.linearThreshold->value() == 0.01,
            "Symlog did not choose a range-relative initial threshold");
        widgets.linearThreshold->setValue(0.25);
        require(controller.colorScale().scale == amrvis::ColorScale::SymLogarithmic
                && controller.colorScale().linearThreshold == 0.25
                && !widgets.logarithmic->isChecked(),
            "Symlog was not selected exclusively with its threshold");
        selectMode(*widgets.mode, RangeMode::Visible);
        require(observed.mode == 2 && !widgets.minimum->isEnabled()
                && !controller.selection().userRange,
            "leaving User kept the bounds enabled or in the selection");
        controller.setControlsReady(false);
        require(!widgets.mode->isEnabled() && !widgets.logarithmic->isEnabled(),
            "un-readiness left the controls enabled");
        require(observed.mode == 2 && observed.userRange == 1
                && observed.logarithmic == 3,
            "readiness changes announced something");
    }

    // Symlog's first threshold is relative to the field's displayed scale,
    // and an explicit edit is restored when returning to that field.
    {
        QToolBar toolbar;
        RangeController controller;
        controller.createToolbarWidgets(&toolbar);
        controller.setControlsReady(true);
        controller.setTrackedField(QStringLiteral("density"));
        const auto widgets = widgetsOf(toolbar);
        controller.showDisplayRange(-3.0e-8, 7.0e-8);
        widgets.symlog->setChecked(true);
        require(widgets.linearThreshold->value() == 1.0e-10,
            "a sub-unit field kept the fixed Symlog threshold");
        widgets.linearThreshold->setValue(2.0e-11);

        controller.switchField(QStringLiteral("temperature"));
        Observed observed;
        observe(controller, observed);
        require(controller.showDisplayRange(-3000.0, 8000.0),
            "initializing a field threshold did not request recoloring");
        require(observed.logarithmic == 0,
            "automatic threshold initialization emitted a user edit");
        require(widgets.linearThreshold->value() == 10.0,
            "a newly selected field did not get its own Symlog threshold");
        require(!controller.showDisplayRange(-3.0e9, 8.0e9),
            "a passive range update requested another recoloring");
        require(widgets.linearThreshold->value() == 10.0,
            "a passive range update replaced the saved threshold");
        controller.switchField(QStringLiteral("density"));
        require(widgets.linearThreshold->value() == 2.0e-11,
            "a field's edited Symlog threshold was not restored");
        controller.switchField(QStringLiteral("tiny"));
        require(controller.showDisplayRange(-3.0e-15, 8.0e-15)
                && widgets.linearThreshold->value() == 1.0e-17,
            "switching to a tiny field retained the previous threshold");

        controller.reset();
        controller.setTrackedField(QStringLiteral("density"));
        require(controller.showDisplayRange(-3000.0, 8000.0) &&
                    widgets.linearThreshold->value() == 10.0,
                "the first field after reset retained the previous dataset threshold");
        require(!controller.showDisplayRange(-3.0e9, 8.0e9) &&
                    widgets.linearThreshold->value() == 10.0,
                "the first field threshold was initialized more than once");
    }

    // FAB navigation resets the cache and restores a saved selection for
    // the rendered field, which need not be the first field in the dataset.
    {
        QToolBar toolbar;
        RangeController controller;
        controller.createToolbarWidgets(&toolbar);
        Observed observed;
        observe(controller, observed);
        for (const double threshold : {2.0e-11, 0.25}) {
            controller.reset();
            controller.setTrackedField(QStringLiteral("first"));
            controller.setTrackedField(QStringLiteral("density"));
            controller.setSelection({RangeMode::Visible, std::nullopt,
                {amrvis::ColorScale::SymLogarithmic, threshold}});
            controller.commitFieldRange(controller.trackedField());
            require(!controller.showDisplayRange(-3000.0, 8000.0)
                    && controller.colorScale().linearThreshold == threshold,
                "navigation replaced an explicitly restored threshold");
            controller.switchField(QStringLiteral("first"));
            require(controller.showDisplayRange(-3000.0, 8000.0)
                    && controller.colorScale().linearThreshold == 10.0,
                "restoration saved the threshold under the wrong field");
            controller.switchField(QStringLiteral("density"));
            require(controller.colorScale().linearThreshold == threshold,
                "the restored threshold was not cached for field switching");
        }
        require(observed.logarithmic == 0,
            "restoration emitted a user scale edit");
    }

    // Blocked writes: setSelection, showDisplayRange, showColorScale emit
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
        controller.setSelection(
            {RangeMode::User, std::pair{-1.0, 2.0}, {amrvis::ColorScale::Logarithmic}});
        require(controller.mode() == RangeMode::User
                && widgets.minimum->value() == -1.0
                && widgets.maximum->value() == 2.0
                && widgets.logarithmic->isChecked()
                && widgets.minimum->isEnabled(),
            "setSelection did not land in the widgets");
        controller.setSelection({RangeMode::Level, std::nullopt, {amrvis::ColorScale::Linear}});
        require(controller.mode() == RangeMode::Level
                && widgets.minimum->value() == -1.0
                && !widgets.logarithmic->isChecked()
                && !widgets.minimum->isEnabled(),
            "setSelection without a range touched the bounds, or kept them "
            "enabled");
        controller.showDisplayRange(3.0, 5.0);
        controller.showColorScale({amrvis::ColorScale::Logarithmic});
        require(widgets.minimum->value() == 3.0 && widgets.maximum->value() == 5.0
                && widgets.logarithmic->isChecked(),
            "the display range or log flag was not mirrored");
        controller.setSelection({RangeMode::User, std::nullopt, {amrvis::ColorScale::Logarithmic}});
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
        // Nothing is tracked until a field is selected, which a name can say
        // and a field id could not.
        require(controller.trackedField().isEmpty(),
            "a fresh controller already tracks a field");
        controller.setTrackedField(QStringLiteral("f0"));
        selectMode(*widgets.mode, RangeMode::User);
        widgets.minimum->setValue(10.0);
        widgets.maximum->setValue(20.0);
        controller.switchField(QStringLiteral("f3"));
        require(controller.trackedField() == QStringLiteral("f3")
                && controller.mode() == RangeMode::File
                && !controller.selection().userRange
                && !widgets.minimum->isEnabled(),
            "an unknown field did not load the default");
        controller.switchField(QStringLiteral("f3"));
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
        controller.switchField(QStringLiteral("f0"));
        require(controller.trackedField() == QStringLiteral("f0")
                && controller.mode() == RangeMode::User
                && controller.selection().userRange == std::pair{10.0, 20.0}
                && widgets.minimum->isEnabled(),
            "switching back did not restore the field's User range");
        controller.switchField(QStringLiteral("f3"));
        require(controller.selection().userRange == std::pair{-5.0, 5.0},
            "the other field's snapshot was not kept");
        require(observed.userRange == edits && observed.mode == 2,
            "a field switch announced a change");
        controller.setSelection({RangeMode::Visible, std::nullopt, {amrvis::ColorScale::Linear}});
        controller.setTrackedField(QStringLiteral("f7"));
        controller.commitFieldRange(QStringLiteral("f7"));
        controller.switchField(QStringLiteral("f0"));
        controller.switchField(QStringLiteral("f7"));
        require(controller.mode() == RangeMode::Visible,
            "commit did not record the field's mode");
        controller.reset();
        require(controller.trackedField().isEmpty()
                && controller.mode() == RangeMode::File
                && widgets.minimum->value() == 0.0
                && widgets.maximum->value() == 1.0
                && !widgets.minimum->isEnabled(),
            "reset did not restore the defaults");
        controller.switchField(QStringLiteral("f3"));
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
        controller.updateAvailability({.file = true, .level = false}, QStringLiteral("f0"));
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
        controller.switchField(QStringLiteral("f1"));
        controller.switchField(QStringLiteral("f0"));
        require(controller.mode() == RangeMode::Visible,
            "the fallback was not recorded in the field's snapshot");
        controller.updateAvailability({.file = true, .level = true}, QStringLiteral("f0"));
        require(item(RangeMode::Level)->isEnabled()
                && item(RangeMode::Level)->toolTip().isEmpty()
                && controller.mode() == RangeMode::Visible
                && observed.statuses.size() == 1,
            "restored availability changed the mode or re-announced");
        selectMode(*widgets.mode, RangeMode::User);
        controller.updateAvailability({.file = false, .level = false}, QStringLiteral("f0"));
        require(controller.mode() == RangeMode::User
                && widgets.minimum->isEnabled(),
            "User was disturbed by metadata modes going away");
    }

    {
        // A field's remembered range follows its name, not its position. The
        // ids move whenever the field list does -- a sequence frame that lists
        // fewer fields, or a derived definition that one frame cannot resolve
        // and leaves out -- and keyed by id the memory handed the range of
        // whichever field used to sit at that number.
        QToolBar toolbar;
        RangeController controller;
        controller.createToolbarWidgets(&toolbar);
        controller.setControlsReady(true);
        const auto widgets = widgetsOf(toolbar);

        controller.setTrackedField(QStringLiteral("speed"));
        selectMode(*widgets.mode, RangeMode::User);
        widgets.minimum->setValue(3.0);
        widgets.maximum->setValue(4.0);
        controller.commitFieldRange(QStringLiteral("speed"));

        // Another field, then back -- as a reload that renumbered the list
        // would leave things.
        controller.switchField(QStringLiteral("drag"));
        require(controller.mode() == RangeMode::File,
            "an unseen field inherited a remembered range");
        controller.switchField(QStringLiteral("speed"));
        require(controller.mode() == RangeMode::User
                && widgets.minimum->value() == 3.0
                && widgets.maximum->value() == 4.0,
            "the field's own range did not come back with its name");
    }

    // The empty name is not a field, so nothing may be filed under it: the
    // memory is keyed by name now, and an unset name is what a controller has
    // before a field is tracked.
    {
        QToolBar toolbar;
        Observed observed;
        RangeController controller;
        observe(controller, observed);
        controller.createToolbarWidgets(&toolbar);
        controller.setControlsReady(true);
        const auto widgets = widgetsOf(toolbar);
        require(controller.trackedField().isEmpty(),
            "a fresh controller already tracks a field");

        // A switch away from nothing must not snapshot the widgets under the
        // empty name.
        selectMode(*widgets.mode, RangeMode::User);
        widgets.minimum->setValue(1.0);
        widgets.maximum->setValue(2.0);
        controller.switchField(QStringLiteral("density"));

        selectMode(*widgets.mode, RangeMode::User);
        widgets.minimum->setValue(7.0);
        widgets.maximum->setValue(8.0);
        controller.switchField(QString());
        require(controller.selection().userRange
                != std::pair{1.0, 2.0},
            "a range was filed under the empty name and handed back");

        // The same rule in the writer a restored spec goes through, which
        // commits whatever trackedField() says.
        selectMode(*widgets.mode, RangeMode::User);
        widgets.minimum->setValue(3.0);
        widgets.maximum->setValue(4.0);
        controller.commitFieldRange(QString());
        controller.switchField(QStringLiteral("temperature"));
        controller.switchField(QString());
        require(controller.selection().userRange != std::pair{3.0, 4.0},
            "commitFieldRange filed a range under the empty name");
    }

    std::cout << "range controller tests passed\n";
    return 0;
}
