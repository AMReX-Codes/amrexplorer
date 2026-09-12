#pragma once

#include "AspectMode.hpp"
#include "MappedGeometry.hpp"
#include "PairGeometry.hpp"
#include "DatasetWindow.hpp"
#include "ExportFrame.hpp"
#include "ImageView.hpp"
#include "NumberFormat.hpp"
#include "SetContoursDialog.hpp"

#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/data/DatasetSession.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/pipeline/DisplayCoordinator.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>
#include <amrexplorer/pipeline/SliceRangeResolver.hpp>
#include <amrexplorer/render2d/Contours.hpp>
#include <amrexplorer/render2d/ImageBuffer.hpp>
#include <amrexplorer/render2d/Palette.hpp>
#include <amrexplorer/render2d/VectorGlyphs.hpp>

#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QMainWindow>
#include <QRectF>
#include <QSize>

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

class QAction;
class QActionGroup;
class QCloseEvent;
class QComboBox;
class QDockWidget;
class QCheckBox;
class QLabel;
class QToolBar;
class QSpinBox;
class QLineF;
class QMenu;
class QProgressDialog;
class QPushButton;
class QStackedWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QRectF;
class QWidget;

namespace amrvis {
struct DatasetMetadata;
struct LineResult;
namespace remote {
class Connection;
class RemoteDatasetSession;
}
enum class CompositionPolicy : std::uint8_t;
}

namespace amrvis::qt {

class AnimationExporter;
class AnimationPanel;
class ColorBarWidget;
class DatasetWindow;
class FabSelectorDock;
class VolumeWindow;
class ImageView;
class IsoWidget;
class LinePlotWindow;
class ScientificDoubleSpinBox;
class DiagnosticsModel;
class FabNavigator;
class DerivedFieldController;
class PaletteController;
class ParticleController;
class RangeController;
class RemoteSessionController;
class SequenceController;
class ThemeController;
class VolumeController;
struct PlaneMapping;
class UserGuideDialog;

// These now live in the Qt-free pipeline layer (SlicePipeline.hpp); re-export
// them in this namespace so amrvis::qt::X keeps resolving for callers and
// tests. DisplayMode is re-exported by SetContoursDialog.hpp.
using amrvis::RangeMode;
using amrvis::SliceDisplayResult;
using amrvis::InitialSliceResult;
using amrvis::FrameSliceSpec;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openDataset(const std::filesystem::path& path, bool metadataOnly = false);
    // The remote session (ssh-launched server, its connection, the Open
    // Remote dialogs and browser) lives in RemoteSessionController; these two
    // forward to it for the command line and the test harnesses. See
    // RemoteSessionController::install and ::start.
    void useRemoteConnection(
        std::shared_ptr<remote::Connection> connection, QString label);
    // `companion` names a plotfile on the same server to show beside the
    // one path once its slices are up (the --ssh ... --companion form);
    // tied to that load, so a startup that fails leaves nothing waiting.
    void startSshRemoteSession(std::string destination,
        std::string serverExecutable, std::vector<std::string> remotePaths,
        std::string companion = {});
    // Open a server-visible path, or a sequence of them, over the installed
    // remote connection.
    void openRemoteDataset(std::string remotePath);
    void openRemoteSequence(const std::vector<std::string>& remotePaths);
    // Opens a plotfile sequence (the legacy "-a" file animation): frames are
    // the plotfile directories, sorted by name; requires at least two valid
    // plotfiles. Opening a single dataset closes the sequence again.
    void openSequence(const std::vector<std::filesystem::path>& frames);
    // Show a second 3-D plotfile beside the open one: the two must share a
    // plane (see PairGeometry). The companion is a secondary load onto the
    // installed dataset; it is closed again by closeCompanion, by any other
    // open, and when its geometry cannot pair. Emits companionOpenFinished
    // either way. A local plotfile pairs with either kind of primary; a
    // remote one (openRemoteCompanion) comes over the primary's own
    // connection when the primary is remote, else over the window's remote
    // session. Plotfiles on two servers cannot pair.
    void openCompanion(const std::filesystem::path& path);
    void openRemoteCompanion(std::string remotePath);
    void closeCompanion();
    [[nodiscard]] bool companionOpen() const noexcept { return m_layers[1].active; }
    // Steps the open sequence by direction frames, wrapping at the ends; the
    // same slot the sequence step buttons and the smoke test hook use.
    void stepSequence(int direction);
    // Starts an animation export without the interactive color-bar/save
    // dialogs, writing frames and MP4s under path's directory. Test-only entry
    // used by the export-quit smoke test to reach the encoder deterministically.
    void startAnimationExportForTest(const QString& path, bool includeColorBar,
                                     bool includeAxes = false, bool transparentBackground = false);
    // Test-only sequence probes: whether the Animation dock is on screen, and
    // whether sequence playback is still running. A frame refresh must not
    // reassert the first, and a failed frame must clear the second.
    [[nodiscard]] bool animationDockVisibleForTest() const;
    [[nodiscard]] bool sequencePlayingForTest() const noexcept
    {
        return m_playbackMode == PlaybackMode::Sequence;
    }
    void setAnimationDockVisibleForTest(bool visible);
    void toggleSequencePlaybackForTest() { toggleSequencePlayback(); }
    // The plane sweep, which unlike sequence playback never reopens the
    // dataset: a change that needs a reload has to be given one outright.
    void toggleSweepPlaybackForTest() { toggleSweepPlayback(); }
    [[nodiscard]] bool sweepPlayingForTest() const noexcept
    {
        return m_playbackMode == PlaybackMode::Sweep;
    }
    // Runs on the GUI thread just after an initial-slice load is launched --
    // the only point at which a test can change something while one is in
    // flight, since the completion arrives as a queued signal. Defined in
    // MainWindowTestAccess.cpp, as every accessor that touches a member the
    // release build does not have is: the body cannot be inline here, where
    // AMREXPLORER_QT_TEST_ACCESS is not necessarily on.
    void setInitialSliceLaunchedHookForTest(std::function<void()> hook);
    [[nodiscard]] bool adaptivePrecisionForTest();
    // Sends the next initial-slice completion down its failure arm, standing
    // in for the reopen a server refuses or a connection that has gone. The
    // one state a test cannot reach through the widgets, and the one the
    // retry an unchanged Apply performs exists for.
    void failNextInitialSliceForTest();
    // Submits a slice for the active view the way an interaction does --
    // synchronously, bumping the view's slice generation. Paired with the hook
    // above it reproduces an interaction that lands while a reload is still
    // replacing the session, which is the one ordering no arrival-side check
    // can sort out on its own.
    void requestActiveViewSliceForTest();
    // Whether the raster on screen came from the session now installed. A
    // reload leaves the outgoing session displayed while its replacement
    // loads, so this is the invariant that says the two have converged.
    [[nodiscard]] bool activeViewSliceMatchesSessionForTest() const;
    // The sequence frame waiting in the prefetch slot, if any.
    [[nodiscard]] std::optional<int> prefetchedSequenceFrameForTest() const;
    // The slot an idle frame-slider press-and-release lands in, which must not
    // restart a frame that is already on screen.
    void requestSequenceFrameForTest(int index) { goToSequenceFrame(index); }
    // The step every field selection goes through, so a test can ask for a row
    // that is not a field -- the separator, or a definition this dataset
    // cannot provide -- and see where the selection actually lands.
    void selectFieldItemForTest(int index) { selectFieldItem(index); }
    // The vector-glyph selections, as ids into the open field list. A reload
    // carries them by name (restoreVectorFields), so a test needs to set them
    // over one dataset and read them back over the next.
    [[nodiscard]] std::array<int, 3> vectorFieldsForTest() const
    {
        return {m_vectorUField, m_vectorVField, m_vectorWField};
    }
    void setVectorFieldsForTest(int u, int v, int w)
    {
        m_vectorUField = u;
        m_vectorVField = v;
        m_vectorWField = w;
    }

    // Test-only: move each 3-D plane to slicePositions (per axis, so the three
    // panels sample different data with different local ranges), switch to
    // contour display (count levels) with the Visible range mode and optional
    // logarithmic mapping, then re-slice every view once. This is the exact
    // shape that exposed the stale-contour bug: three unequal local ranges
    // reconciled into one shared Visible range. interactiveSlicesSettled fires
    // when that batch finishes.
    void configureContourSyncForTest(
        int count, bool logarithmic, std::array<double, 3> slicePositions);
    // Test-only: change the display mode and contour count alone (range and
    // positions untouched) and re-render every view through the cache path.
    void setDisplayModeForTest(DisplayMode mode, int contourCount);

    // Test-only: drive the visible-range sync staleness guard deterministically.
    // Gate a sync mid-flight, re-render every panel through the cache path
    // (a contour-count change: the planes keep their pointers, only the
    // per-view render generation moves), then release workers one at a time:
    // first the now-stale sync, which must drop the whole outcome (tallied in
    // the test-only m_visibleSyncStaleSkips, read via
    // visibleSyncStaleSkipsForTest()); while the self-healing rerun is still
    // gated, the panels must still show the refresh's contours (the stale
    // outcome, extracted at the old count, was not applied); then release the
    // rerun. adjustActiveRequestsForTest stands in for a non-slice background
    // request (a particle load), which must not hold the sync's dispatch. See
    // the staleness smoke test and syncVisibleRanges.
    void requestVisibleSyncForTest();
    void armVisibleSyncGateForTest();
    void releaseVisibleSyncGateForTest();
    void disarmVisibleSyncGateForTest();
    // The next sync worker to run throws instead of rendering, so the
    // completion's failure path can be driven: a current failure is reported,
    // a superseded one counted stale.
    void failNextVisibleSyncForTest();
    // Test-only: hold every cache-path slice worker at a gate until released,
    // so a refresh can be made to overtake a redraw still on its way.
    void armSliceGateForTest();
    void releaseSliceGateForTest();
    void adjustActiveRequestsForTest(int delta);
    [[nodiscard]] std::uint64_t activeViewRenderGenerationForTest() const;
    [[nodiscard]] bool visibleSyncWorkerWaitingForTest() const;
    [[nodiscard]] std::uint64_t visibleSyncStaleSkipsForTest() const noexcept;

    // Test-only: for each current view (ordered by normal axis; 2-D has one),
    // the display range and the distinct contour levels present in its overlay
    // polylines. The contour-sync smoke test checks these levels are re-derived
    // from the shared Visible range rather than each view's local range.
    struct ContourViewProbe {
        double displayMinimum = 0.0;
        double displayMaximum = 0.0;
        bool logarithmic = false;
        std::vector<double> contourLevels;
    };
    [[nodiscard]] std::vector<ContourViewProbe> contourViewProbesForTest();

    // Test-only: select Visible range + Raster display and re-slice the full
    // domain, so the full-domain range is cached. Pair with zoomActiveViewForTest
    // to drive the 2-D range-reuse raster path. interactiveSlicesSettled fires
    // when the re-slice completes.
    void enableVisibleRasterForTest();

    // Test-only: zoom the active view to the upper-value quadrant — a strict
    // subregion whose local range differs from the full domain — and re-slice.
    // With Visible mode active and the full-domain range cached, this exercises
    // the reuse path that must re-render the raster to match the color bar.
    void zoomActiveViewForTest();

    // Test-only: the placeholder every panel is showing, or an empty string if
    // any panel holds an image instead. A failed open must leave a settled
    // placeholder naming what failed, never the "Loading..." one it replaced.
    [[nodiscard]] QString viewPlaceholderForTest();

