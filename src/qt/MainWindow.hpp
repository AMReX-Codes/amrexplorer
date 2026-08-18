#pragma once

#include "DatasetWindow.hpp"
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
class QLabel;
class QSpinBox;
class QLineF;
class QMenu;
class QProgressDialog;
class QPushButton;
class QStackedWidget;
class QTimer;
class QTreeWidget;
class QRectF;
class QWidget;

namespace amrvis {
struct DatasetMetadata;
struct LineResult;
namespace remote {
class Connection;
}
enum class CompositionPolicy : std::uint8_t;
}

namespace amrvis::qt {

class AnimationExporter;
class AnimationPanel;
class ColorBarWidget;
class DatasetWindow;
class FabSelectorDock;
class ImageView;
class IsoWidget;
class LinePlotWindow;
class ScientificDoubleSpinBox;
class DiagnosticsModel;
class FabNavigator;
class PaletteController;
class ParticleController;
class RangeController;
class RemoteSessionController;
class SequenceController;
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
    void startSshRemoteSession(std::string destination,
        std::string serverExecutable, std::vector<std::string> remotePaths);
    // Open a server-visible path, or a sequence of them, over the installed
    // remote connection.
    void openRemoteDataset(std::string remotePath);
    void openRemoteSequence(const std::vector<std::string>& remotePaths);
    // Opens a plotfile sequence (the legacy "-a" file animation): frames are
    // the plotfile directories, sorted by name; requires at least two valid
    // plotfiles. Opening a single dataset closes the sequence again.
    void openSequence(const std::vector<std::filesystem::path>& frames);
    // Steps the open sequence by direction frames, wrapping at the ends; the
    // same slot the sequence step buttons and the smoke test hook use.
    void stepSequence(int direction);
    // Starts an animation export without the interactive color-bar/save
    // dialogs, writing frames and MP4s under path's directory. Test-only entry
    // used by the export-quit smoke test to reach the encoder deterministically.
    void startAnimationExportForTest(const QString& path, bool includeColorBar);
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
    // The slot an idle frame-slider press-and-release lands in, which must not
    // restart a frame that is already on screen.
    void requestSequenceFrameForTest(int index) { goToSequenceFrame(index); }

    // Test-only: move each 3-D plane to slicePositions (per axis, so the three
    // panels sample different data with different local ranges), switch to
    // contour display (count levels) with the Visible range mode and optional
    // logarithmic mapping, then re-slice every view once. This is the exact
    // shape that exposed the stale-contour bug: three unequal local ranges
    // reconciled into one shared Visible range. interactiveSlicesSettled fires
    // when that batch finishes.
    void configureContourSyncForTest(
        int count, bool logarithmic, std::array<double, 3> slicePositions);

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
    [[nodiscard]] bool activeViewScrollBarsVisibleForTest() const;
    [[nodiscard]] bool activeViewHasPhysicalAspectForTest(
        double expectedAspect) const;
    // Test-only: the active view's raster has the aspect of the region it
    // covers measured in finest cells -- the display's unit, one square pixel
    // per cell -- rather than in physical units. The two differ only when
    // the cells are not square (see remote-fit-anisotropic-cells).
    [[nodiscard]] bool activeViewRasterHasCellAspectForTest() const;
    // Test-only: the Volume Rendering window -- open it as the View menu
    // action does, whether it is open, and what fraction of the last frame's
    // pixels the ray caster lit (alpha > 0); zero before any frame.
    void showVolumeWindowForTest();
    [[nodiscard]] bool volumeWindowOpenForTest() const;
    [[nodiscard]] double volumeFrameAlphaCoverageForTest() const;
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

    // Test-only, for the spherical supersample zoom-preserve regression:
    // change the warp factor through the same path as the menu, read the active
    // view's warped-pixmap width (to confirm the raster resized), and read
    // whether it is at fit-to-window without mutating it (unlike
    // activeViewIsFitToWindowForTest, which refits as a side effect).
    void setSphericalSupersampleForTest(int factor);
    [[nodiscard]] int activeViewImageWidthForTest() const;
    [[nodiscard]] std::array<int, 2> activeViewImageSizeForTest() const;
    [[nodiscard]] std::array<int, 2> activeViewViewportSizeForTest() const;
    [[nodiscard]] QImage activeViewViewportImageForTest() const;
    [[nodiscard]] bool activeViewFitsWindowForTest() const;

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
    // Invalid when the species has no stored color, which is what a reset
    // leaves behind until the next dataset re-seeds the defaults.
    [[nodiscard]] QColor particleColorForTest(const std::string& species) const;
    void setParticleColorForTest(
        const std::string& species, const QColor& color);
    [[nodiscard]] bool particleOverlaysUseColorForTest(
        const QColor& color);
    [[nodiscard]] std::size_t particleSampleCountForTest() const;
    [[nodiscard]] std::size_t particleOverlayCountForTest();
    [[nodiscard]] bool particleLoadingForTest() const noexcept;
    [[nodiscard]] bool particleLoadingUiActiveForTest() const;
    [[nodiscard]] bool particleLoadingUiSettledForTest() const;

signals:
    void datasetOpenFinished(bool success);
    void initialSliceFinished(bool success);
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
        std::optional<DisplayCoordinator::RasterGeometry> rasterGeometry;
        double displayMinimum = 0.0;
        double displayMaximum = 1.0;
        bool displayLogarithmic = false;
        std::vector<VectorSegment> vectorSegments;
        std::vector<SliceGridBox> gridBoxes;
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

