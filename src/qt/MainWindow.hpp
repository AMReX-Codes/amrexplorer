#pragma once

#include "DatasetWindow.hpp"
#include "NumberFormat.hpp"
#include "SetContoursDialog.hpp"

#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/query/SliceQuery.hpp>
#include <amrexplorer/render2d/Contours.hpp>
#include <amrexplorer/render2d/ImageBuffer.hpp>
#include <amrexplorer/render2d/Palette.hpp>
#include <amrexplorer/render2d/VectorGlyphs.hpp>

#include <QElapsedTimer>
#include <QImage>
#include <QMainWindow>
#include <QStringList>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class QAction;
class QActionGroup;
class QCheckBox;
class QCloseEvent;
class QColor;
class QComboBox;
class QDockWidget;
class QLabel;
class QSpinBox;
class QLineF;
class QMenu;
class QPlainTextEdit;
class QProgressDialog;
class QPushButton;
class QStackedWidget;
class QTimer;
class QTreeWidget;
class QRectF;
class QWidget;

namespace amrvis {
class PlotfileDataset;
struct DatasetMetadata;
struct LineResult;
enum class CompositionPolicy : std::uint8_t;
}

namespace amrvis::qt {

class AnimationPanel;
class ColorBarWidget;
class DatasetWindow;
class FabSelectorDock;
class ImageView;
class IsoWidget;
class LinePlotWindow;
class ScientificDoubleSpinBox;
class UserGuideDialog;

enum class RangeMode {
    Visible,
    Level,
    File,
    User
};

struct SliceDisplayResult {
    // The request that produced everything below; PlaneViewState keeps it as
    // the cache key for the re-render-from-cache path (see requestSlice).
    SliceRequest request;
    SliceQueryResult slice;
    ImageBuffer image;
    std::vector<VectorSegment> vectors;
    // Contour modes only: the piecewise plane at data resolution the
    // contours were extracted from (the display plane itself when the data
    // is finer than the display), its bilinear refinement, and the
    // polylines extracted from that refinement, already mapped to
    // display-plane pixel space (empty otherwise).
    ScalarPlane contourPlane;
    ScalarPlane contourFinePlane;
    int contourFineFactor = 1;
    std::vector<ContourPolyline> contourPolylines;
    std::string fieldName;
    double minimum = 0.0;
    double maximum = 1.0;
    bool logarithmic = false;
    DisplayMode mode = DisplayMode::Raster;
    std::uint32_t vectorUField = 0;
    std::uint32_t vectorVField = 0;
    int contourCount = 0;
    // Set when the image was intentionally not re-rendered (contour-only
    // refresh): showSlice keeps the view's current pixmap.
    bool rasterUnchanged = false;
};

struct InitialSliceResult {
    std::shared_ptr<PlotfileDataset> dataset;
    // One entry per displayed view, ordered by normal axis (2-D: one entry).
    std::vector<SliceDisplayResult> displays;
    // First line of the plotfile Header when the path is a plotfile
    // directory; empty for standalone datasets.
    std::string fileVersion;
    // Set when a Finest Available load exceeded the cache budget and was
    // retried with a lower composite maximum level.
    int cacheFallbackFromLevel = -1;
    int cacheFallbackToLevel = -1;
};

// Everything needed to render one frame's slice(s) off the GUI thread. The
// sequence path builds this from the current UI state so frame switches keep
// the user's field/level/range/log/palette/visible-region settings; empty or
// default entries mean "fall back to the new dataset's defaults" (midpoint
// slice positions, whole domain, 640x640 output).
struct FrameSliceSpec {
    DisplayMode displayMode = DisplayMode::Raster;
    std::uint32_t field = 0;
    int levelSelection = -1;  // level combo data: -1 = finest available
    RangeMode rangeMode = RangeMode::File;
    std::optional<std::pair<double, double>> userRange;
    bool logarithmic = false;
    Palette palette;
    std::uint32_t vectorUField = 0;
    std::uint32_t vectorVField = 0;
    std::uint32_t vectorWField = 0;
    int contourCount = 10;
    bool defaultPositions = true;
    std::array<double, 3> slicePositions{0.0, 0.0, 0.0};
    std::vector<std::optional<RealBox>> visibleRegions;  // per view, normal order
    std::vector<std::array<int, 2>> outputSizes;         // per view, normal order
};

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    void openDataset(const std::filesystem::path& path, bool metadataOnly = false);
    // Opens a plotfile sequence (the legacy "-a" file animation): frames are
    // the plotfile directories, sorted by name; requires at least two valid
    // plotfiles. Opening a single dataset closes the sequence again.
    void openSequence(const std::vector<std::filesystem::path>& frames);
    // Steps the open sequence by direction frames, wrapping at the ends; the
    // same slot the sequence step buttons and the smoke test hook use.
    void stepSequence(int direction);

signals:
    void datasetOpenFinished(bool success);
    void initialSliceFinished(bool success);
    // Emitted once a sequence frame's slice(s) are on screen; the offscreen
    // smoke test drives frame stepping off it.
    void sequenceFrameDisplayed(int index);
    void sequenceFrameFailed();

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
        ScalarPlane plane;
        // Contour-mode companions of plane: the data-resolution plane the
        // contours were extracted from, its bilinear refinement, and the
        // display-space polylines. Cleared and updated exactly where plane
        // is; together with the cache key below they let range, palette,
        // and contour-count changes refresh without a new SliceQuery.
        ScalarPlane contourPlane;
        ScalarPlane contourFinePlane;
        int contourFineFactor = 1;
        std::vector<ContourPolyline> contourPolylines;
        QString fieldName;
        std::optional<RealBox> visibleRegion;
        double displayMinimum = 0.0;
        double displayMaximum = 1.0;
        bool displayLogarithmic = false;
        std::vector<VectorSegment> vectorSegments;
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