    // Test-only: true when the active view's displayed raster is byte-identical
    // to its plane re-rendered against the current display (color-bar) range —
    // i.e. the raster and color bar agree. See
    // raster-colorbar-mismatch-on-2d-visible-zoom.
    [[nodiscard]] bool activeViewRasterMatchesDisplayRangeForTest();
    [[nodiscard]] bool activeViewUsesViewportBoundedOutputForTest() const;
    [[nodiscard]] bool activeViewUsesNativeOutputForTest() const;
    [[nodiscard]] bool allViewsUseViewportBoundedOutputForTest() const;
    // Test-only: true when every current panel is in fixed-scale mode with
    // everything its viewport shows of the domain backed by the raster —
    // full bleed with no unfetched gaps.
    [[nodiscard]] bool allViewsFixedScaleRasterCoversViewportForTest() const;
    // Test-only: slice requests currently on a worker across the current
    // panels. Zero means the displayed raster is not about to be replaced by
    // work already in flight, which is what a probe reading the raster (or the
    // window it implies) needs before it measures. Pair it with a settle to
    // absorb a request that only queues its successor.
    [[nodiscard]] int slicesInFlightForTest() const;
    // True while a slice request is queued behind the debounce but not yet on a
    // worker; a settle-driven wait must treat this as "not converged".
    [[nodiscard]] bool sliceRequestPendingForTest() const;
    // Test-only: send a real Shift+left drag through the active view's
    // viewport, exercising the same event path as interactive panning.
    void shiftDragActiveViewForTest(int dx, int dy);
    // Test-only: a plain right click at a viewport position of the active
    // view, as the mouse would deliver it.
    void rightClickActiveViewForTest(const QPoint& viewportPosition);
    [[nodiscard]] bool activeViewScrollBarsVisibleForTest() const;
    [[nodiscard]] bool activeViewHasPhysicalAspectForTest(
        double expectedAspect) const;
    // Test-only: the active view's raster has the aspect of the region it
    // covers measured in finest cells -- the raster's unit, one sample per
    // cell -- rather than in physical units. The two differ only when the
    // cells are not square (see remote-fit-anisotropic-cells); a physical
    // proportion is a view-side stretch and leaves the raster alone.
    [[nodiscard]] bool activeViewRasterHasCellAspectForTest() const;
    // Test-only: a second independent top-level window, made exactly as the
    // "Open New Window" menu action makes it, for the close-window test to
    // close again.
    MainWindow* createNewWindowForTest() { return createNewWindow(); }
    // Test-only: the Volume Rendering window -- open it as the View menu
    // action does, whether it is open, and what fraction of the last frame's
    // pixels the ray caster lit (alpha > 0); zero before any frame.
    void showVolumeWindowForTest();
    [[nodiscard]] bool volumeWindowOpenForTest() const;
    [[nodiscard]] double volumeFrameAlphaCoverageForTest() const;
    // Test-only: the open Volume Rendering window, or null, so a harness can
    // drive its controls the way a user would.
    [[nodiscard]] VolumeWindow* volumeWindowForTest() const;
    [[nodiscard]] bool fabStateClearedForTest() const;
    // Test-only: how many failures have been reported non-modally. The FAB
    // rollback smoke tests assert on this so a passing run proves the failure
    // branch actually ran rather than the read having quietly succeeded.
    // Saturates: reportBackgroundError keeps only the newest 50, so a test that
    // waits for this to exceed a baseline of 50 would wait forever. Both
    // current callers start from an empty list.
    [[nodiscard]] int backgroundErrorCountForTest() const;
    // Test-only: the direct "open a raw FAB file" entry point, reached in the
    // app through a file dialog. Supplies no rollback of its own, which is what
    // makes it the interesting case when it supersedes a selector click.
    void openStandaloneFabForTest(const std::filesystem::path& path);
    void setGridBoxesVisibleForTest(bool visible);
    [[nodiscard]] std::size_t activeViewGridBoxCountForTest() const;
    // Test-only: View > Slice Planes, and the guide lines the active view
    // draws for the other two planes.
    void setSlicePlanesVisibleForTest(bool visible);
    [[nodiscard]] bool slicePlanesVisibleForTest() const;
    [[nodiscard]] std::size_t activeViewCrosshairCountForTest() const;

    // Test-only: rubber-band the central half of the active 3-D panel through
    // the same handler used by ImageView::rubberBandSelected.
    void rubberBandZoomActiveViewForTest();
    void rubberBandZoomRectangularActiveViewForTest();
    void rubberBandZoomTallActiveViewForTest();
    // Whether the active view's raster is fitted flush to the pane: within
    // the viewport (no overflow) and touching both borders on the limiting
    // axis, up to fitInView's built-in margin.
    [[nodiscard]] bool activeViewRasterSnugForTest() const;

    // Test-only: true when every current panel has a strict visible subregion.
    // Used to lock down synchronized 3-D rubber-band zoom.
    [[nodiscard]] bool allViewsRubberBandZoomedForTest();
    [[nodiscard]] std::size_t rubberBandZoomedViewCountForTest();

    // Test-only: apply a panel-local scale, drive the exact data-region pan
    // handlers used by Shift+left drag, and inspect the resulting transform.
    // Counts arrow-key pan *requests* that reached a view, which is what the
    // routing regression is about -- not pans that moved something, since a
    // step at the domain edge legitimately moves nothing. The unit test cannot
    // see routing at all: it delivers events to the view directly.
    [[nodiscard]] std::size_t panStepRequestsForTest() const noexcept
    {
        return m_panStepRequests;
    }
    [[nodiscard]] bool activeViewHasFocusForTest() const;
    void focusLevelSelectorForTest();
    void clearFocusForTest();
    // Test-only: the fixture really opened as a spherical view. Without this
    // the spherical exclusion could be "tested" against a Cartesian view.
    [[nodiscard]] bool displayIsSphericalForTest() const
    {
        return displayIsSpherical();
    }
    // Test-only: drive the View > 2-D Spherical > Display group the way the
    // menu does, so a test can reach the unwarped r-theta and theta-r modes.
    // Only R-Z warps, and the scale report turns on that distinction.
    void selectSphericalDisplayForTest(int mode);
    [[nodiscard]] bool displayIsSphericalWarpForTest() const
    {
        return displayIsSphericalWarp();
    }
    void resetZoomAllViewsForTest() { resetZoomAllViews(); }
    void setActiveViewScaleForTest(int factor);
    void selectFixedScaleForTest(int factor);
    // The same choice made from the *toolbar* button's own menu rather than
    // View > Scale. Both must leave the same state; only the View-menu path was
    // covered before, which is why the toolbar's one-way sync went unnoticed.
    void selectToolbarFixedScaleForTest(int factor);
    // What the Scale button currently reports.
    [[nodiscard]] QString scaleUiLabelForTest() const;
    // The View > Scale item currently checked, with its mnemonic stripped, or
    // an empty string when nothing is. The button label and this can differ in
    // wording -- a clamped label says "32x->16x" -- but they must never
    // disagree about which factor is selected.
    [[nodiscard]] QString scaleMenuCheckedLabelForTest() const;
    // The magnification the UI claims for this factor (0 when it is literal),
    // and the finest cell size along the active view's horizontal axis, so a
    // test can check the claim against the window the view actually shows.
    [[nodiscard]] double effectiveFixedScaleForTest(int factor) const
    {
        return effectiveFixedScale(factor);
    }
    [[nodiscard]] double activeViewFinestCellSizeForTest() const;
    // The dataset's physical domain in the active view's two display axes, so
    // a test can say where "centred" is.
    [[nodiscard]] QRectF datasetPhysicalDomainForTest() const;
    // Test-only: send a real wheel event through the active view's viewport,
    // exercising the same zoomBy path a user's scroll wheel takes. Positive
    // notches zoom in.
    void wheelActiveViewForTest(int notches);
    // Test-only: true when the active view still hosts a whole-domain virtual
    // canvas. A wheel zoom leaves the canvas in place and only changes the
    // scale, so every scene-to-physical reader has to cope with a canvas in
    // Custom mode.
    [[nodiscard]] bool activeViewVirtualCanvasActiveForTest() const;
    [[nodiscard]] bool fixedScaleStateMatchesForTest(int factor) const;
    void wheelZoomAndPanActiveViewForTest();
    [[nodiscard]] QRectF activeViewVisibleDataWindowForTest() const;
    // The part of the active view's pixmap inside the viewport, in pixmap
    // pixels (the warp's own pixels on a mapped grid).
    [[nodiscard]] QRectF activeViewVisibleImageRectForTest() const;
    [[nodiscard]] RealBox volumeRegionOfInterestForTest() const
    {
        return volumeRegionOfInterest();
    }
    void panActiveViewForTest(double sceneDeltaX, double sceneDeltaY);
    [[nodiscard]] qreal activeViewScaleForTest() const;
    // Test-only: compare the current transform with ImageView's own fitted
    // transform. The check leaves the view fitted.
    [[nodiscard]] bool activeViewIsFitToWindowForTest();

    // Test-only: true when the active view's whole raster lies inside the
    // viewport — i.e. the displayed image is fully visible, however it got
    // framed. A cropped-region arrival that over-zooms (issue #45) leaves part
    // of the raster outside the viewport and fails this.
    [[nodiscard]] bool activeViewShowsWholeImageForTest() const;

    // Test-only: drill into the FAB catalog entry at index (the same path the
    // dock's viewRequested signal drives). Used by the FAB round-trip zoom test.
    void viewFabForTest(std::size_t index);

    // Test-only: true when the active view holds a zoom (visibleRegion set).
    // See fab-round-trip-loses-visible-region.
    [[nodiscard]] bool activeViewIsZoomedForTest() const;

