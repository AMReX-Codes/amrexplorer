#pragma once

#include "RangeController.hpp"

#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/data/DatasetSession.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>
#include <amrexplorer/pipeline/VolumePipeline.hpp>
#include <amrexplorer/render2d/Palette.hpp>

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QSize>
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

// The pixel size to ray-cast the volume at: the view in *device* pixels for a
// frame that will stay up, half the view's *logical* size for a draft. Both
// bounded to at least one pixel and at most maxVolumeOutputDimension.
//
// Device pixels for the settled frame because a frame rendered at logical size
// is stretched into a view whose wireframe and slice planes are drawn at the
// display's real resolution -- a soft picture under sharp lines, and no
// Quality setting recovers it, because the pixels were never asked for. The
// slice path has always sized its requests this way
// (MainWindow::viewportPixelSize); the volume path had not.
//
// Drafts deliberately ignore the ratio. They exist to keep a moving camera and
// a playing sequence responsive, and scaling them would multiply the cost of
// exactly the frames that get thrown away: a draft is the same size on a
// hi-DPI display as anywhere else, and only the frame that stays gets the
// extra pixels. So the step from draft to settled is larger on such a display
// than on a 1x one -- which is the point, since that is where the settled
// frame was worst.
//
// The ratio is a parameter rather than read from the widget here, which is
// what lets the rule be tested without a scaled platform.
[[nodiscard]] std::array<int, 2> volumeOutputSize(
    QSize viewSize, qreal devicePixelRatio, bool draft) noexcept;

// The volume view's state machine, extracted from MainWindow the way the
// other collaborators are: it owns the Volume Rendering... action and the
// Volume window, decides when a render is due -- a camera move, a changed
// field, level, range, palette or opacity control -- builds each request
// from what the host tells it (through Hooks) and the window's controls,
// runs it on a worker through the session (locally or on the server), and
// pushes the frame back into the window. One render is in flight at a time;
// a change during one is remembered and rendered after it. While the camera
// is moving, the view is being resized, an opacity slider is being dragged or
// a plotfile sequence is playing, the frames are half-size, one-sample drafts;
// once that settles, a full frame. Late results (a superseded generation, a
// closed window, a shutting-down host) are dropped, never displayed.
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
        // True while plotfile-sequence playback is running. Frames arrive
        // faster than a full ray cast finishes, so they are drafted like a
        // moving camera and the frame already up is left in place until the
        // next draft replaces it.
        std::function<bool()> sequencePlaying;
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
    // abandoned (shutdown, a dataset going away); the window stays.
    void configureForDataset();
    // A sequence frame switch has begun, before the frame has loaded: the work
    // in flight was built for the outgoing frame and is abandoned. While
    // playback runs the pending render is left alone -- this happens once per
    // frame, and stopping the throttle here only to re-arm it when the frame
    // arrives pushes that render out by a full interval every time, so at the
    // frame intervals the Speed slider allows it never elapses and nothing
    // renders at all. A single step is not on a clock and takes the cancel()
    // form, which leaves nothing pending against the frame being replaced.
    void frameSwitchStarted();
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
    void pushPalette();
    // The window is going away, closed here or by the user: abandon the render
    // in flight and forget that a frame was ever shown in it.
    void forgetWindow();
    // Abandons whatever is in flight -- its result is dropped as stale -- and
    // leaves the render throttle armed. cancel() is this plus stopping the
    // throttle and the interaction it stands for; sequence playback needs
    // only this half, because stopping the throttle and starting it again on
    // every frame means it never elapses at the frame intervals the Speed
    // slider allows, and nothing renders at all.
    void abandonInFlight();
    [[nodiscard]] QString describe(
        const VolumeDisplayResult& result, const QString& fieldName) const;

    Hooks m_hooks;
    QPointer<QAction> m_action;
    QPointer<VolumeWindow> m_window;
    QTimer* m_debounce = nullptr;
    QTimer* m_settle = nullptr;
    // Kept so closeWindow can drop the window's destroyed handler: that runs a
    // turn later, when m_window may already be a newly opened window.
    QMetaObject::Connection m_windowDestroyed;
    // The view size the last render was built for. A resize to the same size
    // is layout churn, not a resize, and rendering for it would be a loop fed
    // by its own output (showFrame writes a label in the same dock).
    QSize m_lastRenderViewSize;
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
