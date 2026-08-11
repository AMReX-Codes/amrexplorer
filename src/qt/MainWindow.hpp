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
#include <QStringList>

#include <array>
#include <atomic>
#include <cstdint>
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
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDockWidget;
class QLabel;
class QSpinBox;
class QLineF;
class QMenu;
class QPlainTextEdit;
class QProgressDialog;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTimer;
class QTreeWidget;
class QRectF;
class QWidget;

namespace amrvis {
struct DatasetMetadata;
struct LineResult;
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
class SequenceController;
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

    void openDataset(const std::filesystem::path& path, bool metadataOnly = false);
    void openRemoteDataset(std::string host, std::uint16_t port,
        std::string remotePath, std::string token);
    void openRemoteSequence(std::string host, std::uint16_t port,
        const std::vector<std::string>& remotePaths, std::string token);
    // Cheap connect-time check: runs a handshake (and ping) off the GUI thread
    // to confirm the endpoint is reachable and the token is accepted, without
    // opening a dataset.
    void verifyRemoteEndpoint(
        std::string host, std::uint16_t port, std::string token);
    // Persists only the remote port, to prefill the Connect dialog after a
    // client restart against a still-running server. The token is never
    // written to disk.
    void saveRemoteSettings();
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

    // Test-only: move each 3-D plane to slicePositions (per axis, so the three
    // panels sample different data with different local ranges), switch to
    // contour display (count levels) with the Visible range mode and optional
    // logarithmic mapping, then re-slice every view once. This is the exact
    // shape that exposed the stale-contour bug: three unequal local ranges
    // reconciled into one shared Visible range. interactiveSlicesSettled fires
    // when that batch finishes.
    void configureContourSyncForTest(
        int count, bool logarithmic, std::array<double, 3> slicePositions);

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

    // Test-only: true when the active view's displayed raster is byte-identical
    // to its plane re-rendered against the current display (color-bar) range —
    // i.e. the raster and color bar agree. See
    // raster-colorbar-mismatch-on-2d-visible-zoom.
    // Test-only: the placeholder every panel is showing, or an empty string if
    // any panel holds an image instead. A failed open must leave a settled
    // placeholder naming what failed, never the "Loading..." one it replaced.
    [[nodiscard]] QString viewPlaceholderForTest();
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
    // Test-only: send a real Shift+left drag through the active view's
    // viewport, exercising the same event path as interactive panning.
    void shiftDragActiveViewForTest(int dx, int dy);
    [[nodiscard]] bool activeViewScrollBarsVisibleForTest() const;
    [[nodiscard]] bool activeViewHasPhysicalAspectForTest(
        double expectedAspect) const;
    [[nodiscard]] bool fabStateClearedForTest() const;
    void setGridBoxesVisibleForTest(bool visible);
    [[nodiscard]] std::size_t activeViewGridBoxCountForTest() const;

    // Test-only: rubber-band the central half of the active 3-D panel through
    // the same handler used by ImageView::rubberBandSelected.
    void rubberBandZoomActiveViewForTest();
    void rubberBandZoomRectangularActiveViewForTest();

    // Test-only: true when every current panel has a strict visible subregion.
    // Used to lock down synchronized 3-D rubber-band zoom.
    [[nodiscard]] bool allViewsRubberBandZoomedForTest();
    [[nodiscard]] std::size_t rubberBandZoomedViewCountForTest();