    // Test-only: the active view's pixmap width, and whether it is at
    // fit-to-window without mutating it (unlike activeViewIsFitToWindowForTest,
    // which refits as a side effect).
    [[nodiscard]] int activeViewImageWidthForTest() const;
    [[nodiscard]] std::array<int, 2> activeViewImageSizeForTest() const;
    [[nodiscard]] std::array<int, 2> activeViewViewportSizeForTest() const;
    [[nodiscard]] QImage activeViewViewportImageForTest() const;
    void setScaleBarVisibleForTest(bool visible);
    [[nodiscard]] bool scaleBarActionEnabledForTest() const;
    [[nodiscard]] bool activeViewHasScaleBarForTest() const;
    [[nodiscard]] bool activeViewFitsWindowForTest() const;
    // Test-only: the Aspect Ratio controls, driven as the menu and the Axis
    // Scaling dialog drive them, and the stretch they leave on the active
    // view: its vertical screen pixels per scene unit over its horizontal.
    void setAspectModeForTest(AspectMode mode) { setAspectMode(mode); }
    void setAxisScaleForTest(const std::array<double, 3>& axisScale)
    {
        applyAxisScale(axisScale);
    }
    [[nodiscard]] bool aspectMenuEnabledForTest() const;
    [[nodiscard]] double activeViewStretchRatioForTest() const;
    // The isometric view's outlined domain in its display coordinates.
    [[nodiscard]] RealBox isoDomainDisplayBoxForTest() const;
    // Test-only: the Aspect Ratio radio shown checked (the mode in effect,
    // which a mapped grid pins to Physical Size without touching the
    // preference) and whether both radios are offered.
    [[nodiscard]] AspectMode aspectMenuCheckedModeForTest() const;
    [[nodiscard]] bool aspectRadiosEnabledForTest() const;
    [[nodiscard]] AspectMode aspectModePreferenceForTest() const noexcept
    {
        return m_aspectMode;
    }
    // Test-only: View > Mapped Grid, driven as the menu drives it, and what
    // the active view shows: mapped (the physical warp) or the logical grid.
    void setMappedGridForTest(bool enabled);
    [[nodiscard]] bool mappedGridMenuEnabledForTest() const;
    [[nodiscard]] bool displayIsMappedForTest() const;
    [[nodiscard]] bool activeViewIsMappedForTest() const;
    // The physical window the active view's mapped warp was last requested
    // for, on the panel's two axes (x = first in-plane axis); empty until
    // the demand loop has asked for one.
    [[nodiscard]] QRectF activeViewMappedWindowForTest() const;
    // Test-only: a 3-D panel's mapped warp, read without making the panel
    // active (its border would resize the view): mapped, Fit, re-sliced; the
    // window drawn (as above); the image; the tile on screen in device
    // pixels; the tile, canvas and visible rect in scene units; the scale.
    struct MappedPanelForTest {
        bool warped = false;
        bool fit = false;
        bool resliced = false;
        QRectF window;
        QRectF drawn;  // the window the pixmap on screen was drawn for
        QSize image;
        QRectF tileDevice;
        QRectF tile;
        QRectF canvas;
        QRectF visible;
        double scale = 0.0;
    };
    [[nodiscard]] MappedPanelForTest mappedPanelForTest(int normal) const;
    // Test-only: the active view's transform scale and scroll position
    // {m11, m22, h, v}; a scroll by viewport pixels, as a drag or the scroll
    // bars would; and the physical region of the plane it holds (x = first
    // in-plane axis).
    [[nodiscard]] std::array<double, 4> activeViewTransformAndScrollForTest() const;
    void scrollActiveViewForTest(int dx, int dy);
    [[nodiscard]] QRectF activeViewPlaneRegionForTest() const;
    // Test-only: make the 3-D panel with this normal the active view.
    void setActiveViewForTest(int normal);
    // Test-only: the probe readout for a pixmap pixel of the active view
    // (x from the left, y from the top), as the status bar would show it.
    [[nodiscard]] QString probeReadoutActiveViewForTest(int x, int y) const;
    // Test-only: the companion (paired) display. Tiles are indexed by layer;
    // rects are in the panel's scene units.
    [[nodiscard]] int panelTileCountForTest(int normal) const;
    [[nodiscard]] QRectF panelTileRectForTest(int normal, int tile) const;
    [[nodiscard]] bool panelTileVisibleForTest(int normal, int tile) const;
    [[nodiscard]] QString layerFieldNameForTest(int layer, int normal) const;
    // The Log setting a layer's next slice request will carry.
    [[nodiscard]] bool layerLogarithmicSelectedForTest(int layer) const;
    [[nodiscard]] std::pair<double, double> layerDisplayRangeForTest(
        int layer, int normal) const;
    [[nodiscard]] bool companionColorBarVisibleForTest() const;
    [[nodiscard]] bool layerSessionIsRemoteForTest(int layer) const;
    void selectLayerFieldItemForTest(int layer, int index);
    [[nodiscard]] QString layerSelectedFieldForTest(int layer) const;
    // Paired zoom: a scene-rect selection on a panel, that panel's tile
    // raster sizes, and its scene rect (the pair canvas as framed).
    void rubberBandZoomPanelSceneForTest(int normal, const QRectF& sceneRect);
    [[nodiscard]] QSize panelTileImageSizeForTest(int normal, int tile) const;
    [[nodiscard]] QRectF panelCanvasRectForTest(int normal) const;
    [[nodiscard]] QSize panelExportSizeForTest(int normal) const;
    void panStepActiveViewForTest(const QPointF& direction);
    // A panel's view transform scale (m11, m22) and whether it sits on a
    // virtual canvas, for panels other than the active one.
    [[nodiscard]] std::pair<qreal, qreal> panelTransformScaleForTest(int normal) const;
    [[nodiscard]] bool panelVirtualCanvasActiveForTest(int normal) const;
    void setSlicePositionForTest(int axis, double value)
    {
        setSlicePosition(axis, value);
    }
    void setCompanionPerpendicularScaleForTest(double factor);
    [[nodiscard]] double slicePositionForTest(int axis) const
    {
        return m_slicePosition3d[static_cast<std::size_t>(std::clamp(axis, 0, 2))];
    }

    // Test-only: shrink the open dataset's cache budget to force cache-pressure
    // fallback on the next non-cache slice, and read the current resident bytes
    // to size that budget. See cache-budget-exceeded-hard-fails-after-load.
    void setCacheBudgetForTest(std::uint64_t bytes);
    [[nodiscard]] std::uint64_t cacheResidentBytesForTest() const;

    // Test-only: apply particle settings through the same synchronization path
    // as the dialog, and inspect how many points are currently installed.
    void setParticleSelectionForTest(
        std::vector<std::string> species, double fraction,
        std::uint64_t seed = 0);
    [[nodiscard]] std::uint64_t particleSeedForTest() const noexcept;
    [[nodiscard]] double particleFractionForTest() const noexcept;
    void setParticlePointSizeForTest(int pointSize);
    [[nodiscard]] int particlePointSizeForTest() const noexcept;
    void setParticleSliceCellsOnlyForTest(bool sliceCellsOnly);
    [[nodiscard]] bool particleSliceCellsOnlyForTest() const noexcept;
    // Invalid when the species has no stored color, which is what a reset
    // leaves behind until the next dataset re-seeds the defaults.
    [[nodiscard]] QColor particleColorForTest(const std::string& species) const;
    void setParticleColorForTest(
        const std::string& species, const QColor& color);
    [[nodiscard]] bool particleOverlaysUseColorForTest(
        const QColor& color);
    [[nodiscard]] std::size_t particleSampleCountForTest() const;
    // Point batches (one per drawn species) and the points in them: the
    // slice-cell filter thins the batches without emptying them.
    [[nodiscard]] std::size_t particleOverlayCountForTest();
    [[nodiscard]] std::size_t particleOverlayPointCountForTest();
    [[nodiscard]] bool particleLoadingForTest() const noexcept;
    [[nodiscard]] bool particleLoadingUiActiveForTest() const;
    [[nodiscard]] bool particleLoadingUiSettledForTest() const;

signals:
    void datasetOpenFinished(bool success);
    void initialSliceFinished(bool success);
    // Emitted once a companion's slices are on screen, or when its load or
    // pairing failed (the primary stays as it was).
    void companionOpenFinished(bool success);
    // Emitted when an interactive re-slice batch (a mode/range/log/field
    // change, pan, or zoom) finishes with no slice work left in flight. The
    // contour-sync smoke test waits on it. Not emitted for the initial load.
    void interactiveSlicesSettled();
    // Emitted once a sequence frame's slice(s) are on screen; the offscreen
    // smoke test drives frame stepping off it.
    void sequenceFrameDisplayed(int index);
    void sequenceFrameFailed();
    // Emitted when the Volume Rendering window has drawn a frame; the volume
    // smoke tests wait on it.
    void volumeFrameDisplayed();
    // Emitted when the FFmpeg encoding phase begins (frames rendered, encoder
    // workers about to run); the export-quit smoke test quits on it to exercise
    // bounded encoder cancellation.
    void exportEncodingStarted();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    // Everything that used to be singular about the displayed slice, per
    // view: the 2-D stacked page owns one of these, the 3-D grid owns three
    // (one per plane normal, indexed by normal axis). Each view runs its own
    // async slice pipeline (stop source + generation) so moving one slice
    // plane only re-slices the view normal to it.
    struct PlaneViewState {
        ImageView* view = nullptr;
        int normal = 1;
        QString label;      // "2-D" / "YZ" / "XZ" / "XY"
        // The dataset layer this state slices for, and the tile of `view` it
        // draws into: 0 for the primary dataset, 1 for a companion.
        std::size_t layer = 0;
        std::size_t tile = 0;
        // The displayed plane and its contour-mode companions are immutable
        // shared snapshots, never null (empty planes when nothing is shown),
        // never mutated in place. An executeSlice arrival installs a *fresh*
        // pointer; a cache-path refresh (palette/log/range) re-installs the
        // *same* pointer it was built from — the refcount bump that replaces
        // the former ~110 MB deep copy. So pointer identity is NOT a proxy for
        // "same rendering settings": a staleness guard keyed on identity alone
        // fails open across a cosmetic refresh (this exact bug bit
        // syncVisibleRanges — gate on the rerun flag or a render generation
        // instead). The cached-planes refresh worker captures these shared_ptrs
        // and keeps reading its snapshots safely while a newer arrival swaps the
        // view's pointers.
        std::shared_ptr<const ScalarPlane> plane
            = std::make_shared<const ScalarPlane>();
        // Contour-mode companions of plane: the data-resolution plane the
        // contours were extracted from, its bilinear refinement, and the
        // display-space polylines. Cleared and updated exactly where plane
        // is; together with the cache key below they let range, palette,
        // and contour-count changes refresh without a new SliceQuery.
        std::shared_ptr<const ScalarPlane> contourPlane
            = std::make_shared<const ScalarPlane>();
        std::vector<ContourPolyline> contourPolylines;
        QString fieldName;
        std::optional<RealBox> visibleRegion;
        // Dataset coordinate system (AMReX Header code). 2 (spherical) means
        // `plane` holds logical (r, theta) data that `view` displays warped
        // into physical (R, Z), with displayRegion giving that warped raster's
        // (R, Z) bounds. For every other system displayRegion equals the
        // plane's physical region and no warp occurs, so overlays and the probe
        // can map through displayRegion uniformly.
        int coordinateSystem = 0;
        SphericalDisplay sphericalDisplay = SphericalDisplay::RZ;
        RealBox displayRegion;
        // The display region the pixmap on screen was drawn for: displayRegion
        // follows every arrival, this one only those that brought a raster.
        RealBox pixmapRegion;
        // The raster on screen was drawn warped (DisplayWarp): the pixmap is
        // physical and uniform over displayRegion while `plane` stays
        // logical. displaySourceIndex, parallel to the pixmap with row 0 at
        // the bottom, says which plane pixel each pixmap pixel came from;
        // gridNodes (a mapped grid only) places plane pixels (bilinear). Both
        // shared with the arrival that produced them, never copied.
        DisplayWarp warp = DisplayWarp::None;
        std::shared_ptr<const MappedGridPlane> gridNodes;
        std::shared_ptr<const std::vector<std::int32_t>> displaySourceIndex;
        // The canvas a mapped view is laid out on: the node bounding box of
        // the whole domain on this panel's axes, grown by every arrival's
        // bounds and never shrunk, so the anchor of the scene (MappedLayout)
        // holds still while the slice, the region and the window change.
        std::optional<RealBox> mappedCanvasBounds;
        // The physical window and device-pixel size the demand loop last
        // asked the warp for (SliceRequest::displayWindow / displayPixels);
        // the window is empty until it has asked.
        RealBox mappedWindow;
        std::array<int, 2> mappedWindowPixels{0, 0};
        // The node bounding box of the plane on hand
        // (SliceDisplayResult::mappedBounds): how far it can serve a window.
        RealBox mappedNodeBounds;
        std::optional<DisplayCoordinator::RasterGeometry> rasterGeometry;
        double displayMinimum = 0.0;
        double displayMaximum = 1.0;
        bool displayLogarithmic = false;
        std::vector<VectorSegment> vectorSegments;
        std::vector<SliceGridBox> gridBoxes;
        // The Dataset window cell this view is marking, held so showSlice can
        // put the red outline back after setImage clears the scene -- the same
        // reason it rebuilds the grid boxes, overlays and crosshairs. Asking
        // the Dataset table for it again is not an option: the selection that
        // produced it is still up, and Qt emits no selectionChanged for a cell
        // that is already selected, so nothing would report it.
        std::optional<RealBox> datasetCell;
        // Cache key of the slice that produced the planes above: a UI change
        // that leaves every key field untouched (palette/log/range/contour
        // count) is satisfied from the cached planes instead of querying
        // again (see requestSlice).
        SliceRequest cachedRequest{};
        bool hasCachedRequest = false;
        DisplayMode cachedMode = DisplayMode::Raster;
        std::uint32_t cachedVectorUField = 0;
        std::uint32_t cachedVectorVField = 0;
        int cachedContourCount = 0;
        StopSource stopSource;
        // The session epoch `plane` was produced under. A reload replaces the
        // session while the outgoing one is still displayed, so this is what
        // says whether the raster on screen belongs to the session now
        // installed. Derived state rather than a debt flag on purpose: a
        // pending re-slice can be discarded by the next reload clearing the
        // debounce, and a comparison cannot be.
        std::uint64_t planeSessionEpoch = 0;
        std::uint64_t sliceGeneration = 0;
        // Bumped every time `plane` (and its contour companions) is rewritten:
        // each showSlice apply and each dataset reset. The 3-D visible-range
        // sync snapshots this per panel at dispatch and, at completion, applies
        // its shared range/log across all three panels only if *every* panel's
        // stamp still matches -- otherwise it drops the whole outcome (the
        // union would be over a superseded plane) and the rerun recomputes one.
        // This is the staleness key cached-plane reuse defeated for pointer
        // identity; distinct from sliceGeneration, which bumps at *request
        // dispatch* (before the plane lands) and so would mark a not-yet-arrived
        // panel current. See syncVisibleRanges.
        std::uint64_t renderGeneration = 0;
        // Slice requests currently on a worker for this view; the sweep
        // playback skips ticks while one is in flight.
        int pendingRequests = 0;
    };

