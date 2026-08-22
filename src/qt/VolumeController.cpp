#include "VolumeController.hpp"

#include "QtErrorText.hpp"
#include "VolumeWindow.hpp"

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
    // render. A throttle, not a debounce: scheduleRender leaves an already
    // running timer alone, so a continuous drag -- which emits faster than
    // this interval -- gets a draft every interval instead of rearming the
    // timer forever and rendering nothing until the mouse stops.
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
        m_action->setEnabled(dataset && dataset->supportsVolumeRendering());
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
    m_frameShown = false;
    // A camera move, a resize and a dragged opacity slider all arrive far
    // faster than a full render finishes, so they share one path: draft
    // frames while it continues, a full frame once it has been still for the
    // settle interval. A drag also ends explicitly, on the mouse release.
    const auto beginInteraction = [this] {
        m_interacting = true;
        m_settle->start();
        scheduleRender();
    };
    connect(window, &VolumeWindow::cameraChanged, this, beginInteraction);
    connect(window, &VolumeWindow::rampChanged, this, beginInteraction);
    connect(window, &VolumeWindow::viewResized, this, [this, beginInteraction] {
        // Until the first frame is up nothing is being stretched: these are
        // the window's own opening layout passes, and drafting them would
        // open on a half-size frame instead of the view-sized one.
        if (!m_frameShown) {
            scheduleRender();
            return;
        }
        beginInteraction();
    });
    connect(window, &VolumeWindow::interactionEnded, this, [this] {
        m_settle->stop();
        m_interacting = false;
        scheduleRender();
    });
    // The quality combo is a single discrete choice: render it in full.
    connect(window, &VolumeWindow::qualityChanged, this, [this] {
        m_settle->stop();
        m_interacting = false;
        scheduleRender();
    });
    connect(window, &QObject::destroyed, this, [this] {
        // A close cancels the render in flight; its result would go nowhere.
        ++m_generation;
        m_stopSource.request_stop();
        m_stopSource = StopSource{};
        m_rerun = false;
        m_debounce->stop();
        m_settle->stop();
        m_interacting = false;
        m_frameShown = false;
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
    // Before the new geometry goes in: a frame still in flight was rendered
    // for the outgoing dataset and must not be displayed against this one.
    cancel();
    pushGeometry();
    m_window->clearFrame();
    m_frameShown = false;
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
    // The settle timer too, and the interaction it stands for: left armed, it
    // fires after the cancellation and schedules a render of whatever dataset
    // is published by then -- the outgoing frame's, under the incoming one's
    // geometry.
    m_settle->stop();
    m_interacting = false;
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
    if (!m_debounce->isActive()) {
        m_debounce->start();
    }
}

void VolumeController::startRender()
{
    if (!m_window || m_inFlight) {
        return;
    }
    const auto dataset = m_hooks.dataset ? m_hooks.dataset() : nullptr;
    const auto field = m_hooks.field ? m_hooks.field() : std::nullopt;
    if (!dataset || !dataset->supportsVolumeRendering() || !field) {
        // Nothing to render after all: the label must not outlive the
        // intention to render that put it up.
        m_window->showRendering(false);
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
    // Mid-interaction (a moving camera, a resize, a dragged slider) gets a
    // half-size, single-sample draft; a settled view the full frame at the
    // view's size.
    const int width = std::max(1, m_interacting ? viewSize.width() / 2 : viewSize.width());
    const int height = std::max(1, m_interacting ? viewSize.height() / 2 : viewSize.height());
    request.outputSize = {std::min(width, maxVolumeOutputDimension),
        std::min(height, maxVolumeOutputDimension)};
    // No request.logarithmic here: the choice built below carries it, and
    // the pipeline sets it from there before each attempt's range resolve.
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

    // The name the request was built from: read again at display time it
    // would label these pixels with whatever field the combo shows by then.
    const auto fieldName = field->second;
    auto* watcher = new QFutureWatcher<VolumeDisplayResult>(this);
    connect(watcher, &QFutureWatcher<VolumeDisplayResult>::finished, this,
        [this, watcher, generation, cancellation, fieldName] {
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
                    auto status = describe(result, fieldName);
                    m_lastFrame = std::move(result.frame);
                    m_window->showFrame(m_lastFrame, status);
                    if (m_lastFrame.cacheFallbackFromLevel >= 0) {
                        emit statusMessage(
                            tr("Volume: the finest level exceeded the cache; "
                               "showing levels 0 through %1 instead of 0 through %2.")
                                .arg(m_lastFrame.cacheFallbackToLevel)
                                .arg(m_lastFrame.cacheFallbackFromLevel),
                            5000);
                    }
                    m_frameShown = true;
                    emit frameDisplayed();
                } else {
                    emit staleResultDropped();
                }
            } catch (const std::exception& error) {
                if (current && !cancellation.stop_requested()) {
                    emit renderFailed(
                        tr("Cannot render the volume: %1").arg(exceptionMessage(error)));
                } else {
                    emit staleResultDropped();
                }
            }
            watcher->deleteLater();
            // A change that arrived while this ran is rendered now, and the
            // label stays up for it -- taking it down here would flicker it
            // off and on again on every coalesced rerun of a drag. Otherwise
            // it comes down whatever this result was: a superseded render has
            // still stopped, and gating that on the generation left the label
            // up forever after a cancel.
            if (m_rerun && m_window) {
                m_rerun = false;
                scheduleRender();
            } else if (m_window) {
                m_window->showRendering(false);
            }
        });
    // The choice, not a range resolved once here: under cache pressure the
    // pipeline lowers the level, and a Level range read at the level asked
    // for would colour and label pixels no part of that level produced. The
    // overload taking it re-resolves per attempt (VolumePipeline.hpp).
    const VolumeRangeChoice rangeChoice{rangeSelection.mode,
        rangeSelection.userRange, rangeSelection.logarithmic};
    watcher->setFuture(QtConcurrent::run(
        [dataset, request = std::move(request), rangeChoice,
            cancellation]() mutable {
            return executeVolumeRenderWithFallback(dataset, std::move(request),
                rangeChoice, cancellation);
        }));
}

QString VolumeController::describe(
    const VolumeDisplayResult& result, const QString& fieldName) const
{
    const auto& frame = result.frame;
    const auto& range = frame.usedRange;
    return tr("%1  level %2  range [%3, %4]%5\ngrid %6 x %7 x %8 (%9)  %10 ms")
        .arg(fieldName)
        .arg(result.request.maximumLevel)
        .arg(range.minimum)
        .arg(range.maximum)
        .arg(range.logarithmic ? tr(" log") : QString())
        .arg(frame.metrics.gridDims[0])
        .arg(frame.metrics.gridDims[1])
        .arg(frame.metrics.gridDims[2])
        .arg(frame.metrics.gridFromCache ? tr("cached") : tr("sampled"))
        // The metrics are microseconds (Volume.hpp says why); one decimal
        // place so a sub-millisecond render reads as fast rather than as 0.
        .arg(static_cast<double>(frame.metrics.renderMicroseconds
                 + frame.metrics.sampleMicroseconds)
                / 1000.0,
            0, 'f', 1);
}

} // namespace amrvis::qt