    // One prefetched sequence frame: the dataset plus its rendered slice(s),
    // consumable only while the slice spec that produced it is unchanged.
    struct PrefetchedFrame {
        int frameIndex = -1;
        std::uint64_t specGeneration = 0;
        bool defaultPositions = false;
        InitialSliceResult result;
    };

    void chooseDataset();
    void chooseStandaloneDataset(const QString& caption, bool rawFab);
    void openDatasetImpl(const std::filesystem::path& path, bool metadataOnly,
        std::optional<PlotfileMetadataResult> preparedMetadata,
        std::filesystem::path dataRoot, bool preserveFabSelector,
        std::optional<FrameSliceSpec> initialSpec);
    void configureFabSelector(const PlotfileMetadataResult& result,
        const std::filesystem::path& path);
    void viewFab(std::size_t entry);
    void backToMultiFab();
    // A fresh independent top-level window (WA_DeleteOnClose) for the
    // "Open New Window" menu action; it shares no view/cache state with this one.
    MainWindow* createNewWindow();
    void exportImage();
    void exportAnimation();
    // Animation export is signal-driven (frame rendering is async): exportAnimation
    // kicks off frame 0; onExportFrameDisplayed saves each rendered frame and
    // advances; finalizeExportAnimation encodes the MP4; endExportAnimation is the
    // shared cleanup/restore/report path.
    void onExportFrameDisplayed(int index);
    void onExportFrameFailed();
    void finalizeExportAnimation();
    void endExportAnimation(bool success, const QString& message);
    [[nodiscard]] QImage composeExportFrame(const ImageView* view,
        bool includeColorBar, qreal scaleFactor) const;
    [[nodiscard]] bool probeFfmpeg() const;
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
    void updateAnimationDockVisibility();
    void updateWindowTitle();
    void restoreSettings();
    void saveSettings();