    // One dataset shown in this window and everything that belongs to it
    // alone: its session and catalog, its field and level selectors, its
    // range controls and colour bar, and one PlaneViewState per 3-D panel.
    // The primary layer is always index 0; a companion dataset occupies
    // index 1 while one is open. The array is fixed so every PlaneViewState
    // keeps its address for the lambdas that capture it.
    struct DatasetLayer {
        bool active = false;
        std::shared_ptr<DatasetSession> session;
        std::shared_ptr<const DatasetMetadata> openMetadata;
        std::string fileVersion;
        std::filesystem::path path;
        QString name;
        QComboBox* fieldSelector = nullptr;
        QComboBox* levelSelector = nullptr;
        // Owns the range mode, User min/max and Log widgets and the per-field
        // range memory; selection() feeds every slice request and frame spec.
        RangeController* range = nullptr;
        ColorBarWidget* colorBar = nullptr;
        // Which session is installed. Bumped wherever the session is replaced
        // or cleared, and captured by a slice request at submission: an
        // arrival whose stamp no longer matches was computed against a
        // session that is gone, and the catalog, field list and colour bar on
        // screen are the new one's.
        //
        // Note what this cannot do on its own. In the common ordering an
        // interactive slice finishes *before* the reload installs, so its
        // stamp still matches and it is rightly accepted -- it only goes stale
        // a moment later. Acceptance is therefore only half the invariant; the
        // other half is that a view whose display the reload skipped is
        // re-sliced, so no view keeps a raster from a session that is no
        // longer installed.
        std::uint64_t sessionEpoch = 0;
        // A companion's display stretch along the axis perpendicular to the
        // plane it shares with the primary (see PairGeometry). The primary's
        // own factor is m_axisScale on that axis, as with one dataset.
        double perpendicularScale = 1.0;
        std::array<PlaneViewState, 3> planeViews;
        // The 3-D visible-range sync's single-flight state (see
        // syncVisibleRanges) and the full-domain range store it defers.
        bool visibleSyncInFlight = false;
        bool visibleSyncRerun = false;
        std::optional<amrvis::DisplayCoordinator::RangeKey> pendingRangeStore;
    };

    [[nodiscard]] DatasetLayer& primary() noexcept { return m_layers[0]; }
    [[nodiscard]] const DatasetLayer& primary() const noexcept
    {
        return m_layers[0];
    }
    [[nodiscard]] DatasetLayer& layerFor(const PlaneViewState& state) noexcept
    {
        return m_layers[state.layer];
    }
    [[nodiscard]] const DatasetLayer& layerFor(
        const PlaneViewState& state) const noexcept
    {
        return m_layers[state.layer];
    }
    // Whether a state's layer shows a remote dataset. Asked per layer, since
    // a companion has a session of its own.
    [[nodiscard]] bool layerIsRemote(const PlaneViewState& state) const;

