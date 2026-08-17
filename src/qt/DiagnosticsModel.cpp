#include "DiagnosticsModel.hpp"

#include <QDockWidget>
#include <QPlainTextEdit>

#include <utility>

namespace amrvis::qt {

namespace {

constexpr int maximumProbeLines = 100;
constexpr int maximumErrors = 50;

} // namespace

DiagnosticsModel::DiagnosticsModel(Hooks hooks, QObject* parent)
    : QObject(parent)
    , m_hooks(std::move(hooks))
{
}

QDockWidget* DiagnosticsModel::createDock(QWidget* parent)
{
    auto* dock = new QDockWidget(tr("Diagnostics"), parent);
    auto* view = new QPlainTextEdit(dock);
    view->setReadOnly(true);
    dock->setWidget(view);
    dock->setVisible(false);
    m_dock = dock;
    m_view = view;
    return dock;
}

void DiagnosticsModel::adjustActivity(int delta)
{
    if (delta >= 0) {
        m_activeRequests += static_cast<std::uint64_t>(delta);
        return;
    }
    const auto decrement = static_cast<std::uint64_t>(-delta);
    if (decrement > m_activeRequests) {
        // A decrement without its increment is a bookkeeping bug elsewhere;
        // an unsigned wrap here would defeat every "nothing in flight" check
        // that reads the count, so clamp and say so.
        qWarning("diagnostics: active-request count would go negative "
                 "(%llu - %llu); clamped to zero",
            static_cast<unsigned long long>(m_activeRequests),
            static_cast<unsigned long long>(decrement));
        m_activeRequests = 0;
        return;
    }
    m_activeRequests -= decrement;
}

void DiagnosticsModel::noteStaleResult()
{
    ++m_staleResults;
}

void DiagnosticsModel::setCacheMetrics(const CacheMetrics& metrics)
{
    m_cacheBudgetBytes = metrics.budgetBytes;
    m_cacheResidentBytes = metrics.residentBytes;
    m_cachePinnedBytes = metrics.pinnedBytes;
    m_cacheEvictions = metrics.evictions;
}

void DiagnosticsModel::setMetadataMetrics(
    std::uint64_t filesRead, std::uint64_t bytesRead)
{
    m_lastFilesRead = filesRead;
    m_lastBytesRead = bytesRead;
}

void DiagnosticsModel::setSliceMetrics(std::uint64_t blocksRead,
    std::uint64_t cacheHits, std::uint64_t payloadBytesRead)
{
    m_lastBlocksRead = blocksRead;
    m_lastCacheHits = cacheHits;
    m_lastPayloadBytesRead = payloadBytesRead;
}

void DiagnosticsModel::resetDatasetMetrics()
{
    m_lastBlocksRead = 0;
    m_lastCacheHits = 0;
    m_lastPayloadBytesRead = 0;
    m_cacheBudgetBytes = 0;
    m_cacheResidentBytes = 0;
    m_cachePinnedBytes = 0;
    m_cacheEvictions = 0;
    m_probeLines.clear();
}

void DiagnosticsModel::appendProbeLine(const QString& line)
{
    m_probeLines.append(line);
    while (m_probeLines.size() > maximumProbeLines) {
        m_probeLines.removeFirst();
    }
}

void DiagnosticsModel::reportBackgroundError(const QString& message)
{
    qWarning("%s", message.toUtf8().constData());
    m_backgroundErrors.append(message);
    while (m_backgroundErrors.size() > maximumErrors) {
        m_backgroundErrors.removeFirst();
    }
    if (m_dock) {
        m_dock->setVisible(true);
    }
    refresh();
}

QString DiagnosticsModel::text() const
{
    auto text = tr("generation: %1\nactive background requests: %2\n"
                   "stale results discarded: %3\nmetadata files read: %4\n"
                   "metadata bytes read: %5\nblocks read: %6\ncache hits: %7\n"
                   "payload bytes read: %8\ncache budget bytes: %9\n"
                   "cache resident bytes: %10\ncache pinned bytes: %11\n"
                   "cache evictions: %12\nlast frame switch: %13 ms")
                    .arg(m_hooks.generation ? m_hooks.generation() : 0)
                    .arg(m_activeRequests)
                    .arg(m_staleResults)
                    .arg(m_lastFilesRead)
                    .arg(m_lastBytesRead)
                    .arg(m_lastBlocksRead)
                    .arg(m_lastCacheHits)
                    .arg(m_lastPayloadBytesRead)
                    .arg(m_cacheBudgetBytes)
                    .arg(m_cacheResidentBytes)
                    .arg(m_cachePinnedBytes)
                    .arg(m_cacheEvictions)
                    .arg(m_hooks.lastFrameSwitchMs ? m_hooks.lastFrameSwitchMs()
                                                   : 0);
    if (m_hooks.remoteLines) {
        text += m_hooks.remoteLines();
    }
    for (const auto& line : m_probeLines) {
        text += QLatin1Char('\n');
        text += line;
    }
    for (const auto& error : m_backgroundErrors) {
        text += QLatin1Char('\n');
        text += tr("background error: %1").arg(error);
    }
    return text;
}

void DiagnosticsModel::refresh()
{
    if (m_view) {
        m_view->setPlainText(text());
    }
}

} // namespace amrvis::qt