    void chooseDataset();
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
    // A fresh independent top-level window (WA_DeleteOnClose) for the
    // "Open New Window" menu action; it shares no view/cache state with this one.
    MainWindow* createNewWindow();
    void exportImage();
    void exportAnimation();
    // Shared body of exportAnimation once the output path and color-bar choice
    // are known (from the dialogs, or from the test hook): freezes the export
    // zoom, starts the AnimationExporter (which owns the export state machine),
    // and kicks off frame 0.
    void beginAnimationExport(const QString& path, bool includeColorBar);
    [[nodiscard]] QImage composeExportFrame(const ImageView* view,
        bool includeColorBar, qreal scaleFactor) const;
    void createMenus();
    void rebuildLevelMenu();
    void rebuildVariableMenu();
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
    void showContoursDialog();
    // Draws the ParticleController's samples into a view: the projection and
    // the plane mapping are the host's, the settings and samples are its.
    void updateParticleOverlay(PlaneViewState& state);
    void updateParticleOverlays();
    void applyContourSettings(DisplayMode mode, int count, int uField, int vField,
        int wField, int contourColor);
    void showNumberFormatDialog();
    void applyNumberFormat(const QString& format);
    void validateVectorMode();
    void ensureVectorFieldDefaults();
    void showDatasetWindow();
    void closeDatasetWindow();
    void refreshDatasetWindow();
    void datasetCellActivated(const RealBox& physicalCell);
    [[nodiscard]] std::optional<DatasetRequest> buildDatasetRequest() const;
    void showUserGuide();
    void showKeyboardMouseReference();
    void showAboutDialog();
    void showMetadata(const PlotfileMetadataResult& result, const std::filesystem::path& path);
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

    // Per-view wiring and display updates.
    void wireView(PlaneViewState& state);
    // Every view state, whatever the current dimension -- the 2-D view and all
    // three slice panels. currentViews() answers a narrower question: the views
    // the *displayed* dataset uses. Teardown and failure states have to reach
    // all four, since the dimension they were showing is already gone.
    [[nodiscard]] std::array<PlaneViewState*, 4> allViewStates();
    void setAllViewPlaceholders(const QString& text);
    [[nodiscard]] std::vector<PlaneViewState*> currentViews();
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
    [[nodiscard]] PlaneMapping planeMapping(const PlaneViewState& state) const;
    // Enable/disable and re-check the 2-D Spherical menus for the current
    // dataset and display mode (Supersampling applies only to the R-Z warp).
    void updateSphericalControls();
    // Horizontal and vertical axis names for a spherical layout ({"R","Z"},
    // {"r","theta"}, or {"theta","r"}). Callers pass the displayed view
    // state's mode so labels always describe the raster on screen.
    [[nodiscard]] static std::array<QString, 2> sphericalAxisLabels(
        SphericalDisplay mode);
    void probeMoved(PlaneViewState& state, int x, int displayY);
    void probeClicked(PlaneViewState& state, int x, int displayY);
    [[nodiscard]] QString probeReadout(
        const PlaneViewState& state, int x, int displayY) const;
    void rubberBandZoom(PlaneViewState& state, const QRectF& sceneRect);
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
    // Spherical supersample change: the physical (R, Z) bounds are unchanged
    // but the warped pixmap is resized. Returns the scene rect that keeps the
    // currently-visible physical window on screen at the new resolution, or
    // nullopt when a plain refit is correct (first frame, dataset/domain
    // change, or no resolution change).
    [[nodiscard]] std::optional<QRectF> sphericalReframe(
        const PlaneViewState& state, const SliceDisplayResult& display) const;
    // By value, and callers move into it: the planes are the largest thing an
    // arrival carries -- at the 4096 output cap a ScalarPlane is around 117 MB
    // and the ImageBuffer around 67 MB -- and a const& forced this function to
    // deep-copy them again into the shared_ptr snapshots it publishes.
    void showSlice(PlaneViewState& state, SliceDisplayResult display);
    void updateOverlay(PlaneViewState& state);
    void updateOverlays();
    void updateGridBoxes(PlaneViewState& state);
    void updateGridBoxes();
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
    void syncVisibleRanges();
    // Panel slices currently on a worker (summed PlaneViewState::pendingRequests);
    // the visible-range sync defers dispatch until this is zero. Panel work only
    // -- excludes particle/line-plot/prefetch requests, which the
    // DiagnosticsModel's active count tracks.
    [[nodiscard]] int slicesInFlight() const;