    void chooseDataset();
    // The local directory dialog, and the remote one (RemoteSessionController).
    void chooseCompanion();
    void chooseRemoteCompanion();
    // What a companion load hands back: the catalog it read, how it pairs
    // with the primary, and the rendered first slices.
    struct CompanionLoad {
        PlotfileMetadataResult metadata;
        PairGeometry geometry;
        InitialSliceResult result;
    };
    // What a companion reload puts back once the new session is in: its
    // selections, the field by name since a reinstalled list can move ids.
    struct CompanionRestore {
        QString fieldName;
        int levelData = -1;
        RangeMode rangeMode = RangeMode::File;
        std::optional<std::pair<double, double>> userRange;
        // The shared position and the companion's zoomed regions, per
        // panel, so a reload renders what was on show and a change made
        // while it loaded is told from what it rendered.
        std::array<double, 3> slicePositions{0.0, 0.0, 0.0};
        std::array<std::optional<RealBox>, 3> regions;
    };
    // The companion's selections as they stand now, in the same shape.
    [[nodiscard]] CompanionRestore currentCompanionSelections() const;
    // Whether a reload's selections still stand: the same field, level,
    // range, shared position and zoom as when its load was sent.
    [[nodiscard]] static bool sameCompanionSelections(
        const CompanionRestore& a, const CompanionRestore& b);
    void installCompanion(const std::filesystem::path& path, CompanionLoad load,
        const std::optional<CompanionRestore>& restore = std::nullopt);
    // closeCompanion's body; a replacement keeps follow mode and the shared
    // slice position for the companion about to take the slot.
    void tearDownCompanion(bool replacing);
    // Fills the companion's controls for its session; `loadedField` is the
    // field the load rendered, which the selector shows unless `selections`
    // (a reload's, as they stand at install) name a field this list has.
    void configureCompanionControls(std::optional<std::uint32_t> loadedField,
        const std::optional<CompanionRestore>& selections);
    // Which views a companion's controls and states reach.
    void scheduleLayerSliceRequests(DatasetLayer& layer);
    // The per-panel layouts follow the pair geometry and the aspect settings;
    // applying them re-places every tile without re-rendering.
    void updatePairLayouts();
    void applyPairLayouts();
    // The scene rect a panel frames: what arrivals assert as the scene rect
    // and Fit frames. The layout's whole canvas until a layer on the panel is
    // zoomed; then the panel's framed window (m_pairWindows), so a confined
    // zoom is not re-grown by the next arrival.
    [[nodiscard]] SceneRect pairCanvasRect(int normal) const;
    // Rubber-band zoom over two datasets: a scene window on a panel becomes
    // each layer's region (the part of its domain under the window, grown
    // out to a local layer's cell edges; a remote layer keeps the exact
    // window), the layers re-slice for them, and the view frames the window.
    [[nodiscard]] RealBox snappedPairRegion(
        std::size_t layer, int normal, const RealBox& region) const;
    [[nodiscard]] std::array<std::optional<RealBox>, 2> pairRegionsForSceneWindow(
        int normal, const QRectF& window) const;
    // The rect a panel's regions occupy, if any layer on it has one.
    [[nodiscard]] std::optional<QRectF> pairRegionsRect(int normal) const;
    // Sets the panel's layers to these regions (none: back to the whole
    // domain), records the framed window (`window`, else the regions' rect)
    // and re-slices. `refit` frames the window (a selection); a pan keeps
    // the view's scale and only moves it onto the window. False when both
    // regions are empty; nothing changes then.
    bool applyPairRegions(int normal,
        const std::array<std::optional<RealBox>, 2>& regions,
        std::optional<QRectF> window, bool refit);
    bool applyPairZoomWindow(int normal, const QRectF& window, bool refit = true);
    void pairRubberBandZoom(int normal, const QRectF& sceneRect);
    // A pan over a pair: the framed window moved against the drag, stopped
    // at the edge of the domains it covers with its size kept.
    [[nodiscard]] QRectF shiftedPairWindow(
        int normal, const QRectF& window, const QPointF& sceneDelta) const;
    // Both layers on the panel back to their whole domains, fitted.
    void resetPairPanelZoom(int normal);
    [[nodiscard]] const PairLayout& pairLayout(int normal) const noexcept
    {
        return m_pairLayouts[static_cast<std::size_t>(std::clamp(normal, 0, 2))];
    }
    // The panel normal to the perpendicular axis shows one layer at a time:
    // the one whose domain holds the slice position along that axis.
    [[nodiscard]] bool stateShown(const PlaneViewState& state) const noexcept;
    void updateShownLayers();
    // Actions that have no meaning with two datasets open are disabled while
    // a companion is, and restored when it closes.
    void updatePairedModeControls();
    // Whether the open dataset can take a companion: a 3-D plotfile with
    // physical geometry, local or remote, outside a sequence.
    [[nodiscard]] bool canOpenCompanion() const;
    // Push the primary's geometry to the isometric view, stretched by the
    // Axis Scaling factors as the slice panels are; physical proportions
    // otherwise, in either aspect mode.
    void updateIsoGeometry();
    // Push the pair geometry, in the panels' display proportions, to the
    // isometric view.
    void updatePairedIsoGeometry();
    void setCompanionFollowsPrimary(bool follows);
    // Re-slice a following companion's panel when the primary's displayed
    // range or mapping there differs from what the companion shows.
    void refreshFollowingCompanion(std::size_t normal, double minimum,
        double maximum, bool logarithmic);
    void chooseStandaloneDataset(const QString& caption, bool rawFab);
    struct RemoteOpen {
        std::shared_ptr<remote::Connection> connection;
        std::string remotePath;
    };
    void openDatasetImpl(const std::filesystem::path& path, bool metadataOnly,
        std::optional<PlotfileMetadataResult> preparedMetadata,
        std::filesystem::path dataRoot, bool preserveFabSelector,
        std::optional<FrameSliceSpec> initialSpec,
        std::optional<RemoteOpen> remoteOpen = std::nullopt);
    // Where a companion comes from: a local plotfile, or one on the server
    // the primary came from (`path` then carries the remote path, for its
    // name and the metadata dock).
    struct CompanionSource {
        std::filesystem::path path;
        std::optional<RemoteOpen> remote;
    };
    // Why the source cannot pair with the open dataset, if it cannot: a
    // remote companion needs a live connection, and beside a remote primary
    // it must be the primary's own.
    [[nodiscard]] std::optional<QString> companionSourceRefusal(
        const CompanionSource& source) const;
    void openCompanionImpl(CompanionSource source,
        std::optional<CompanionRestore> restore = std::nullopt);
    // Whether the companion's session was opened with the list the editor now
    // holds; true with no companion, or one that cannot take definitions.
    [[nodiscard]] bool companionSessionHasCurrentDefinitions() const;
    // The companion's reloadIfDefinitionsMoved, asked after the primary's
    // own reload has landed (which is what bumps m_generation).
    void reloadCompanionIfDefinitionsMoved();
    // Reopens the companion with the list as it stands, keeping its
    // selections. False when there is none to reload.
    bool reloadCompanion();
    // The worker halves of a companion open: read, pair, render. The local
    // one makes the session from the path with the id given; the remote one
    // opens a session on the primary's connection and takes the server's id.
    [[nodiscard]] static CompanionLoad loadLocalCompanion(
        const std::filesystem::path& path, FrameSliceSpec spec,
        const DatasetMetadata& primaryMetadata, DatasetId id,
        StopToken cancellation);
    [[nodiscard]] static CompanionLoad loadRemoteCompanion(
        const RemoteOpen& remote, FrameSliceSpec spec,
        const DatasetMetadata& primaryMetadata, StopToken cancellation);
    // A fresh independent top-level window (WA_DeleteOnClose) for the
    // "Open New Window" menu action; it shares no view/cache state with this one.
    MainWindow* createNewWindow();
    void exportImage();
    void exportAnimation();
    // Shared body of exportAnimation once the output path and color-bar choice
    // are known (from the dialogs, or from the test hook): freezes the export
    // zoom, starts the AnimationExporter (which owns the export state machine),
    // and kicks off frame 0.
    void beginAnimationExport(const QString& path, const ExportOptions& options);
    [[nodiscard]] ExportOptions exportOptions(bool includeColorBar, bool includeAxes,
                                              bool transparentBackground = false) const;
    [[nodiscard]] QImage composeExportFrame(const ImageView* view, const ExportOptions& options,
                                            qreal scaleFactor,
                                            ExportLayout* frozenLayout = nullptr) const;
    void createMenus();
    void rebuildLevelMenu();
    // One row of the derived part of a field list.
    struct DerivedFieldRow {
        QString name;
        // The id this dataset installed the definition under; nullopt when it
        // could not, which is what the row being shown greyed out means.
        std::optional<std::uint32_t> field;
        // Rich text, so the expression survives whatever the user wrote:
        // Qt reads a tooltip as markup as soon as it might be one, and only
        // some of the escapes it needs make it decide that.
        QString tooltip;
    };
    void rebuildVariableMenu(const std::vector<DerivedFieldRow>& rows);
    // Reopens what is on screen with the current frame spec -- the derived
    // field list included -- without the teardown a fresh open performs: the
    // sequence, the zoom and the open windows all stay. A sequence goes
    // through its controller so its frame bookkeeping stays consistent; a
    // single dataset is reloaded straight through requestInitialSlice, whose
    // new generation both invalidates the in-flight work and gives the
    // replacement session a dataset id of its own.
    // Reopens the dataset on the list as it now stands. False when it stood
    // aside without reloading -- mid-sequence playback, or nothing open --
    // which is what reloadIfDefinitionsMoved's memo must not record as done.
    bool reloadCurrentDataset();
    // The directory the last file dialog ended in, remembered across all of
    // them. The dialogs themselves stay separate -- one picks several
    // directories, one saves with a default suffix -- but this was copied into
    // each of them, as was the offscreen option (fileDialogOptions, in
    // MainWindow.cpp, where both its callers are).
    [[nodiscard]] QString rememberedDialogDirectory() const;
    void rememberDialogDirectory(const QString& path);
    // The Import/Export file dialog the derived-field controller asks for.
    [[nodiscard]] QString chooseExpressionListPath(
        QWidget* parent, bool forSaving);
    // Rebuilds the Dataset Metadata dock from the open session's metadata,
    // which a reload can change (a derived field appears or leaves) without
    // going through the open path that first filled it.
    void refreshMetadataDisplay();
    // Fills the field selector from the open dataset: the stored fields, then
    // a separator, then the derived ones (see derivedFieldRows). Shared by the
    // two configure paths, which populated it identically. Only the field rows
    // carry a field id as item data, so lookups stay findData-based and
    // nothing that reads item data can take the separator -- or a definition
    // this dataset could not install -- for a field.
    // `rows` is derivedFieldRows(), which the caller shares with
    // rebuildVariableMenu: the two views are the same list, and building it
    // twice per load means twice the work per sequence frame.
    void populateFieldSelector(
        DatasetLayer& layer, const std::vector<DerivedFieldRow>& rows);
    void populateFieldSelector(const std::vector<DerivedFieldRow>& rows)
    {
        populateFieldSelector(primary(), rows);
    }
    // Selects a field entry: `index` is where to start looking, and the
    // selection comes to rest on the nearest row that is actually a field.
    void selectFieldItem(DatasetLayer& layer, int index);
    void selectFieldItem(int index) { selectFieldItem(primary(), index); }
    // The session's definitions as rows to list, in the order they were
    // written. The field selector and the Variable menu are the same list
    // shown twice, and the comment saying so kept them in step by hand.
    // Where the stored fields end and the derived tail begins, clamped to what
    // the metadata actually holds. Five places used to decide this
    // independently -- the field selector, the Variable menu, the derived rows,
    // and both editor hooks -- and the clamp is what guards a session whose
    // count outruns the field list it carries.
    // Per layer: a companion has a session and a field list of its own.
    [[nodiscard]] std::size_t storedFieldCount(const DatasetLayer& layer) const;
    [[nodiscard]] std::size_t storedFieldCount() const
    {
        return storedFieldCount(primary());
    }
    [[nodiscard]] std::vector<DerivedFieldRow> derivedFieldRows(
        const DatasetLayer& layer) const;
    [[nodiscard]] std::vector<DerivedFieldRow> derivedFieldRows() const
    {
        return derivedFieldRows(primary());
    }
    // Adds a listed-but-unchoosable row to the field selector. False when the
    // combo's model is not one whose item flags can be set, in which case no
    // row is added at all: a row that looks selectable but carries no field id
    // is read as field 0 by everything downstream.
    [[nodiscard]] static bool addUnavailableFieldItem(
        QComboBox* selector, const QString& name, const QString& tooltip);
    // Whether a load built from the window's state as it stands can install
    // derived fields. Deliberately not asked of primary().session: a sequence builds
    // its first spec while the *outgoing* dataset is still installed (see
    // prepareSequence), so frame 0 would load without the definitions and
    // frame 1 would make them appear. A prepared session cannot take them
    // either, but that is a property of the load, not of the window, so
    // requestInitialSlice asks it there.
    // Opens a remote session for one load and renders it. Shared by the
    // sequence loader and the reload of a single remote dataset, so the
    // connected() pre-check and -- the reason it exists -- the derived-field
    // capability gate are written once. A peer that predates protocol 1.4 gets
    // an open with no definitions rather than a refusal, so a session that
    // cannot compute fields still shows the ones it stores.
    // Opens a remote session for a load, sending only the definitions the peer
    // can install. Not a refusal for an older peer: a list the user happens to
    // have must not stop it from opening a plotfile, and the editor is greyed
    // with the reason instead.
    [[nodiscard]] static std::shared_ptr<remote::RemoteDatasetSession>
    openRemoteSessionForLoad(
        const std::shared_ptr<remote::Connection>& connection,
        const std::string& remotePath,
        const std::vector<DerivedFieldDefinition>& derivedFields,
        StopToken cancellation);
    [[nodiscard]] static InitialSliceResult loadRemoteFrame(
        const std::shared_ptr<remote::Connection>& connection,
        const std::string& remotePath, std::uint64_t connectionGeneration,
        const FrameSliceSpec& spec, StopToken cancellation);
    [[nodiscard]] bool derivedFieldsReachNextLoad() const;
    // Whether the session on screen was opened with the list the editor now
    // holds. Asked of the session itself rather than inferred from when
    // things happened: a window is not told to reload while it has no dataset
    // (the whole of an open), a reload stands aside during playback, and a FAB
    // return reopens from a spec captured before any of it.
    [[nodiscard]] bool openSessionHasCurrentDefinitions() const;
    // Reloads the open dataset if it does not, once the caller's own work is
    // off the stack. Deferred because the frame path reaches this from inside
    // SequenceController::finishLoad, which goes on to touch its own state
    // after the display call returns.
    void reloadIfDefinitionsMoved();
    // The vector-glyph selections travel by name for the same reason the
    // scalar one does: an id means something only in the field list it came
    // from. Captured before a load swaps the dataset, resolved again after.
    [[nodiscard]] std::array<std::string, 3> vectorFieldNames() const;
    void restoreVectorFields(const std::array<std::string, 3>& names);
    void syncMenuChecks();
    void syncVariableMenu();
    // Runs the palette-file dialog for the controller's Load Palette File...
    // and reports a load failure.
    void loadPaletteFile();
    // The host's reaction to PaletteController::paletteChanged: pushes the
    // effective palette to the color bar, iso widget, overlays and a
    // re-render.
    void refreshPaletteDisplay();
    // Range state for a fresh dataset: the controller's widgets and per-field
    // memory, plus this window's full-domain range cache and pending store.
    void resetRangeState();
    // Which metadata-backed range modes the current field/level offers,
    // handed to the RangeController (which falls back to Visible if needed).
    void updateRangeModeAvailability();
    void updateRangeModeAvailability(DatasetLayer& layer);
    void showContoursDialog();
    // Draws the ParticleController's samples into a view: the projection and
    // the plane mapping are the host's, the settings and samples are its.
    void updateParticleOverlay(PlaneViewState& state);
    void updateParticleOverlays();
    void applyContourSettings(DisplayMode mode, int count, int uField, int vField,
        int wField, int contourColor);
    void showNumberFormatDialog();
    void applyNumberFormat(const QString& format);
    // Re-resolve the authored format against a displayed range and push the
    // result to the readouts. Driven from committed range values only --
    // deriving it from a bound the user is mid-edit would re-render the box
    // being typed into on every keystroke.
    void applyDisplayPrecision(double minimum, double maximum);
    void pushDisplayFormat();
    void showLengthUnitsDialog();
    void applyLengthUnit(const QString& unitId);
    // View > Aspect Ratio: the per-axis display stretch (see AspectMode.hpp).
    // The dialog edits m_axisScale; applyAxisScale installs a new set and
    // resetAxisScale returns to unit factors when a dataset is opened.
    void showAxisScalingDialog();
    // The per-axis factors (the primary's along every axis) and, with a
    // companion, the companion's factor along the perpendicular axis.
    void applyAxisScale(const std::array<double, 3>& axisScale,
        std::optional<double> companionPerpendicularScale = std::nullopt);
    void resetAxisScale();
    void setAspectMode(AspectMode mode);
    [[nodiscard]] std::array<double, 3> displayStretchPerAxis() const;
    // The two factors a panel shows, normalized so the smaller is one.
    [[nodiscard]] std::array<double, 2> displayStretchFor(
        const PlaneViewState& state) const;
    // viewportPixelSize enlarged along the less stretched axis, the bound a
    // remote raster is sized to (see sliceOutputSize and the sequence spec).
    [[nodiscard]] std::array<int, 2> stretchedViewportPixelSize(
        const PlaneViewState& state) const;
    // Push the current stretch to one view (showSlice, before the raster is
    // installed) or to every view after an option change, when a remote view
    // also re-requests a raster sized for the new stretch.
    void applyDisplayStretch(PlaneViewState& state);
    void applyDisplayStretches();
    // The scene layout of a mapped view: its canvas on this panel's axes at
    // the current axis factors. Nothing until the first mapped arrival has
    // brought the canvas bounds.
    [[nodiscard]] std::optional<MappedLayout> mappedLayout(
        const PlaneViewState& state) const;
    // Ask the warp for what the viewport shows: the visible physical window
    // at the viewport's own device pixels, re-sliced first when the plane on
    // hand cannot serve it (capped below native resolution, or not covering
    // the window). Runs on every view change; a no-op unless the view shows
    // a mapped raster on a known canvas.
    void updateMappedDemand(PlaneViewState& state);
    // Whether the demand loop has asked the view's warp for a window yet.
    [[nodiscard]] bool hasMappedWindow(const PlaneViewState& state) const;
    // A frame spec's per-view mapped windows and pixels, from what each view
    // shows now (FrameSliceSpec::displayWindows).
    void fillMappedDisplays(FrameSliceSpec& spec,
        const std::vector<PlaneViewState*>& views) const;
    // Enable/disable the Aspect Ratio submenu for the current dataset.
    void updateAspectControls();
    void validateVectorMode();
    void ensureVectorFieldDefaults();
    void showDatasetWindow();
    void closeDatasetWindow();
    // Drops every view's marked cell and the outline drawn from it. Over
    // allViewStates(), not currentViews(): the latter is empty mid-teardown,
    // because openDatasetImpl zeroes m_viewDimension before it closes the
    // window, and a cell left behind there is redrawn over the next dataset.
    void clearDatasetCellHighlights();
    void refreshDatasetWindow();
    // Pushes the active view's palette and display range -- the pair the color
    // bar is drawn from -- to an open Dataset window, so its numbers keep the
    // colors the bar is showing. Called from every place that moves either.
    void syncDatasetWindowColors();
    void datasetCellActivated(const RealBox& physicalCell);
    // Draws state.datasetCell as the view's cell highlight, or clears it when
    // the cell falls outside the raster. Called both when the selection moves
    // and after a redraw replaces the image.
    void applyDatasetCellHighlight(PlaneViewState& state);
    [[nodiscard]] std::optional<DatasetRequest> buildDatasetRequest() const;
    void showUserGuide();
    void showKeyboardMouseReference();
    void showAboutDialog();
    void showMetadata(const PlotfileMetadataResult& result, const std::filesystem::path& path);
    // The rows for one dataset, at the top level (root null) or under a parent
    // row when two datasets are listed; the session is the dataset's own.
    void appendMetadataRows(QTreeWidgetItem* root,
        const PlotfileMetadataResult& result, const std::filesystem::path& path,
        const std::shared_ptr<DatasetSession>& session);
    // Re-renders the Diagnostics panel; the model owns the counters, this
    // window only supplies the lines it alone knows (see the model's Hooks).
    void updateDiagnostics();
    // Non-modal failure report: status bar plus the model's error history
    // (which shows the dock); suppressed while closing.
    void reportBackgroundError(const QString& message);
    // The one wording for a shared-range sync that could not be scheduled,
    // run, or applied.
    void reportVisibleSyncFailure(const std::exception& error);
    void updateAnimationDockVisibility();
    void updateWindowTitle();
    void restoreSettings();
    void saveSettings();

