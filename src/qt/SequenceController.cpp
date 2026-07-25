#include "SequenceController.hpp"

#include "CacheConfig.hpp"

#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent>

#include <utility>

namespace amrvis::qt {

namespace {

// Sequence frames get synthetic dataset ids well above interactive ones so a
// stale cross-dataset cache entry can never alias a frame's blocks.
constexpr std::uint64_t sequenceDatasetIdBase = 0x4000000000000000ULL;

} // namespace

SequenceController::SequenceController(Hooks hooks, QObject* parent)
    : QObject(parent)
    , m_hooks(std::move(hooks))
{
}

void SequenceController::open(std::vector<std::filesystem::path> frames)
{
    m_frames = std::move(frames);
    goToFrame(0);
}

void SequenceController::close()
{
    discardPrefetch();
    m_loadStopSource.request_stop();
    ++m_loadGeneration;
    m_frames.clear();
    m_index = -1;
    m_inFlight = false;
}

void SequenceController::step(int direction)
{
    if (m_frames.empty()) {
        return;
    }
    goToFrame(m_index + direction);
}

void SequenceController::goToFrame(int index, bool forceRestart)
{
    if (m_frames.empty()) {
        return;
    }
    const auto count = static_cast<int>(m_frames.size());
    // Both steps and playback wrap around the ends of the sequence.
    index = ((index % count) + count) % count;
    if (!forceRestart && m_inFlight && index == m_index) {
        return;
    }
    // The host cancels the previous frame's in-flight work and closes its
    // per-dataset windows (synchronously — direct connection).
    emit frameSwitchStarted(index);

    const auto generation = ++m_loadGeneration;
    m_index = index;
    m_inFlight = true;
    m_frameTimer.start();

    // A still-valid prefetch of this frame is consumed instead of loading
    // again; anything else in the slot is cancelled and dropped.
    if (m_prefetched && m_prefetched->frameIndex == index
        && m_prefetched->specGeneration == m_specGeneration) {
        auto prefetched = std::move(*m_prefetched);
        m_prefetched.reset();
        discardPrefetch();
        finishLoad(std::move(prefetched.result), prefetched.defaultPositions);
        return;
    }
    discardPrefetch();
    startLoad(index, generation);
}

void SequenceController::invalidatePrefetch()
{
    ++m_specGeneration;
    discardPrefetch();
}

void SequenceController::cancelActiveWork()
{
    m_loadStopSource.request_stop();
    m_prefetchStopSource.request_stop();
}

void SequenceController::startLoad(int index, std::uint64_t generation)
{
    auto spec = m_hooks.buildSpec();
    const auto defaultPositions = spec.defaultPositions;
    const auto path = m_frames[static_cast<std::size_t>(index)];
    const auto datasetId = DatasetId{
        sequenceDatasetIdBase + ++m_datasetCounter};
    m_loadStopSource = StopSource{};
    const auto cancellation = m_loadStopSource.get_token();
    emit loadActivityChanged(+1);
    emit statusMessage(tr("Loading frame %1...").arg(
        QString::fromStdString(path.filename().string())));

    auto* watcher = new QFutureWatcher<InitialSliceResult>(this);
    connect(watcher, &QFutureWatcher<InitialSliceResult>::finished, this,
        [this, watcher, generation, index, defaultPositions] {
            emit loadActivityChanged(-1);
            if (m_hooks.isShuttingDown()) {
                watcher->deleteLater();
                return;
            }
            try {
                auto result = watcher->result();
                if (generation == m_loadGeneration && index == m_index) {
                    finishLoad(std::move(result), defaultPositions);
                } else {
                    emit staleResultDropped();
                }
            } catch (const std::exception& error) {
                if (generation == m_loadGeneration && index == m_index) {
                    m_inFlight = false;
                    emit frameLoadFailed(
                        QString::fromUtf8(error.what()));
                } else {
                    emit staleResultDropped();
                }
            }
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [path, datasetId, spec = std::move(spec), cancellation] {
        return executeFrameLoad(path, datasetId, spec, initialCacheBudget(),
            cancellation);
    }));
}

void SequenceController::finishLoad(
    InitialSliceResult result, bool defaultPositions)
{
    try {
        m_hooks.displayFrame(result, defaultPositions);
    } catch (const std::exception& error) {
        m_inFlight = false;
        emit frameLoadFailed(QString::fromUtf8(error.what()));
        return;
    }
    m_inFlight = false;
    m_lastFrameSwitchMs = m_frameTimer.elapsed();
    emit frameDisplayed(m_index);
    // Bounded low-priority prefetch of the next frame: queued behind the
    // display update, and re-validated when it runs so a frame jump in the
    // meantime does not start obsolete I/O.
    const auto displayedIndex = m_index;
    QTimer::singleShot(0, this, [this, displayedIndex] {
        if (m_frames.empty() || m_inFlight || m_index != displayedIndex) {
            return;
        }
        const auto count = static_cast<int>(m_frames.size());
        startPrefetch((displayedIndex + 1) % count);
    });
}

void SequenceController::startPrefetch(int frameIndex)
{
    // Single bounded slot: cancel and drop whatever prefetch came before.
    discardPrefetch();
    auto spec = m_hooks.buildSpec();
    const auto defaultPositions = spec.defaultPositions;
    const auto specGeneration = m_specGeneration;
    const auto generation = m_prefetchGeneration;
    const auto path = m_frames[static_cast<std::size_t>(frameIndex)];
    const auto datasetId = DatasetId{
        sequenceDatasetIdBase + ++m_datasetCounter};
    m_prefetchStopSource = StopSource{};
    const auto cancellation = m_prefetchStopSource.get_token();
    emit loadActivityChanged(+1);

    auto* watcher = new QFutureWatcher<InitialSliceResult>(this);
    connect(watcher, &QFutureWatcher<InitialSliceResult>::finished, this,
        [this, watcher, generation, frameIndex, specGeneration,
            defaultPositions] {
            emit loadActivityChanged(-1);
            try {
                auto result = watcher->result();
                if (generation == m_prefetchGeneration && !m_frames.empty()) {
                    m_prefetched = PrefetchedFrame{frameIndex, specGeneration,
                        defaultPositions, std::move(result)};
                } else {
                    emit staleResultDropped();
                }
            } catch (const std::exception&) {
                // Prefetch failures stay silent: reaching the frame loads it
                // through the normal path and reports any error then. A
                // superseded failure still counts as a stale drop.
                if (generation != m_prefetchGeneration) {
                    emit staleResultDropped();
                }
            }
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [path, datasetId, spec = std::move(spec), cancellation] {
        return executeFrameLoad(path, datasetId, spec, initialCacheBudget(),
            cancellation);
    }));
}

void SequenceController::discardPrefetch()
{
    m_prefetchStopSource.request_stop();
    ++m_prefetchGeneration;
    m_prefetched.reset();
}

} // namespace amrvis::qt
