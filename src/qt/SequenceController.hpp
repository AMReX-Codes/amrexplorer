#pragma once

#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <QObject>
#include <QElapsedTimer>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace amrvis::qt {

// The plotfile-sequence state machine extracted from MainWindow: owns the
// frame list, the current index, the in-flight flag, the single-slot
// prefetch with its spec/stop generations, the per-sequence dataset-id
// counter, and the frame-switch timing. It runs the frame-load and prefetch
// workers (executeFrameLoad on the global pool) and their watchers; the host
// window supplies the GUI-coupled pieces through Hooks, reacts to the
// signals below, and keeps its public openSequence/stepSequence surface as
// thin wrappers over this controller.
class SequenceController final : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        // Snapshot of the current UI state for a frame load (the host's
        // buildFrameSpec).
        std::function<FrameSliceSpec()> buildSpec;
        // Displays a loaded frame in the views; throws to report a failure
        // (the host's displayFrameResult).
        std::function<void(InitialSliceResult&, bool defaultPositions)>
            displayFrame;
        // True once application shutdown began; late watcher results are
        // dropped without touching the GUI.
        std::function<bool()> isShuttingDown;
    };

    SequenceController(Hooks hooks, QObject* parent = nullptr);

    // Takes ownership of a validated, sorted, deduplicated frame list and
    // navigates to frame 0. The host performs validation and its own UI
    // setup before calling.
    void open(std::vector<std::filesystem::path> frames);
    // Adopts a frame that was fully loaded outside the controller. This is the
    // transactional viewer-state import seam: no frame-zero load occurs.
    void adoptPrepared(std::vector<std::filesystem::path> frames, int index,
        InitialSliceResult result, bool defaultPositions);
    // Drops the sequence and cancels the in-flight load and prefetch. The
    // host hides its sequence UI itself.
    void close();
    void step(int direction);
    // Navigates to a frame. forceRestart reloads even the current frame while
    // it is already in flight, so a slice-affecting change (field, level,
    // range, particle selection, ...) mid-load is picked up by a fresh load
    // built from the new spec instead of being ignored.
    void goToFrame(int index, bool forceRestart = false);

    // A slice-affecting UI change invalidates any prefetched frame rendered
    // against the old spec.
    void invalidatePrefetch();
    // Application shutdown: cooperative stop for the load/prefetch workers.
    void cancelActiveWork();

    [[nodiscard]] bool hasSequence() const noexcept
    {
        return !m_frames.empty();
    }
    [[nodiscard]] int frameCount() const noexcept
    {
        return static_cast<int>(m_frames.size());
    }
    [[nodiscard]] int currentIndex() const noexcept { return m_index; }
    [[nodiscard]] bool inFlight() const noexcept { return m_inFlight; }
    [[nodiscard]] qint64 lastFrameSwitchMs() const noexcept
    {
        return m_lastFrameSwitchMs;
    }
    [[nodiscard]] const std::filesystem::path& framePath(int index) const
    {
        return m_frames[static_cast<std::size_t>(index)];
    }
    [[nodiscard]] const std::vector<std::filesystem::path>& frames() const noexcept
    {
        return m_frames;
    }

signals:
    // Emitted synchronously at the start of every frame switch, before the
    // load begins: the host cancels the previous frame's in-flight work and
    // closes per-dataset windows, exactly like opening a fresh dataset, but
    // keeps the view state for the next frame.
    void frameSwitchStarted(int index);
    // Background-load bookkeeping for the host's diagnostics (+1 when a
    // load or prefetch worker starts, -1 when its watcher fires).
    void loadActivityChanged(int delta);
    void statusMessage(const QString& message);
    // A frame is on screen (displayFrame returned): the host updates its
    // panel/diagnostics and forwards its public sequenceFrameDisplayed.
    void frameDisplayed(int index);
    // Loading or displaying the current frame failed; the host reports it
    // (suppressing its own dialog while an export is active) and forwards
    // its public sequenceFrameFailed.
    void frameLoadFailed(const QString& message);
    // A load or prefetch result arrived after being superseded and was
    // dropped; the host counts these in its diagnostics.
    void staleResultDropped();

private:
    // One prefetched sequence frame: the dataset plus its rendered slice(s),
    // consumable only while the slice spec that produced it is unchanged.
    struct PrefetchedFrame {
        int frameIndex = -1;
        std::uint64_t specGeneration = 0;
        bool defaultPositions = false;
        InitialSliceResult result;
    };

    void startLoad(int index, std::uint64_t generation);
    void finishLoad(InitialSliceResult result, bool defaultPositions);
    void startPrefetch(int frameIndex);
    void discardPrefetch();

    Hooks m_hooks;
    std::vector<std::filesystem::path> m_frames;
    int m_index = -1;
    bool m_inFlight = false;
    // Invalidates in-flight loads across frame switches and close(); the
    // watcher also re-checks the index so a stale result never displays.
    std::uint64_t m_loadGeneration = 0;
    std::uint64_t m_specGeneration = 0;
    std::uint64_t m_prefetchGeneration = 0;
    std::uint64_t m_datasetCounter = 0;
    std::optional<PrefetchedFrame> m_prefetched;
    StopSource m_loadStopSource;
    StopSource m_prefetchStopSource;
    QElapsedTimer m_frameTimer;
    qint64 m_lastFrameSwitchMs = 0;
};

} // namespace amrvis::qt