    // Per-view wiring and display updates. A panel's ImageView is wired once
    // for the signals that belong to the panel (zoom, fit, resize, scroll,
    // pan) and fanned out to every layer's state on it; each state is wired
    // for the tile-addressed signals (probe, line plot, slice move), taking
    // only those for its own tile. Wiring per state for everything would
    // fire the panel signals once per layer.
    void wirePanelSignals(ImageView* view, int normal);
    void wireTileSignals(PlaneViewState& state);
    // Every view state of every layer, whatever the current dimension -- the
    // 2-D view and both layers' slice panels. currentViews() answers a
    // narrower question: the views the *displayed* datasets use. Teardown and
    // failure states have to reach them all, since the dimension they were
    // showing is already gone.
    [[nodiscard]] std::vector<PlaneViewState*> allViewStates();
    void setAllViewPlaceholders(const QString& text);
    // The active layers' states for the current dimension: three per layer in
    // 3-D, the 2-D view otherwise.
    [[nodiscard]] std::vector<PlaneViewState*> currentViews();
    // The primary layer's states only: what a dataset load produces one
    // display per, and what frame specs and exports enumerate.
    [[nodiscard]] std::vector<PlaneViewState*> primaryViews();
    // The states drawn on one 3-D panel, one per active layer.
    [[nodiscard]] std::vector<PlaneViewState*> statesForPanel(int normal);
    void setActiveView(PlaneViewState& state);
    // Give the active view keyboard focus so the arrow-key pan works on a
    // freshly opened dataset without a click first -- unless the user is
    // already typing somewhere, in which case their place is theirs to keep.
    void focusActiveViewForPanning();
    // Point the color scale and range spin boxes at the active view's display
    // state. Shared by setActiveView and showSlice's active-view branch.
    void syncActiveViewColorControls(const PlaneViewState& state);
    [[nodiscard]] std::array<int, 2> displayAxes(int normal) const;
    [[nodiscard]] std::array<int, 2> nativeOutputSize(
        const PlaneViewState& state) const;
    [[nodiscard]] std::array<int, 2> viewportPixelSize(
        const PlaneViewState& state) const;
    [[nodiscard]] std::array<int, 2> sliceOutputSize(
        const PlaneViewState& state, bool forceRemote = false) const;
    [[nodiscard]] QSize logicalImageSize(const PlaneViewState& state,
        const ScalarPlane& plane, const QImage& image) const;
    // True when the active dataset is displayed as a warped 2-D spherical
    // (r, theta) plane. Gates the coordinate-warp overlay, probe, and label
    // paths; all other datasets keep their Cartesian behavior.
    [[nodiscard]] bool displayIsSpherical() const;
    // True only for the warped R-Z spherical view. Overlays that assume a
    // linear plane-pixel-to-scene mapping (line plots, particle points, vector
    // glyphs) work in the logical r-theta / theta-r layouts but not here.
    [[nodiscard]] bool displayIsSphericalWarp() const;
    // Coordinate mapper for a view: logical (x, y)/(r, theta) <-> scene pixels,
    // built from the plane, the warped display region, and the pixmap size.
    // The volume's "limit to visible region" box: the part of the domain the
    // three panels show, in the logical coordinates the volume samples.
    [[nodiscard]] RealBox volumeRegionOfInterest() const;
    [[nodiscard]] PlaneMapping planeMapping(const PlaneViewState& state) const;
    // Enable/disable the 2-D Spherical menu for the current dataset.
    void updateSphericalControls();
    // Whether the primary dataset can be drawn on its mapped grid: its
    // session carries the nodal positions and no companion is open (a pair's
    // tiles are placed affinely, which a physically uniform pixmap of a
    // stretched grid does not satisfy).
    [[nodiscard]] bool mappedGridAvailable() const;
    // The View > Mapped Grid choice as it applies now: on, and available.
    [[nodiscard]] bool displayIsMapped() const;
    // The warp a slice request for this view asks for: the mapped grid for
    // the primary while it is shown, the R-Z wedge for a 2-D spherical
    // plane in that layout, else none.
    [[nodiscard]] DisplayWarp requestedWarpFor(const PlaneViewState& state) const;
    // Enable/disable the Mapped Grid menu for the current dataset, with a
    // tooltip saying why it is off.
    void updateMappedGridControls();
    // Horizontal and vertical axis names for a spherical layout ({"R","Z"},
    // {"r","theta"}, or {"theta","r"}). Callers pass the displayed view
    // state's mode so labels always describe the raster on screen.
    [[nodiscard]] static std::array<QString, 2> sphericalAxisLabels(
        SphericalDisplay mode);
    void probeMoved(PlaneViewState& state, int x, int displayY);
    // The readout for the status bar: probeReadout, prefixed with the
    // dataset's name while a companion is open.
    [[nodiscard]] QString probeLine(
        const PlaneViewState& state, int x, int displayY) const;
    void probeClicked(PlaneViewState& state, int x, int displayY);
    [[nodiscard]] QString probeReadout(
        const PlaneViewState& state, int x, int displayY) const;
    void rubberBandZoom(PlaneViewState& state, const QRectF& sceneRect);
    // A rubber band over a mapped view: the scene is the physical canvas, so
    // the selection is a physical window and the zoom is the view's alone
    // (updateMappedDemand draws the warp for what it then shows). With sync
    // on, each other 3-D panel is zoomed to the selection's extent along the
    // axis it shares with this one, over its own canvas.
    void mappedRubberBandZoom(PlaneViewState& state, const QRectF& sceneRect);
    // The normalized plane rect (top-down) of the raster cells drawn inside a
    // pixmap-pixel rect of a mapped view, found through the source index.
    // Nothing when no cell was drawn in it.
    [[nodiscard]] std::optional<QRectF> mappedPlaneBounds(
        const PlaneViewState& state, const QRectF& pixmapRect) const;
    void applyRubberBandZoom(
        PlaneViewState& state, const QRectF& normalizedRect);
    void beginPanDrag(PlaneViewState& state);
    void updatePanDrag(PlaneViewState& state, const QPointF& totalSceneDelta,
        const QPoint& viewportDelta);
    void endPanDrag(PlaneViewState& state, const QPointF& totalSceneDelta);
    void flushPanDrag(bool finalize);
    void applyFixedScale(int factor);
    // Fetch whatever finest cells the virtual canvas currently shows (plus a
    // constant one-cell slack so scroll pans replace equal-size rasters).
    void updateRemoteFixedScaleDemand(PlaneViewState& state);
    // True when this view runs the demand-driven remote fixed scale, i.e. a
    // virtual whole-domain canvas holding a fetched raster window.
    [[nodiscard]] bool remoteDemandCanvas(const PlaneViewState& state) const;
    [[nodiscard]] std::optional<ImageView::VirtualPlacement>
    virtualPlacementFor(
        const PlaneViewState& state, const RealBox& region) const;
    void centerViewOnData(
        PlaneViewState& state, const std::array<double, 2>& dataCenter);
    [[nodiscard]] std::array<double, 2> viewCenterInData(
        const PlaneViewState& state) const;
    void applyPanStep(PlaneViewState& state, const QPointF& direction);
    [[nodiscard]] std::optional<RealBox> shiftedPanRegion(
        const PlaneViewState& state, const RealBox& baseRegion,
        int planeWidth, int planeHeight, const QPointF& sceneDelta) const;
    void linePlotRequested(PlaneViewState& state, int imageX, int imageY,
        Qt::MouseButton button);
    void sliceMoveRequested(PlaneViewState& state, int imageX, int imageY,
        Qt::MouseButton button);
    // Reset a view (or all views) to the whole domain and refit: the Scale
    // menu's "Reset Zoom", the 0 shortcut, and double-click all land here.
    // Distinct from ImageView::fitToWindow, which only refits the current
    // raster without touching the visible region.
    void resetViewZoom(PlaneViewState& state);
    void resetZoomAllViews();
    // When a Preserve-policy raster arrives with a different pixels-per-data
    // density than the plane it replaces (a zoomed re-slice on a dataset whose
    // full-domain raster hit the output cap), returns the currently visible
    // physical window mapped into the incoming plane's scene coordinates, for
    // re-framing after the swap; nullopt when the plain Preserve is already
    // correct. See issue #45.
    [[nodiscard]] std::optional<QRectF> preservedDataWindow(
        const PlaneViewState& state, const ScalarPlane& incoming) const;
    // By value, and callers move into it: the planes are the largest thing an
    // arrival carries -- at the 4096 output cap a ScalarPlane is around 117 MB
    // and the ImageBuffer around 67 MB -- and a const& forced this function to
    // deep-copy them again into the shared_ptr snapshots it publishes.
    // sessionEpoch is the epoch the display was computed under, stamped onto
    // the view. No default: every caller has to say which session produced
    // what it is showing, which is the whole point of the stamp.
    void showSlice(PlaneViewState& state, SliceDisplayResult display,
        std::uint64_t sessionEpoch);
    // Re-slices every current view whose raster came from a session that is no
    // longer installed. Called wherever a load settles -- including when it
    // fails, since a failed reload leaves the previous session installed and
    // its own display untouched.
    void resliceReplacedViews();
    void updateOverlay(PlaneViewState& state);
    void updateOverlays();
    void updateGridBoxes(PlaneViewState& state);
    void updateGridBoxes();
    void updateScaleBar(PlaneViewState& state);
    void updateScaleBars();
    // The scale bar is offered only while the screen has one pixel density
    // per physical unit on both in-plane axes, which depends on the dataset's
    // cell sizes and on the aspect settings; re-evaluated when either changes.
    void updateScaleBarAvailability();
    void resetLengthUnit();
    void updateCrosshairs(PlaneViewState& state);
    void updateCrosshairs();
    [[nodiscard]] QLineF planeSegmentToScene(const PlaneViewState& state,
        float x0, float y0, float x1, float y1) const;
    [[nodiscard]] QColor overlayColor() const;
    [[nodiscard]] QColor sliceAxisColor(int axis) const;