    // Slice requests: the debounce timer coalesces into per-view requests.
    // rasterDirty false means the trigger (contour mode/count) cannot change
    // the raster, so a cache-satisfied request skips the image re-render.
    void scheduleSliceRequest(bool rasterDirty = true);
    void scheduleSliceRequest(PlaneViewState& state, bool rasterDirty = true);
    void flushSliceRequests();
    void requestSlice(PlaneViewState& state, bool rasterDirty);
    void requestInitialSlice(const std::filesystem::path& path,
        std::uint64_t generation,
        std::optional<PlotfileMetadataResult> preparedMetadata = std::nullopt,
        std::filesystem::path dataRoot = {},
        std::optional<FrameSliceSpec> initialSpec = std::nullopt,
        std::shared_ptr<DatasetSession> preparedSession = {});
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
    void configureSequenceControls(bool defaultPositions);
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
    ColorBarWidget* m_colorBar = nullptr;
    LinePlotWindow* m_linePlotWindow = nullptr;
    // Cancels in-flight line-plot queries on dataset switch or window close so
    // a late result neither reopens a closed window nor wastes I/O.
    StopSource m_linePlotStopSource;
    DatasetWindow* m_datasetWindow = nullptr;
    SetContoursDialog* m_contoursDialog = nullptr;
    QDialog* m_numberFormatDialog = nullptr;
    UserGuideDialog* m_userGuideDialog = nullptr;
    QComboBox* m_fieldSelector = nullptr;
    QComboBox* m_levelSelector = nullptr;
    // Owns the range mode, User min/max and Log widgets and the per-field
    // range memory; selection() feeds every slice request and frame spec.
    RangeController* m_range = nullptr;
    QWidget* m_slicePositionControls = nullptr;
    QAction* m_positionSeparator = nullptr;
    std::array<QSpinBox*, 3> m_sliceSpinboxes{nullptr, nullptr, nullptr};
    QTimer* m_sliceDebounce = nullptr;
    QTimer* m_panDebounce = nullptr;
    PlaneViewState* m_panView = nullptr;
    RealBox m_panStartRegion{};
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
    bool m_visibleSyncInFlight = false;
    bool m_visibleSyncRerun = false;
    std::optional<amrvis::DisplayCoordinator::RangeKey> m_pendingRangeStore;
#ifdef AMREXPLORER_QT_TEST_ACCESS
    // Test-only: superseded visible-range sync outcomes dropped by the
    // rerun guard. Sole writer is that drop, so the overlapping-sync test can
    // assert an exact count. The DiagnosticsModel's stale count carries the
    // same event for the user-facing diagnostics panel.
    std::uint64_t m_visibleSyncStaleSkips = 0;
#endif
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
    // Supersampling is its first child; more options will join it.
    QMenu* m_sphericalMenu = nullptr;
    QMenu* m_sphericalDisplayMenu = nullptr;
    QActionGroup* m_sphericalDisplayGroup = nullptr;
    QMenu* m_sphericalSupersampleMenu = nullptr;
    QActionGroup* m_sphericalSupersampleGroup = nullptr;
    QActionGroup* m_scaleGroup = nullptr;
    QActionGroup* m_levelGroup = nullptr;
    QActionGroup* m_variableGroup = nullptr;
    QAction* m_boxesAction = nullptr;
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
    std::shared_ptr<DatasetSession> m_dataset;
    std::shared_ptr<const DatasetMetadata> m_openMetadata;
    std::string m_fileVersion;
    PlaneViewState m_view2d;
    std::array<PlaneViewState, 3> m_planeViews;
    PlaneViewState* m_activeView = nullptr;
    int m_viewDimension = 0;
    std::array<double, 3> m_slicePosition3d{0.0, 0.0, 0.0};
    bool m_pendingAllViews = false;
    std::vector<PlaneViewState*> m_pendingViews;
    // OR of the rasterDirty flags of the coalesced pending requests.
    bool m_pendingRasterDirty = false;
    StopSource m_initialStopSource;
    StopSource m_metadataStopSource;
    DisplayMode m_displayMode = DisplayMode::Raster;
    int m_contourCount = 15;
    // 2-D spherical warp supersample factor (see SliceRequest::sphericalSupersample).
    int m_sphericalSupersample = 4;
    // 2-D spherical display layout (see SliceRequest::sphericalDisplay).
    SphericalDisplay m_sphericalDisplay = SphericalDisplay::RZ;
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
    QString m_numberFormat = defaultNumberFormat();
    bool m_controlsReady = false;
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
