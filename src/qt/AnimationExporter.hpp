#pragma once

#include "ExportFrame.hpp"

#include <QFuture>
#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

class QProgressDialog;
class QWidget;

namespace amrvis::qt {

// Drives the signal-driven animation-export loop extracted from MainWindow:
// advances the open sequence one frame at a time as each renders, saves the
// rendered frame(s) as PNGs, then encodes MP4s with FFmpeg on worker
// threads with bounded, cancellable waits. Owns every piece of export state
// (progress dialog, cancellation flags, encoder bookkeeping — previously
// raw-new'd shared counters that leaked on early teardown). The host window
// supplies frame rendering and sequence navigation as callbacks, forwards
// sequenceFrameDisplayed/Failed here, and reacts to finished() by restoring
// its UI; it never reaches into the exporter's state.
class AnimationExporter final : public QObject {
    Q_OBJECT

public:
  // Renders the current frame for export: one (fileSuffix, image) pair per
  // panel ("" for the single 2-D view, "_yz"/"_xz"/"_xy" in 3-D). A null
  // image or an absent panel aborts the export. Layouts are initialized by
  // the renderer on frame 0 and then reused; exceptions explain failures.
  using FrameRenderer = std::function<std::vector<std::pair<QString, QImage>>(
      const ExportOptions& options, qreal scale, std::map<QString, ExportLayout>& layouts)>;
  // Navigates the sequence to the given frame (the host's
  // goToSequenceFrame); the next sequenceFrameDisplayed continues the loop.
  using AdvanceFrame = std::function<void(int index)>;

  AnimationExporter(FrameRenderer renderFrames, AdvanceFrame advanceFrame,
                    QObject* parent = nullptr);

  // Starts an export writing <stem><suffix>_<index>.png next to `path`
  // (whose directory and basename become the output location and stem) and
  // one <stem><suffix>.mp4 per panel suffix. panelSuffixes is frozen for
  // the whole export. Returns false when an export is already running or
  // there is nothing to export. The caller is expected to navigate to
  // frame 0 afterwards (mirroring the pre-extraction flow).
  [[nodiscard]] bool begin(const QString& path, const ExportOptions& options, int totalFrames,
                           int restoreIndex, qreal scale, std::vector<QString> panelSuffixes,
                           QWidget* dialogParent);

  [[nodiscard]] bool active() const noexcept { return m_active; }

  // Application shutdown: dismiss the progress dialog and signal the
  // encoder workers to terminate their FFmpeg processes.
  void cancelForShutdown();

  // Forwarded sequence events: a rendered frame continues the loop; a
  // failed frame aborts the export (the host suppresses its own error
  // dialog while an export is active — endExport reports instead).
  void onFrameDisplayed(int index);
  void onFrameFailed();

signals:
    // Frames are written; the FFmpeg workers are about to run. The
    // export-quit smoke test quits here to exercise bounded encoder
    // cancellation.
    void encodingStarted();
    // Emitted exactly once per begin(): the export state is already reset;
    // restoreIndex is the frame the user was viewing before the export.
    void finished(bool success, const QString& message, int restoreIndex);

private:
    void endExport(bool success, const QString& message);
    // finalizeEncoding waits for the (already-running) ffmpeg probe off the
    // GUI thread, then finalizeEncodingWithProbe does the PNG-only vs. MP4
    // decision and launches the encoders.
    void finalizeEncoding();
    void finalizeEncodingWithProbe();
    [[nodiscard]] static bool probeFfmpeg();

    FrameRenderer m_renderFrames;
    AdvanceFrame m_advanceFrame;

    bool m_active = false;
    bool m_canceled = false;
    bool m_framesDone = false;
    ExportOptions m_options;
    std::map<QString, ExportLayout> m_layouts;
    int m_nextFrame = 0;
    bool m_hasFfmpeg = false;
    // Launched off the GUI thread at begin() (probing ffmpeg can block up to
    // ~4 s); consumed at finalize, by which point rendering has usually
    // already let it finish.
    QFuture<bool> m_ffmpegProbe;
    qreal m_scale = 1.0;   // frozen export zoom so every frame matches
    int m_totalFrames = 0;
    int m_restoreIndex = -1;
    int m_digitWidth = 5;
    QString m_directory;
    QString m_stem;
    std::vector<QString> m_panelSuffixes;
    QProgressDialog* m_progress = nullptr;
    // Shared with the encoder workers (captured by value, so it outlives
    // this object). Set by the progress Cancel and by cancelForShutdown.
    std::shared_ptr<std::atomic<bool>> m_encoderCancel;
    // Multi-panel encoder bookkeeping — plain members instead of the former
    // raw-new'd shared counters, so nothing leaks on early teardown.
    int m_encodersRemaining = 0;
    bool m_encodeFailed = false;
    QString m_encodeFailureLog;
};

} // namespace amrvis::qt