    // Shared 3-D slice positions (physical coordinates per axis).
    void configureSlicePositionControls();
    // Show or hide the Position group together with its trailing toolbar
    // separator, so the separator never dangles when no dataset is loaded.
    void setSlicePositionControlsVisible(bool visible);
    void setSlicePosition(int axis, double value);
    // Pushes m_slicePosition3d to everything that draws the planes: the iso
    // quadrant and, when it is open, the volume window. Every writer that
    // publishes the positions goes through here; requestInitialSlice sets
    // them and relies on configureSliceControls to publish, and the ForTest
    // setter is test-only.
    void publishSlicePositions();
    [[nodiscard]] int sliceIndexLevel() const;
    // Visible-range mode in 3-D: recompute the min/max from all three panels'
    // planes so the single color bar maps them consistently. The heavy part
    // (extrema scans, contour re-extraction, up to three full raster renders)
    // runs on a worker over the panels' immutable plane snapshots. Single-flight:
    // dispatch waits until the panel batch settles (slicesInFlight() == 0) and no
    // sync is running; a call in the meantime marks a rerun instead of stacking
    // workers. At completion the shared result is applied all-or-nothing, keyed
    // on the per-view render generation (see PlaneViewState::renderGeneration) --
    // if any panel was re-sliced mid-sync the whole outcome is dropped and the
    // rerun recomputes it.
    // Per layer: each dataset has its own range and colour bar, so its three
    // panels are synchronized on their own. The no-argument form runs it for
    // every active layer.
    void syncVisibleRanges();
    void syncVisibleRanges(DatasetLayer& layer);
    // Panel slices currently on a worker (summed PlaneViewState::pendingRequests);
    // the visible-range sync defers dispatch until this is zero. Panel work only
    // -- excludes particle/line-plot/prefetch requests, which the
    // DiagnosticsModel's active count tracks.
    [[nodiscard]] int slicesInFlight() const;
    [[nodiscard]] int slicesInFlight(const DatasetLayer& layer) const;
    // Emits interactiveSlicesSettled once the DiagnosticsModel's active count
    // is zero; otherwise leaves the signal to the activity that is still
    // running (a frame prefetch, see the loadActivityChanged handler).
    void settleIfDrained();

    // Slice requests: the debounce timer coalesces into per-view requests.
    // rasterDirty false means the trigger (contour mode/count) cannot change
    // the raster, so a cache-satisfied request skips the image re-render.
    void scheduleSliceRequest(bool rasterDirty = true);
    void scheduleSliceRequest(PlaneViewState& state, bool rasterDirty = true);
    void flushSliceRequests();
    void requestSlice(PlaneViewState& state, bool rasterDirty);
    // How requestInitialSlice is to get its session: one that is already open,
    // or a remote endpoint to reopen. Exactly one applies, which is why they
    // travel together rather than as two parameters that must not both be set.
    // Both isRemote() and responseBytes() are asked *before* the worker runs --
    // they size the output rasters and arm the post-load resize coalescing --
    // so the reopen case has to be able to answer them without a session.
    struct SliceLoad {
        // Already open: the initial remote open, which needed the session on
        // the GUI thread for its metadata.
        std::shared_ptr<DatasetSession> session;
        // To be opened on the worker: the quiet reload of a remote dataset,
        // which must not tear the window down the way a fresh open does.
        std::optional<RemoteOpen> reopen;
        [[nodiscard]] bool isRemote() const;
        // The negotiated frame size, which bounds a remote raster.
        [[nodiscard]] std::optional<std::uint32_t> responseBytes() const;
    };
    void requestInitialSlice(const std::filesystem::path& path,
        std::uint64_t generation,
        std::optional<PlotfileMetadataResult> preparedMetadata = std::nullopt,
        std::filesystem::path dataRoot = {},
        std::optional<FrameSliceSpec> initialSpec = std::nullopt,
        SliceLoad load = {});
    // The scale the toolbar button and the View > Scale radio group report.
    // They are one state shown twice, so there is one setter: picking "4x" from
    // the toolbar used to leave the View menu unchecked, and neither reset when
    // a new dataset opened fitted, so the toolbar could claim "4x" over a
    // fitted view. fixedScaleStateMatchesForTest asserts exactly this
    // agreement.
    enum class ScaleUiState : std::uint8_t { Fit, Fixed, Custom, Mixed };
    void setScaleUiState(ScaleUiState state, int factor = 0);
    // Re-state the current scale after the active view or its dataset changed;
    // the clamped label is computed from both.
    void refreshScaleReport();
    // The radio items' text and every lookup that matches them.
    [[nodiscard]] QString plainScaleLabel(int factor) const;
    [[nodiscard]] QString fixedScaleLabel(int factor, double effective) const;
    [[nodiscard]] QString defaultScaleToolTip() const;
    // View pixels per finest cell that a chosen fixed scale actually achieves,
    // which is the factor itself unless the whole-domain raster hit
    // maxSliceOutputDimension. Past that clamp one raster pixel is more than one
    // finest cell and the view scales the clamped raster by the factor anyway,
    // so the same menu item means different magnifications on different
    // domains -- and something different again remotely, where the fetch is
    // demand-driven and never clamped. Zero when there is nothing to report.
    // See agent-notes/issues/fixed-scale-clamped-native-raster.md.
    [[nodiscard]] double effectiveFixedScale(int factor) const;
    void configureSliceControls();
    // Enable the dataset-dependent field/level/range/menu controls once a
    // dataset (single or sequence frame) is loaded. Shared by
    // configureSliceControls and configureSequenceControls; the export-animation
    // action is sequence-only and stays at that call site.
    void enableDatasetControls(const DatasetMetadata& metadata);
    void appendLinePlotCurve(const LineResult& line, const std::string& fieldName,
        int dimension, int primaryFixedAxis, int lineAxis,
        const std::array<double, 3>& fixedCoordinates, int maximumLevel,
        CompositionPolicy composition);

    // Animation: one shared playback timer drives either the 3-D plane sweep
    // or plotfile-sequence playback, never both at once.
    enum class PlaybackMode {
        None,
        Sweep,
        Sequence
    };
    void choosePlotfileSequence();
    // Establishes the shared local/remote sequence invariants after the frame
    // list has been validated.
    void prepareSequence(std::size_t frameCount);
    void closeSequence();
    void goToSequenceFrame(int index, bool forceRestart = false);
    void toggleSequencePlayback();
    void stepSweep(int direction);
    void toggleSweepPlayback();
    void setPlaybackMode(PlaybackMode mode);
    void playbackTick();
    void applySpeed();

    // Sequence frame switching lives in the SequenceController; this window
    // supplies the GUI-coupled pieces below as its hooks.
    void displayFrameResult(InitialSliceResult& result, bool defaultPositions);
    // `displayedField` is the field the frame was actually rendered with,
    // which the pipeline resolves by name (resolveSpecField) and so need not
    // be the id -- or the combo position -- the previous frame used.
    void configureSequenceControls(
        bool defaultPositions, std::optional<std::uint32_t> displayedField);
    [[nodiscard]] FrameSliceSpec buildFrameSpec();
    // Stop timers and request stop on every async task this window can launch,
    // so an in-flight read that holds the global I/O mutex bails promptly and
    // does not block QThreadPool teardown. Called from closeEvent and (via a
    // lambda that additionally clears the shared pool) from aboutToQuit. It
    // must not clear() the global pool itself: the pool is shared across
    // windows and clearing it from a per-window close strands other windows'
    // queued work (see window-close-clears-shared-thread-pool).
    void cancelInFlight();

