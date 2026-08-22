#pragma once

#include "RangeController.hpp"

#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/data/DatasetSession.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>
#include <amrexplorer/pipeline/VolumePipeline.hpp>
#include <amrexplorer/render2d/Palette.hpp>

#include <QObject>
#include <QPointer>
#include <QString>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

class QAction;
class QTimer;
class QWidget;

namespace amrvis::qt {

class VolumeWindow;

// The volume view's state machine, extracted from MainWindow the way the
// other collaborators are: it owns the Volume Rendering... action and the
// Volume window, decides when a render is due -- a camera move, a changed
// field, level, range, palette or opacity control -- builds each request
// from what the host tells it (through Hooks) and the window's controls,
// runs it on a worker through the session (locally or on the server), and
// pushes the frame back into the window. One render is in flight at a time;
// a change during one is remembered and rendered after it. While the camera
// is moving, the view is being resized or an opacity slider is being dragged,
// the frames are half-size, one-sample drafts; once that settles, a full
// frame. Late results (a superseded generation, a closed
// window, a shutting-down host) are dropped, never displayed.
class VolumeController final : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        // The open dataset, or null.
        std::function<std::shared_ptr<DatasetSession>()> dataset;
        // The selected field and its display name, or nullopt with none.
        std::function<std::optional<std::pair<FieldId, QString>>()> field;
        // The level combo's selection.
        std::function<LevelSelection()> levelSelection;
        // The range controls' selection.
        std::function<RangeController::Selection()> rangeSelection;
        // The palette in effect (colours; alpha ramp when the file had one).
        std::function<const Palette&()> palette;
        // The three slice positions and whether the planes are shown.
        std::function<std::array<double, 3>()> slicePositions;
        std::function<bool()> slicePlanesVisible;
        // True once application shutdown began: late results are dropped
        // without touching the GUI.
        std::function<bool()> isShuttingDown;
    };

    VolumeController(Hooks hooks, QObject* parent = nullptr);
    ~VolumeController() override;

    // The View menu's action: opens the window; enabled while the dataset
    // can be volume-rendered (a 3-D plotfile, and for a remote one a server
    // speaking protocol 1.2). Owned by `parent`.
    QAction* createAction(QObject* parent);
    // The window (one at a time), parented to `parent` for placement only.
    void showWindow(QWidget* parent);
    void closeWindow();
    [[nodiscard]] bool windowOpen() const noexcept;

    // Host notifications. configureForDataset: a dataset opened or a sequence
    // frame arrived -- the geometry is pushed and, with the window open, a
    // frame rendered with the same camera. refresh: field, level, range, log
    // or palette changed. slicePositionsChanged / slicePlanesVisibilityChanged:
    // overlay-only, no render. reset: the dataset is going away -- cancel,
    // close the window, disable the action. cancel: in-flight work is
    // abandoned (a frame switch, shutdown); the window stays.
    void configureForDataset();
    void refresh();
    void slicePositionsChanged();
    void slicePlanesVisibilityChanged();
    void reset();
    void cancel();

    // The last frame displayed (empty until one is), for tests.
    [[nodiscard]] const VolumeFrame& lastFrame() const noexcept { return m_lastFrame; }
    [[nodiscard]] bool renderInFlight() const noexcept { return m_inFlight; }

signals:
    // A render started (+1) or ended (-1), for the host's activity count.
    void renderActivityChanged(int delta);
    void renderFailed(const QString& message);
    void statusMessage(const QString& message, int timeoutMs);
    void staleResultDropped();
    void frameDisplayed();

private:
    void refreshActionEnabled();
    void scheduleRender();
    void startRender();
    void pushGeometry();
    [[nodiscard]] QString describe(
        const VolumeDisplayResult& result, const QString& fieldName) const;

    Hooks m_hooks;
    QPointer<QAction> m_action;
    QPointer<VolumeWindow> m_window;
    QTimer* m_debounce = nullptr;
    QTimer* m_settle = nullptr;
    StopSource m_stopSource;
    std::uint64_t m_generation = 0;
    bool m_inFlight = false;
    bool m_rerun = false;
    bool m_interacting = false;
    // Whether the open window has shown a frame yet: only then is a resize
    // stretching something, and only then is it worth a draft.
    bool m_frameShown = false;
    VolumeFrame m_lastFrame;
};

} // namespace amrvis::qt
