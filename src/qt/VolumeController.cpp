#include "VolumeController.hpp"

#include "QtErrorText.hpp"
#include "VolumeWindow.hpp"

#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/core/Metadata.hpp>

#include <QAction>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <exception>
#include <utility>

namespace amrvis::qt {

VolumeController::VolumeController(Hooks hooks, QObject* parent)
    : QObject(parent)
    , m_hooks(std::move(hooks))
{
    // Several changes in one event-loop turn (a range mode and its user
    // range, a palette and its reversal, sliders dragged) collapse into one
    // render; a moving camera keeps rearming it, so the draft frames come
    // at most every debounce interval.
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(40);
    connect(m_debounce, &QTimer::timeout, this, [this] { startRender(); });
    // A wheel zoom has no "release": the camera counts as settled once it
    // has been still for this long, and the settled camera gets a full frame.
    m_settle = new QTimer(this);
    m_settle->setSingleShot(true);
    m_settle->setInterval(250);
    connect(m_settle, &QTimer::timeout, this, [this] {
        if (m_interacting) {
            m_interacting = false;
            scheduleRender();
        }
    });
}

VolumeController::~VolumeController()
{
    m_stopSource.request_stop();
    closeWindow();
}

QAction* VolumeController::createAction(QObject* parent)
{
    auto* action = new QAction(tr("&Volume Rendering..."), parent);
    action->setObjectName(QStringLiteral("volumeRenderingAction"));
    action->setToolTip(tr("Ray-cast the 3-D field in its own window"));
    m_action = action;
    refreshActionEnabled();
    connect(action, &QAction::triggered, this, [this] {
        showWindow(qobject_cast<QWidget*>(this->parent()));
    });
    return action;
}

void VolumeController::refreshActionEnabled()
{
    const auto dataset = m_hooks.dataset ? m_hooks.dataset() : nullptr;
    if (m_action) {
        m_action->setEnabled(
            dataset && dataset->supportsVolumeRendering() && !m_suspended);
    }
}

void VolumeController::showWindow(QWidget* parent)
{
    if (m_window) {
        m_window->raise();
        m_window->activateWindow();
        return;
    }
    const auto dataset = m_hooks.dataset ? m_hooks.dataset() : nullptr;
    if (!dataset || !dataset->supportsVolumeRendering()) {
        return;
    }
    // A top-level window (parented for placement only, so it stays above
    // the main window without being modal). Deleted on close; the pointer
    // is a QPointer so a close from the title bar clears it.
    auto* window = new VolumeWindow(parent);
    window->setWindowFlag(Qt::Window, true);
    m_window = window;
    connect(window, &VolumeWindow::cameraChanged, this, [this] {
        m_interacting = true;
        m_settle->start();
        scheduleRender();
    });
    connect(window, &VolumeWindow::interactionEnded, this, [this] {
        m_settle->stop();
        m_interacting = false;
        scheduleRender();
    });
    connect(window, &VolumeWindow::rampChanged, this, [this] { scheduleRender(); });
    connect(window, &VolumeWindow::qualityChanged, this, [this] { scheduleRender(); });
    connect(window, &QObject::destroyed, this, [this] {
        // A close cancels the render in flight; its result would go nowhere.
        ++m_generation;
        m_stopSource.request_stop();
        m_stopSource = StopSource{};
        m_rerun = false;
        m_debounce->stop();
        m_settle->stop();
        m_interacting = false;
    });
    pushGeometry();
    window->show();
    window->raise();
    window->activateWindow();
    scheduleRender();
}

void VolumeController::closeWindow()
{
    if (m_window) {
        auto* window = m_window.data();
        m_window = nullptr;
        window->close();
    }
}

bool VolumeController::windowOpen() const noexcept
{
    return !m_window.isNull();
}

void VolumeController::pushGeometry()
{
    if (!m_window) {
        return;
    }
    const auto dataset = m_hooks.dataset ? m_hooks.dataset() : nullptr;
    if (dataset) {
        m_window->setDatasetGeometry(dataset->metadata());
    }
    if (m_hooks.palette) {
        const auto& palette = m_hooks.palette();
        m_window->setColorPalette(&palette);
        m_window->setPaletteHasAlpha(palette.hasAlphaRamp());
    }
    slicePositionsChanged();
    slicePlanesVisibilityChanged();
}

void VolumeController::configureForDataset()
{
    refreshActionEnabled();
    if (!m_window) {
        return;
    }
    const auto dataset = m_hooks.dataset ? m_hooks.dataset() : nullptr;
    if (!dataset || !dataset->supportsVolumeRendering()) {
        // The new dataset cannot be volume-rendered: the window shows nothing
        // of it, so it closes as the dataset window does.
        closeWindow();
        return;
    }
    pushGeometry();
    m_window->clearFrame();
    scheduleRender();
}

void VolumeController::refresh()
{
    if (!m_window) {
        return;
    }
    if (m_hooks.palette) {
        const auto& palette = m_hooks.palette();
        m_window->setColorPalette(&palette);
        m_window->setPaletteHasAlpha(palette.hasAlphaRamp());
    }
    scheduleRender();
}

void VolumeController::slicePositionsChanged()
{
    if (m_window && m_hooks.slicePositions) {
        const auto positions = m_hooks.slicePositions();
        m_window->setSlicePositions(positions[0], positions[1], positions[2]);
    }
}

void VolumeController::slicePlanesVisibilityChanged()
{
    if (m_window && m_hooks.slicePlanesVisible) {
        m_window->setSlicePlanesVisible(m_hooks.slicePlanesVisible());
    }
}

void VolumeController::reset()
{
    cancel();
    closeWindow();
    m_lastFrame = VolumeFrame{};
    refreshActionEnabled();
}

void VolumeController::cancel()
{
    ++m_generation;
    m_stopSource.request_stop();
    m_stopSource = StopSource{};
    m_rerun = false;
    m_debounce->stop();
}

void VolumeController::scheduleRender()
{
    if (!m_window) {
        return;
    }
    if (m_inFlight) {
        m_rerun = true;
        return;
    }
    m_debounce->start();
}

void VolumeController::startRender()
{
    if (!m_window || m_inFlight) {
        return;
    }
    const auto dataset = m_hooks.dataset ? m_hooks.dataset() : nullptr;
    const auto field = m_hooks.field ? m_hooks.field() : std::nullopt;
    if (!dataset || !dataset->supportsVolumeRendering() || !field) {
        return;
    }
    const auto& metadata = dataset->metadata();
    const auto level = m_hooks.levelSelection
        ? m_hooks.levelSelection() : LevelSelection{};
    const auto rangeSelection = m_hooks.rangeSelection
        ? m_hooks.rangeSelection() : RangeController::Selection{};
    const auto quality = m_window->quality();
    const auto viewSize = m_window->viewSize();

    // The request, less its range: that is resolved on the worker, since
    // File/Level statistics may be a round trip away on a remote session.
    VolumeRenderRequest request;
    request.dataset = dataset->id();
    request.field = field->first;
    request.maximumLevel = std::clamp(level.maximumLevel, 0, metadata.finestLevel);
    request.composition = level.composition;
    request.region = datasetSampleBounds(metadata);
    request.camera = m_window->camera();
    // A moving camera gets a half-size, single-sample draft; the settled
    // camera the full frame at the view's size.
    const int width = std::max(1, m_interacting ? viewSize.width() / 2 : viewSize.width());
    const int height = std::max(1, m_interacting ? viewSize.height() / 2 : viewSize.height());
    request.outputSize = {std::min(width, maxVolumeOutputDimension),
        std::min(height, maxVolumeOutputDimension)};
    request.logarithmic = rangeSelection.logarithmic;
    request.transfer = makeVolumeTransferFunction(
        m_hooks.palette ? m_hooks.palette() : builtinPalette(BuiltinPalette::Rainbow),
        m_window->ramp());
    request.samplesPerVoxel = m_interacting ? 1 : quality.samplesPerVoxel;
    request.maximumVoxels = quality.maximumVoxels;

    const auto generation = ++m_generation;
    m_stopSource = StopSource{};
    const auto cancellation = m_stopSource.get_token();
    m_inFlight = true;
    m_rerun = false;
    m_window->showRendering(true);
    emit renderActivityChanged(1);

    auto* watcher = new QFutureWatcher<VolumeDisplayResult>(this);
    connect(watcher, &QFutureWatcher<VolumeDisplayResult>::finished, this,
        [this, watcher, generation, cancellation, dataset] {
            emit renderActivityChanged(-1);
            m_inFlight = false;
            if (m_hooks.isShuttingDown && m_hooks.isShuttingDown()) {
                watcher->deleteLater();
                return;
            }
            const bool current = generation == m_generation && m_window;
            try {
                auto result = watcher->future().takeResult();
                if (current) {
                    m_lastFrame = result.frame;
                    m_window->showFrame(m_lastFrame, describe(result));
                    if (result.frame.cacheFallbackFromLevel >= 0) {
                        emit statusMessage(
                            tr("Volume: the finest level exceeded the cache; "
                               "showing levels 0 through %1 instead of 0 through %2.")
                                .arg(result.frame.cacheFallbackToLevel)
                                .arg(result.frame.cacheFallbackFromLevel),
                            5000);
                    }
                    emit frameDisplayed();
                } else {
                    emit staleResultDropped();
                }
            } catch (const ReadCancelled&) {
                emit staleResultDropped();
            } catch (const std::exception& error) {
                if (current && !cancellation.stop_requested()) {
                    emit renderFailed(
                        tr("Cannot render the volume: %1").arg(exceptionMessage(error)));
                } else {
                    emit staleResultDropped();
                }
            }
            if (m_window && generation == m_generation) {
                m_window->showRendering(false);
            }
            watcher->deleteLater();
            // A change that arrived while this ran: render it now.
            if (m_rerun && m_window) {
                m_rerun = false;
                m_debounce->start();
            }
        });
    watcher->setFuture(QtConcurrent::run(
        [dataset, request, rangeSelection, level, cancellation]() mutable {
            request.range = resolveVolumeRange(dataset, request.field,
                request.maximumLevel, request.composition, rangeSelection.mode,
                rangeSelection.userRange, rangeSelection.logarithmic, cancellation);
            return executeVolumeRenderWithFallback(dataset, std::move(request),
                cancellation);
        }));
}

QString VolumeController::describe(const VolumeDisplayResult& result) const
{
    const auto field = m_hooks.field ? m_hooks.field() : std::nullopt;
    const auto& frame = result.frame;
    const auto& range = frame.usedRange;
    return tr("%1  level %2  range [%3, %4]%5\ngrid %6 x %7 x %8 (%9)  %10 ms")
        .arg(field ? field->second : QString())
        .arg(result.request.maximumLevel)
        .arg(range.minimum)
        .arg(range.maximum)
        .arg(range.logarithmic ? tr(" log") : QString())
        .arg(frame.metrics.gridDims[0])
        .arg(frame.metrics.gridDims[1])
        .arg(frame.metrics.gridDims[2])
        .arg(frame.metrics.gridFromCache ? tr("cached") : tr("sampled"))
        .arg(frame.metrics.renderMilliseconds + frame.metrics.sampleMilliseconds);
}

} // namespace amrvis::qt