    // Per-view wiring and display updates.
    void wireView(PlaneViewState& state);
    [[nodiscard]] std::vector<PlaneViewState*> currentViews();
    void setActiveView(PlaneViewState& state);
    [[nodiscard]] std::array<int, 2> displayAxes(int normal) const;
    void probeMoved(PlaneViewState& state, int x, int displayY);
    void probeClicked(PlaneViewState& state, int x, int displayY);
    [[nodiscard]] QString probeReadout(
        const PlaneViewState& state, int x, int displayY) const;
    void rubberBandZoom(PlaneViewState& state, const QRectF& sceneRect);
    void beginPanDrag(PlaneViewState& state);
    void updatePanDrag(PlaneViewState& state, const QPointF& totalSceneDelta,
        const QPoint& viewportDelta);
    void endPanDrag(PlaneViewState& state, const QPointF& totalSceneDelta);
    void flushPanDrag(bool finalize);
    void applyPanStep(PlaneViewState& state, const QPointF& direction);
    void setupPanShortcuts();
    [[nodiscard]] std::optional<RealBox> shiftedPanRegion(
        const PlaneViewState& state, const RealBox& baseRegion,
        int planeWidth, int planeHeight, const QPointF& sceneDelta) const;
    void linePlotRequested(PlaneViewState& state, int imageX, int imageY,
        Qt::MouseButton button);
    void sliceMoveRequested(PlaneViewState& state, int imageX, int imageY,
        Qt::MouseButton button);
    void fitView(PlaneViewState& state);
    void fitViewToWindow();
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
    void setSlicePosition(int axis, double value);
    [[nodiscard]] int sliceIndexLevel() const;
    // Visible-range mode in 3-D: recompute the min/max from all three panels'
    // planes so the single color bar maps them consistently.
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
        std::optional<FrameSliceSpec> initialSpec = std::nullopt);
    void configureSliceControls();
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
    void closeSequence();
    void goToSequenceFrame(int index);
    void toggleSequencePlayback();
    void stepSweep(int direction);
    void toggleSweepPlayback();
    void setPlaybackMode(PlaybackMode mode);
    void playbackTick();
    void applySpeed();

    // Sequence frame switching. At most one sync frame load and one
    // prefetch are alive; everything superseded is cancelled via stop
    // sources and discarded via generation counters.
    void startFrameLoad(int index, std::uint64_t generation);
    void finishFrameLoad(InitialSliceResult result, bool defaultPositions);
    void displayFrameResult(InitialSliceResult& result, bool defaultPositions);
    void configureSequenceControls(bool defaultPositions);
    [[nodiscard]] FrameSliceSpec buildFrameSpec();
    void startPrefetch(int frameIndex);
    void discardPrefetch();
    // Stop timers, request stop on every async task this window can launch,
    // and clear queued thread-pool work. Wired to aboutToQuit so the process
    // exits promptly instead of blocking QThreadPool teardown on an in-flight
    // read that holds the global I/O mutex.
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
    // Full-domain range cache — kept current whenever a non-zoomed slice
    // completes, and reused for RangeMode::Visible during zoom/pan so the
    // color bar stays stable instead of tracking the subregion extrema.
    std::optional<std::pair<double, double>> m_fullDomainRange;
    FieldId m_fullDomainRangeField{};
    int m_fullDomainRangeMaxLevel = -1;
    CompositionPolicy m_fullDomainRangeComposition{};
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
    QActionGroup* m_levelGroup = nullptr;
    QActionGroup* m_variableGroup = nullptr;
    QActionGroup* m_paletteGroup = nullptr;
    QAction* m_boxesAction = nullptr;
    QAction* m_fitScaleAction = nullptr;
    QAction* m_contoursAction = nullptr;
    QAction* m_datasetAction = nullptr;
    QAction* m_exportAnimationAction = nullptr;

    // Drives File -> Export Animation... Active only while an export is running;
    // sequenceFrameDisplayed advances it one frame at a time.
    struct ExportAnimationState {
        bool active = false;
        bool canceled = false;
        bool framesDone = false;
        bool includeColorBar = false;
        bool hasFfmpeg = false;
        qreal scale = 1.0;  // frozen export zoom factor so every frame matches
        int totalFrames = 0;
        int restoreIndex = -1;
        int digitWidth = 5;
        QString directory;
        QString stem;
        QProgressDialog* progress = nullptr;
    };
    ExportAnimationState m_exportAnim;
    std::shared_ptr<PlotfileDataset> m_dataset;
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
    DisplayMode m_displayMode = DisplayMode::Raster;
    int m_contourCount = 15;
    int m_contourColor = contourColorBlack;
    int m_vectorUField = -1;
    int m_vectorVField = -1;
    int m_vectorWField = -1;
    std::filesystem::path m_datasetPath;
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
    Palette m_palette = builtinPalette(BuiltinPalette::Rainbow);
    int m_builtinIndex = 0;
    bool m_paletteFromFile = false;
    QString m_paletteFilePath;
    QString m_numberFormat = defaultNumberFormat();
    QStringList m_probeLines;
    bool m_controlsReady = false;
    std::uint64_t m_generation = 0;
    std::uint64_t m_activeRequests = 0;
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

    // Animation state.
    AnimationPanel* m_animationPanel = nullptr;
    QTimer* m_playbackTimer = nullptr;
    PlaybackMode m_playbackMode = PlaybackMode::None;
    std::vector<std::filesystem::path> m_sequenceFrames;
    int m_sequenceIndex = -1;
    bool m_sequenceInFlight = false;
    // Bumped by every slice-affecting UI change; prefetched frames store the
    // value they were built against and are discarded once it moves on.
    std::uint64_t m_specGeneration = 0;
    // Bumped whenever the prefetch slot is cancelled/invalidated, so a late
    // prefetch watcher knows to drop its result.
    std::uint64_t m_prefetchGeneration = 0;
    std::uint64_t m_sequenceDatasetCounter = 0;
    StopSource m_prefetchStopSource;
    std::optional<PrefetchedFrame> m_prefetched;
    QElapsedTimer m_frameTimer;
    qint64 m_lastFrameSwitchMs = 0;
};

} // namespace amrvis::qt
