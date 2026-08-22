#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/OrthoProjection.hpp>
#include <amrexplorer/core/Volume.hpp>
#include <amrexplorer/pipeline/VolumePipeline.hpp>

#include <QMainWindow>
#include <QSize>
#include <QString>

#include <cstdint>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;

namespace amrvis {
class Palette;
}

namespace amrvis::qt {

class IsoWidget;

// The Volume Rendering window: an IsoWidget in the middle -- the same
// orthographic view as the main window's iso quadrant, drag to rotate, wheel
// to zoom, XY/XZ/YZ presets, the domain wireframe, the grid boxes and the
// slice planes -- with the rendered volume drawn under the wireframe, and a
// dock of controls: the opacity ramp (a window over the colour range and a
// maximum opacity, or the palette's own alpha ramp), the render quality, and
// the overlay toggles. The window holds no data logic: VolumeController
// decides when to render and pushes each frame here.
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
    [[nodiscard]] OpacityRamp ramp() const;
    [[nodiscard]] Quality quality() const;

signals:
    // The user moved the camera (drag or wheel) / finished a drag; changed
    // the opacity controls; changed the quality. viewResized: the viewport
    // changed size, so the frame drawn in it is being stretched.
    void cameraChanged();
    void interactionEnded();
    // rampChanged is the sliders, which arrive continuously while dragged;
    // paletteAlphaChanged is the checkbox, one discrete choice like the
    // quality combo.
    void rampChanged();
    void paletteAlphaChanged();
    void qualityChanged();
    void viewResized();

private:
    void buildControls();

    IsoWidget* m_view = nullptr;
    QSlider* m_lowSlider = nullptr;
    QSlider* m_highSlider = nullptr;
    QSlider* m_maximumSlider = nullptr;
    QCheckBox* m_paletteAlpha = nullptr;
    QComboBox* m_qualityCombo = nullptr;
    QCheckBox* m_boxesCheck = nullptr;
    QCheckBox* m_outlineCheck = nullptr;
    QLabel* m_lowLabel = nullptr;
    QLabel* m_highLabel = nullptr;
    QLabel* m_maximumLabel = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_rendering = nullptr;
};

} // namespace amrvis::qt
