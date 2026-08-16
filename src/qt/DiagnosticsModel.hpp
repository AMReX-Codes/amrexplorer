#pragma once

#include <amrexplorer/cache/CacheMetrics.hpp>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>

class QDockWidget;
class QPlainTextEdit;
class QWidget;

namespace amrvis::qt {

// The Diagnostics panel's model extracted from MainWindow: the counters and
// metrics the panel shows -- background request activity, superseded results
// dropped, the last read's metadata and payload metrics, the cache state --
// plus the probe readout history and the recent background errors, each with
// its bounded-history rule. It owns the dock and its text widget and renders
// itself on refresh(); the host supplies the lines only it knows (dataset
// generation, last frame switch, remote endpoint) through Hooks, and keeps
// its own reportBackgroundError as the place that also sets the status bar
// and suppresses reports during shutdown.
//
// The active-request count is what "no background work in flight" decisions
// read, so it must never underflow: an unbalanced decrement (a bug elsewhere)
// is clamped and warned about rather than left to wrap to 2^64.
class DiagnosticsModel final : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        std::function<std::uint64_t()> generation;
        std::function<qint64()> lastFrameSwitchMs;
        // Extra lines for the remote endpoint, empty when there is none.
        std::function<QString()> remoteLines;
    };

    DiagnosticsModel(Hooks hooks, QObject* parent = nullptr);

    // The dock (hidden by default) holding the read-only text; the host adds
    // it to its dock area and its View menu. Owned by `parent`.
    QDockWidget* createDock(QWidget* parent);

    // Background request bookkeeping: +1 when a worker starts, -1 when its
    // watcher fires. Reads say whether any background work is in flight.
    void adjustActivity(int delta);
    [[nodiscard]] std::uint64_t activeRequests() const noexcept
    {
        return m_activeRequests;
    }
    [[nodiscard]] std::uint64_t staleResults() const noexcept
    {
        return m_staleResults;
    }
    // A load or query result arrived after being superseded and was dropped.
    void noteStaleResult();

    void setCacheMetrics(const CacheMetrics& metrics);
    void setMetadataMetrics(std::uint64_t filesRead, std::uint64_t bytesRead);
    void setSliceMetrics(std::uint64_t blocksRead, std::uint64_t cacheHits,
        std::uint64_t payloadBytesRead);
    // A dataset teardown: the per-dataset metrics and the probe history go;
    // the request/stale counters and the error history stay.
    void resetDatasetMetrics();

    // The status-bar probe readouts the user clicked, newest last, capped.
    void appendProbeLine(const QString& line);

    // A background failure: logged, kept (newest 50), and the dock is shown
    // so it is seen. The host sets its status bar and guards shutdown.
    void reportBackgroundError(const QString& message);
    [[nodiscard]] int backgroundErrorCount() const noexcept
    {
        return static_cast<int>(m_backgroundErrors.size());
    }

    // Re-renders the panel from the current state and the hooks.
    void refresh();
    // The rendered text (what refresh() shows); for tests.
    [[nodiscard]] QString text() const;

private:
    Hooks m_hooks;
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
    QStringList m_probeLines;
    QStringList m_backgroundErrors;
    QPointer<QDockWidget> m_dock;
    QPointer<QPlainTextEdit> m_view;
};

} // namespace amrvis::qt