    // Test-only: apply a panel-local scale, drive the exact data-region pan
    // handlers used by Shift+left drag, and inspect the resulting transform.
    void setActiveViewScaleForTest(int factor);
    void selectFixedScaleForTest(int factor);
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
        // shared snapshots, never null (empty planes when nothing is shown):
        // arrivals REPLACE the pointer and never mutate the pointee. The
        // cached-planes refresh worker (requestSlice's fromCache path)
        // captures these shared_ptrs — a refcount bump instead of the former
        // ~110 MB deep copy on the GUI thread — and can keep reading its
        // snapshots safely while a newer arrival swaps the view's pointers.
        std::shared_ptr<const ScalarPlane> plane
            = std::make_shared<const ScalarPlane>();
        // Contour-mode companions of plane: the data-resolution plane the
        // contours were extracted from, its bilinear refinement, and the
        // display-space polylines. Cleared and updated exactly where plane
        // is; together with the cache key below they let range, palette,
        // and contour-count changes refresh without a new SliceQuery.
        std::shared_ptr<const ScalarPlane> contourPlane
            = std::make_shared<const ScalarPlane>();
        std::shared_ptr<const ScalarPlane> contourFinePlane
            = std::make_shared<const ScalarPlane>();
        int contourFineFactor = 1;
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
        // Slice requests currently on a worker for this view; the sweep
        // playback skips ticks while one is in flight.
        int pendingRequests = 0;
    };

    void chooseDataset();
    void chooseStandaloneDataset(const QString& caption, bool rawFab);
    void openDatasetImpl(const std::filesystem::path& path, bool metadataOnly,
        std::optional<PlotfileMetadataResult> preparedMetadata,
        std::filesystem::path dataRoot, bool preserveFabSelector,
        std::optional<FrameSliceSpec> initialSpec,
        std::optional<
            std::tuple<std::string, std::uint16_t, std::string, std::string>>
            remoteOpen = std::nullopt);

    // Clears standalone FAB/MultiFab view state (mode flag, MultiFab-return
    // record, source metadata/paths) and hides the FAB selector dock. Any path
    // that replaces the displayed dataset with a non-FAB one -- a plain dataset
    // open, or a plotfile sequence -- must call this or stale FAB state leaks
    // into the new view (see open-sequence-stale-fab-state).
    void resetFabState();

    void viewFab(std::size_t entry);
    void backToMultiFab();
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
    void syncPaletteChecks();
    void syncPaletteSelector();
    void selectBuiltinPalette(int index);
    void loadPaletteFile();
    void applyPalette(const Palette& palette, std::optional<int> builtinIndex,
        const QString& filePath);
    // Derives m_palette from m_basePalette and the reverse toggle, then pushes
    // it to the color bar, iso widget, and a re-render. Shared by applyPalette
    // and the reverse-colormap toggle.
    void refreshPaletteDisplay();
    // Sets the reverse-colormap state and keeps the menu action, toolbar
    // selector, settings, and rendering in sync. The single entry point for
    // both the menu toggle and the palette-selector toggle item.
    void setReversePalette(bool reversed);
    // Per-field user range: each field remembers its own RangeMode and, when
    // User, its min/max. commitFieldRange snapshots the current widgets for a
    // field, applyFieldRange loads a field's snapshot (or the Visible default)
    // back into the widgets, and resetRangeState clears everything for a fresh
    // dataset.
    void commitFieldRange(std::uint32_t field);
    void applyFieldRange(std::uint32_t field);
    void resetRangeState();
    void updateRangeModeAvailability();
    void showContoursDialog();
    void showParticlesDialog();
    void configureParticleControls(bool preserveSelection);
    void requestParticleReload();
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
    void updateDiagnostics();
    void reportBackgroundError(const QString& message);
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
    void setupPanShortcuts();
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
    void showSlice(PlaneViewState& state, const SliceDisplayResult& display);
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
    // runs on a worker over the panels' immutable plane snapshots;
    // completions apply per panel only while its snapshot is still the
    // displayed one (pointer identity). Coalesced single-flight: a call while
    // a sync worker is in flight marks a rerun instead of stacking workers.
    void syncVisibleRanges();

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
    void applyParticleSelection(
        std::vector<std::string> species, double fraction, int pointSize,
        std::uint64_t seed);
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
    QComboBox* m_rangeMode = nullptr;
    QComboBox* m_paletteSelector = nullptr;
    QCheckBox* m_logarithmic = nullptr;
    ScientificDoubleSpinBox* m_rangeMinimum = nullptr;
    ScientificDoubleSpinBox* m_rangeMaximum = nullptr;
    // Per-field range state for the current dataset. m_trackedField is the
    // field the range widgets currently represent; the field selector swaps
    // snapshots through this map when the user changes fields.
    struct FieldRange {
        RangeMode mode = RangeMode::File;
        std::optional<std::pair<double, double>> userRange;
    };
    std::unordered_map<std::uint32_t, FieldRange> m_fieldRanges;
    std::uint32_t m_trackedField = 0;
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
    QTreeWidget* m_metadataTree = nullptr;
    QPlainTextEdit* m_diagnostics = nullptr;
    QDockWidget* m_metadataDock = nullptr;
    QDockWidget* m_diagnosticsDock = nullptr;
    QDockWidget* m_colorBarDock = nullptr;
    QDockWidget* m_animationDock = nullptr;
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
    QActionGroup* m_paletteGroup = nullptr;
    QAction* m_reversePaletteAction = nullptr;
    QAction* m_boxesAction = nullptr;
    QAction* m_slicePlanesAction = nullptr;
    QAction* m_resetZoomAction = nullptr;
    QAction* m_syncRubberBandZoomAction = nullptr;
    QAction* m_contoursAction = nullptr;
    QAction* m_particlesAction = nullptr;
    QProgressBar* m_particleProgress = nullptr;
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
    StopSource m_remoteVerifyStopSource;
    std::uint64_t m_remoteVerifyGeneration = 0;
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
    std::vector<std::string> m_selectedParticleSpecies;
    std::vector<ParticleSample> m_particleSamples;
    std::unordered_map<std::string, QColor> m_particleColors;
    double m_particleFraction = 1.0;
    std::uint64_t m_particleSeed = 0;
    int m_particlePointSize = 3;
    bool m_particleSelectionInitialized = false;
    bool m_particleLoading = false;
    StopSource m_particleStopSource;
    std::uint64_t m_particleGeneration = 0;
    std::filesystem::path m_datasetPath;
    std::string m_remoteHost;
    std::uint16_t m_remotePort = 0;
    std::string m_remoteToken;
    bool m_remoteSequence = false;
    std::uint64_t m_remoteSequenceConnectionGeneration = 0;
    struct MultiFabReturnState {
        std::filesystem::path path;
        std::filesystem::path dataRoot;
        PlotfileMetadataResult metadata;
        FrameSliceSpec spec;
    };
    std::optional<MultiFabReturnState> m_multifabReturn;
    std::optional<PlotfileMetadataResult> m_fabSourceMetadata;
    std::filesystem::path m_fabSourcePath;
    std::filesystem::path m_fabDataRoot;
    bool m_fabMode = false;
    // The selected palette before any reversal; m_palette is derived from it as
    // m_reversePalette ? m_basePalette.reversed() : m_basePalette and is the
    // value the renderer, color bar, and overlays actually use.
    Palette m_basePalette = builtinPalette(BuiltinPalette::Rainbow);
    Palette m_palette = builtinPalette(BuiltinPalette::Rainbow);
    int m_builtinIndex = 0;
    bool m_paletteFromFile = false;
    bool m_reversePalette = false;
    QString m_paletteFilePath;
    QString m_numberFormat = defaultNumberFormat();
    QStringList m_probeLines;
    QStringList m_backgroundErrors;
    bool m_controlsReady = false;
    std::uint64_t m_generation = 0;
    std::uint64_t m_activeRequests = 0;
    bool m_closing = false;
    std::uint64_t m_staleResults = 0;
    std::uint64_t m_lastFilesRead = 0;
    std::uint64_t m_lastBytesRead = 0;
    std::uint64_t m_lastBlocksRead = 0;
    std::uint64_t m_lastCacheHits = 0;
    std::uint64_t m_lastPayloadBytesRead = 0;
    std::uint64_t m_cacheBudgetBytes = 0;
    std::uint64_t m_cacheResidentBytes = 0;
    std::uint64_t m_cachePinnedBytes = 0;
    std::uint64_t m_cacheEvictions = 0;

    // Animation state. The sequence frames/index/in-flight/prefetch state
    // machine lives in the SequenceController; this window keeps only the
    // playback timer and mode.
    AnimationPanel* m_animationPanel = nullptr;
    QTimer* m_playbackTimer = nullptr;
    PlaybackMode m_playbackMode = PlaybackMode::None;
    SequenceController* m_sequenceController = nullptr;
};

} // namespace amrvis::qt
