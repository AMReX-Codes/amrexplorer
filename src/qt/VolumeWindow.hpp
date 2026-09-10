#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/OrthoProjection.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Statistics.hpp>
#include <amrexplorer/core/Volume.hpp>
#include <amrexplorer/pipeline/VolumePipeline.hpp>

#include <QColor>
#include <QMainWindow>
#include <QSize>
#include <QString>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QSlider;
class QVBoxLayout;

namespace amrvis {
class Palette;
}

namespace amrvis::qt {

class IsoWidget;
class OpacityCurveWidget;
class ScientificDoubleSpinBox;

// The Volume Rendering window: an IsoWidget in the middle -- the same
// orthographic view as the main window's iso quadrant, drag to rotate, wheel
// to zoom, XY/XZ/YZ presets, the domain wireframe and the slice planes, and
// the grid boxes when they are switched on -- with the rendered volume drawn
// under the wireframe, and a dock of controls: the opacity curve (or the
// palette's own alpha ramp), the render quality, and the overlay toggles. The
// window holds no data logic: VolumeController decides when to render and
// pushes each frame here.
class VolumeWindow final : public QMainWindow {
    Q_OBJECT

public:
    struct Quality {
        int samplesPerVoxel = 2;
        std::uint64_t maximumVoxels = defaultVolumeVoxelBudget;
    };

    explicit VolumeWindow(QWidget* parent = nullptr);

    // Geometry and overlays, pushed by the host as its own change.
    void setDatasetGeometry(const DatasetMetadata& metadata);
    void setSlicePositions(double x, double y, double z);
    void setSlicePlanesVisible(bool visible);
    void setColorPalette(const Palette* palette);
    // Enables the "use palette alpha" control (the palette carries a ramp).
    void setPaletteHasAlpha(bool hasAlpha);
    // Whether the session can be asked how to sample. A server speaking an
    // older protocol renders volumes but always at the nearest voxel, so the
    // control is offered only when asking for anything else would be heard.
    void setSamplingSelectable(bool selectable);
    // The fields an isosurface may be taken from -- the main window's list,
    // id and display name -- and the one to select while the user has not
    // chosen (the host's pick: a volume fraction, else the volume's own
    // field). A chosen field is kept by name across refills, since ids shift
    // when derived fields come and go. Silent: the host that pushes the list
    // schedules its own render.
    void setIsosurfaceFields(
        const std::vector<std::pair<FieldId, QString>>& fields, FieldId fallback);
    // The isosurface field's value range: the slider's span, and the default
    // iso-value (its midpoint) until the user picks one. nullopt when the
    // session has none, which disables the slider and leaves the spin box as
    // the way in.
    void setIsosurfaceValueRange(std::optional<ValueRange> range);
    // Whether the session can be asked for an isosurface at all, or to hide
    // the volume (a server speaking an older protocol cannot): the shape of
    // setSamplingSelectable, for the same reason.
    void setIsosurfaceSelectable(bool selectable);
    // The surface's colour: set by the colour dialog, by a host restoring a
    // remembered one, and by tests; read by the host to remember it.
    void setIsosurfaceColor(const QColor& color);
    [[nodiscard]] QColor isosurfaceColor() const noexcept
    {
        return m_isosurfaceColorValue;
    }

    // What the render should draw. showVolume is true whenever there is no
    // isosurface in effect -- a render has to draw something -- and otherwise
    // follows its box; isosurface is nullopt while the group is off or the
    // session cannot be asked.
    [[nodiscard]] bool showVolume() const;
    [[nodiscard]] std::optional<VolumeIsosurface> isosurface() const;
    // The field the isosurface controls name whether or not the surface is on,
    // so the host can have its range ready before the toggle.
    [[nodiscard]] std::optional<FieldId> isosurfaceField() const;
    [[nodiscard]] QString isosurfaceFieldName() const;

    // The frame to draw, the camera it was rendered with (so a camera moved
    // since can be corrected for), and a line of status text; and whether a
    // render is in flight (shown in the status). showFailure replaces the
    // status with a render's error, leaving the last good frame on screen.
    void showFrame(const VolumeFrame& frame, const OrthoCamera& camera,
        const QString& status);
    void showFailure(const QString& message);
    void clearFrame();
    void showRendering(bool rendering);