    QStackedWidget* m_stack = nullptr;
    IsoWidget* m_isoWidget = nullptr;
    QLabel* m_probeLabel = nullptr;
    // Permanent status-bar item, shown only while the remote server
    // predates full-precision values; a status message would be
    // overwritten by the open that follows the session's ready line.
    QLabel* m_remotePrecisionLabel = nullptr;
    LinePlotWindow* m_linePlotWindow = nullptr;
    // Cancels in-flight line-plot queries on dataset switch or window close so
    // a late result neither reopens a closed window nor wastes I/O.
    StopSource m_linePlotStopSource;
    DatasetWindow* m_datasetWindow = nullptr;
    SetContoursDialog* m_contoursDialog = nullptr;
    QDialog* m_numberFormatDialog = nullptr;
    QDialog* m_lengthUnitsDialog = nullptr;
    QDialog* m_axisScalingDialog = nullptr;
    UserGuideDialog* m_userGuideDialog = nullptr;
    QWidget* m_slicePositionControls = nullptr;
    QAction* m_positionSeparator = nullptr;
    std::array<QSpinBox*, 3> m_sliceSpinboxes{nullptr, nullptr, nullptr};
    QTimer* m_sliceDebounce = nullptr;
    QTimer* m_panDebounce = nullptr;
    PlaneViewState* m_panView = nullptr;
    RealBox m_panStartRegion{};
    // Over a pair: the panel's framed window when the drag began, which the
    // drag shifts (see flushPanDrag's paired arm).
    QRectF m_panStartSceneWindow;
    int m_panPlaneWidth = 0;
    int m_panPlaneHeight = 0;
    QPointF m_panSceneDelta;
    QPointF m_panLastScheduledDelta;
    bool m_panDataRefresh = false;
    // Owns the full-domain range cache (kept current whenever a non-zoomed
    // slice completes, reused for RangeMode::Visible during zoom/pan so the
    // color bar stays stable) plus the shared-range and transform-policy
    // decisions the slice paths share. See pipeline/DisplayCoordinator.hpp.
    amrvis::DisplayCoordinator m_displayCoordinator;
    // Single-flight state of the async 3-D shared-range sync: one worker at a
    // time; a request while one is in flight coalesces into a rerun. The
    // pending range-store key carries the "cache the full-domain union after
    // the sync" step (see the slice-arrival completion) into the sync
    // completion, where the union is actually known.
#ifdef AMREXPLORER_QT_TEST_ACCESS
    // Test-only: run just after an initial-slice load is launched; see
    // setInitialSliceLaunchedHookForTest.
    std::function<void()> m_initialSliceLaunchedForTest;
    // Test-only: consumed by the next initial-slice completion, which then
    // throws where a load that failed would have. See
    // failNextInitialSliceForTest.
    bool m_failNextInitialSliceForTest = false;
    // Test-only: superseded visible-range sync outcomes dropped by the
    // rerun guard. Sole writer is that drop, so the overlapping-sync test can
    // assert an exact count. The DiagnosticsModel's stale count carries the
    // same event for the user-facing diagnostics panel.
    std::uint64_t m_visibleSyncStaleSkips = 0;
#endif
    // Whether the connection a remote sequence was opened on can install
    // derived fields. Captured there rather than asked of primary().session, for the
    // reason derivedFieldsReachNextLoad gives: frame 0's spec is built while
    // the outgoing dataset is still installed.
    bool m_remoteSequenceDerivedFields = false;
    // The list a reload has already been asked for, with the session epoch it
    // was asked under. What bounds the reopens is no longer the memo itself:
    // any install invalidates it, so the bound rests on no install ever leaving
    // a mismatch. That holds because a session records the list it was asked
    // for, and all three paths that filter the list before an open -- a pre-1.4
    // connection, either !derivedFieldsReachNextLoad case, and a prepared
    // session opened with the copied list -- make available() false, so this is
    // never reached for them. The epoch is what keeps it from getting stuck: any
    // install makes the memo stale by itself, so it cannot suppress a reload
    // for a different dataset or one that a later session could satisfy. It is
    // cleared outright when a reload stands aside or a load fails, neither of
    // which installs anything.
    std::optional<std::pair<std::vector<DerivedFieldDefinition>, std::uint64_t>>
        m_reloadAskedFor;
    // The same memo for the companion's session (see
    // reloadCompanionIfDefinitionsMoved).
    std::optional<std::pair<std::vector<DerivedFieldDefinition>, std::uint64_t>>
        m_companionReloadAskedFor;
    QTreeWidget* m_metadataTree = nullptr;
    QDockWidget* m_metadataDock = nullptr;
    QDockWidget* m_diagnosticsDock = nullptr;
    QDockWidget* m_colorBarDock = nullptr;
    QDockWidget* m_animationDock = nullptr;
    // *Why* the Animation panel currently applies, so
    // updateAnimationDockVisibility can act on the transition rather than
    // reasserting visibility on every sequence frame. Both reasons are kept
    // separately, not folded into one "applies" flag: the panel hosts two
    // different sets of controls, so 3-D-only -> sequence is a real transition
    // even though the panel applied before and after.
    bool m_animationDockSequence = false;
    bool m_animationDockThreeD = false;
    // Arrow-key pan requests that reached a view, for the routing regression.
    std::size_t m_panStepRequests = 0;
    // Standalone-FAB / MultiFab navigation: mode, source, return record,
    // the selector dock and the async header reads. Its dock is what the
    // View menu toggles.
    FabNavigator* m_fabNavigator = nullptr;
    FabSelectorDock* m_fabSelectorDock = nullptr;
    QToolBar* m_sliceToolbar = nullptr;
    QToolBar* m_rangeToolbar = nullptr;
    QPushButton* m_scaleButton = nullptr;
    QMenu* m_levelMenu = nullptr;
    QMenu* m_variableMenu = nullptr;
    // "2-D Spherical" View section grouping the warped-display options; the
    // whole submenu is enabled only while a 2-D spherical dataset is shown.
    QMenu* m_sphericalMenu = nullptr;
    QMenu* m_sphericalDisplayMenu = nullptr;
    QActionGroup* m_sphericalDisplayGroup = nullptr;
    // View > Aspect Ratio: disabled for 2-D spherical data; the Physical
    // Size radio is further disabled without physical geometry.
    QMenu* m_aspectMenu = nullptr;
    QActionGroup* m_aspectGroup = nullptr;
    QAction* m_aspectCellCountsAction = nullptr;
    QAction* m_aspectPhysicalAction = nullptr;
    // View > Mapped Grid: enabled only while the primary dataset carries
    // nodal positions and no companion is open (updateMappedGridControls).
    QMenu* m_mappedGridMenu = nullptr;
    QAction* m_mappedGridAction = nullptr;
    QActionGroup* m_scaleGroup = nullptr;
    QActionGroup* m_levelGroup = nullptr;
    QActionGroup* m_variableGroup = nullptr;
    // Window-owned so rebuildVariableMenu's clear() does not delete it.
    QAction* m_expressionEditorAction = nullptr;
    QAction* m_boxesAction = nullptr;
    QAction* m_scaleBarAction = nullptr;
    // The saved preference is separate from the action's checked state:
    // anisotropic datasets force the action off without erasing what the user
    // selected for datasets on which a physical scale bar is meaningful.
    bool m_scaleBarVisible = false;
    // Empty means the plotfile coordinate unit is unknown. Otherwise this is
    // one of ScaleBar.hpp's stable length-unit ids (cm, AU, pc, ...).
    QString m_lengthUnitId;
    QAction* m_slicePlanesAction = nullptr;
    QAction* m_resetZoomAction = nullptr;
    QAction* m_syncRubberBandZoomAction = nullptr;
    QAction* m_contoursAction = nullptr;
    QAction* m_datasetAction = nullptr;
    QAction* m_exportAnimationAction = nullptr;

    // Drives File -> Export Animation...: owns the whole export state machine
    // (progress, cancellation, FFmpeg encoding). This window supplies frame
    // rendering and sequence navigation, and restores its UI on finished().
    AnimationExporter* m_animationExporter = nullptr;
    std::array<DatasetLayer, 2> m_layers;
    // Set while a companion is open: how the two layers' domains meet, and the
    // scene layout of each 3-D panel derived from it (index = normal).
    std::optional<PairGeometry> m_pair;
    std::array<PairLayout, 3> m_pairLayouts;
    // Per panel, the window a zoom or pan framed, kept apart from the
    // layers' regions: those are rounded to each layer's cells, and a window
    // rebuilt from them would drift a cell per step where the cells of the
    // two layers do not line up.
    std::array<std::optional<QRectF>, 3> m_pairWindows;
    QToolBar* m_companionToolbar = nullptr;
    QLabel* m_companionLabel = nullptr;
    // "Same as primary": the companion's slices take the primary's displayed
    // range on the same panel and its own range controls and colour bar are
    // withheld.
    QCheckBox* m_companionFollowBox = nullptr;
    bool m_companionFollowsPrimary = false;
    QAction* m_openCompanionAction = nullptr;
    QAction* m_openRemoteCompanionAction = nullptr;
    // A --companion given with --ssh, waiting for the load it belongs to
    // (by generation) to finish; any other open drops it.
    struct PendingRemoteCompanion {
        std::string remotePath;
        std::uint64_t generation = 0;
    };
    std::optional<PendingRemoteCompanion> m_pendingRemoteCompanion;
    QAction* m_closeCompanionAction = nullptr;
    QAction* m_volumeAction = nullptr;
    QAction* m_particlesAction = nullptr;
    StopSource m_companionStopSource;
    std::uint64_t m_companionGeneration = 0;
    // The 2-D page's one view state; it always slices the primary layer.
    PlaneViewState m_view2d;
    PlaneViewState* m_activeView = nullptr;
    int m_viewDimension = 0;
    std::array<double, 3> m_slicePosition3d{0.0, 0.0, 0.0};
    bool m_pendingAllViews = false;
    std::vector<PlaneViewState*> m_pendingViews;
    // OR of the rasterDirty flags of the coalesced pending requests.
    bool m_pendingRasterDirty = false;
    // A slice landed while other activity ran: the settle is still owed.
    bool m_settleDeferred = false;
    StopSource m_initialStopSource;
    StopSource m_metadataStopSource;
    DisplayMode m_displayMode = DisplayMode::Raster;
    int m_contourCount = 15;
    // 2-D spherical display layout (see SliceRequest::sphericalDisplay).
    SphericalDisplay m_sphericalDisplay = SphericalDisplay::RZ;
    // View > Mapped Grid: draw slices on the plotfile's stretched grid when
    // it carries one (see SliceRequest::mappedGrid). A persisted preference
    // that applies whenever the open dataset can honour it.
    bool m_mappedGrid = false;
    // Persisted preference; the per-axis factors belong to the open dataset
    // and reset to one with each new one (they survive sequence frames).
    AspectMode m_aspectMode = AspectMode::CellCounts;
    std::array<double, 3> m_axisScale{1.0, 1.0, 1.0};
    int m_contourColor = contourColorBlack;
    int m_vectorUField = -1;
    int m_vectorVField = -1;
    int m_vectorWField = -1;
    // Owns the particle selection, samples, sample load, dialog, action and
    // progress indicator; the host draws its samples into the views.
    ParticleController* m_particleController = nullptr;
    VolumeController* m_volumeController = nullptr;
    std::filesystem::path m_datasetPath;
    // Owns the ssh session and the connection every remote open goes
    // through, and the Open Remote dialogs and browser; asks this window to
    // open what the user picked. Server DatasetIds restart at one per
    // connection, so dataset-scoped caches are keyed by its
    // connectionGeneration() as well.
    RemoteSessionController* m_remoteSession = nullptr;
    bool m_remoteSequence = false;
    std::uint64_t m_remoteSequenceConnectionGeneration = 0;
    // Owns the palette selection, its widgets and persistence; palette() is
    // what the renderer, color bar and overlays use.
    PaletteController* m_paletteController = nullptr;
    // Owns the Skin selection (System/Light/Dark) and its persistence. The
    // skin it applies is application-wide, so every window shares one.
    ThemeController* m_themeController = nullptr;
    DerivedFieldController* m_derivedFields = nullptr;
    // The authored format, and that format resolved against the displayed
    // range. m_numberFormat is what the dialog shows and what is persisted;
    // m_displayFormat is what the range controls render with. The color bar
    // and child windows resolve the authored format against their own ranges.
    QString m_numberFormat = defaultNumberFormat();
    QString m_displayFormat = defaultNumberFormat();
    // The range the resolved format was derived from, so a format change can
    // re-resolve without waiting for the next slice.
    double m_lastDisplayMinimum = 0.0;
    double m_lastDisplayMaximum = 1.0;
    bool m_controlsReady = false;
    // Set while showSlice installs an arrival: the view changes it makes
    // (a Fit, a stretch) must not ask the warp for a window against the
    // request that is being replaced; showSlice asks once itself afterwards.
    bool m_applyingArrival = false;
    std::uint64_t m_generation = 0;
    bool m_closing = false;
    // Owns the Diagnostics panel's counters (background requests, stale
    // results), the last read's metrics, the cache state, the probe history
    // and the background-error history, and renders the dock.
    DiagnosticsModel* m_diagnosticsModel = nullptr;

    // Animation state. The sequence frames/index/in-flight/prefetch state
    // machine lives in the SequenceController; this window keeps only the
    // playback timer and mode.
    AnimationPanel* m_animationPanel = nullptr;
    QTimer* m_playbackTimer = nullptr;
    PlaybackMode m_playbackMode = PlaybackMode::None;
    SequenceController* m_sequenceController = nullptr;
};

} // namespace amrvis::qt