    [[nodiscard]] const OrthoCamera& camera() const noexcept;
    [[nodiscard]] QSize viewSize() const;
    // The view's device pixel ratio: what its logical size has to be
    // multiplied by to cover the pixels the display actually has.
    [[nodiscard]] qreal viewDevicePixelRatio() const;
    [[nodiscard]] OpacityRamp ramp() const;
    // Whether the render should cover only what the slice views show.
    [[nodiscard]] bool limitToVisibleRegion() const;
    // The march's sampling policy: Linear while the box is ticked and in
    // effect, Nearest otherwise.
    [[nodiscard]] SamplingPolicy sampling() const;
    [[nodiscard]] Quality quality() const;

signals:
    // The user moved the camera (drag or wheel) / finished a drag; changed
    // the opacity controls; changed the quality. viewResized: the viewport
    // changed size, so the frame drawn in it is being stretched.
    void cameraChanged();
    void interactionEnded();
    // rampChanged is the opacity curve, which arrives continuously while
    // dragged;
    // paletteAlphaChanged is the checkbox, one discrete choice like the
    // quality combo.
    void rampChanged();
    void paletteAlphaChanged();
    void qualityChanged();
    void regionLimitChanged();
    void samplingChanged();
    // The isosurface controls: isosurfaceChanged is a discrete change (the
    // toggles, the field, the colour, a committed value), isosurfaceDragged a
    // slider still moving -- the same split as rampChanged.
    void isosurfaceChanged();
    void isosurfaceDragged();
    void viewResized();
    void viewScaleChanged();

private:
    void buildControls();
    void buildIsosurfaceControls(QVBoxLayout* layout, QWidget* panel);
    // Follows the palette-alpha box: the curve is editable only when it is the
    // opacity source.
    void syncCurveEnabled();
    // The volume box and the value slider follow the isosurface group and the
    // session: the box has no say while there is no surface (the volume is
    // then all there is to draw), the slider none while no range spans it.
    void syncIsosurfaceEnabled();
    void chooseIsosurfaceColor();
    void setIsosurfaceValueFromSlider(int position);
    void syncIsosurfaceSlider();
    void exportImage();

    IsoWidget* m_view = nullptr;
    OpacityCurveWidget* m_curve = nullptr;
    QCheckBox* m_paletteAlpha = nullptr;
    QComboBox* m_qualityCombo = nullptr;
    QCheckBox* m_regionCheck = nullptr;
    QCheckBox* m_smoothCheck = nullptr;
    // What was last asked of the smooth-sampling box while it was available.
    // The box itself cannot hold it: it is unticked while unavailable so it
    // does not claim a smoothness the render lacks, and something has to
    // remember what to put back.
    bool m_smoothWanted = true;
    QCheckBox* m_boxesCheck = nullptr;
    QCheckBox* m_outlineCheck = nullptr;
    QCheckBox* m_showVolumeCheck = nullptr;
    QGroupBox* m_isosurfaceGroup = nullptr;
    QComboBox* m_isosurfaceField = nullptr;
    ScientificDoubleSpinBox* m_isosurfaceValue = nullptr;
    QSlider* m_isosurfaceSlider = nullptr;
    QPushButton* m_isosurfaceColor = nullptr;
    QSlider* m_isosurfaceOpacity = nullptr;
    QColor m_isosurfaceColorValue = Qt::white;
    std::optional<ValueRange> m_isosurfaceRange;
    // Whether the user has picked a field, and a value: until they do, the
    // field follows the volume's and the value the range's midpoint.
    bool m_isosurfaceFieldChosen = false;
    bool m_isosurfaceValueChosen = false;
    bool m_isosurfaceSelectable = true;
    // What was last asked of the volume box while it had a say; see
    // m_smoothWanted for why the box cannot hold it itself.
    bool m_volumeWanted = true;
    QLabel* m_status = nullptr;
    QLabel* m_rendering = nullptr;
};

} // namespace amrvis::qt
