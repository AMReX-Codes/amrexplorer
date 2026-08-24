#include "VolumeController.hpp"

#include "QtErrorText.hpp"
#include "VolumeWindow.hpp"

#include <amrexplorer/core/Metadata.hpp>

#include <QAction>
#include <QFutureWatcher>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace amrvis::qt {

std::array<int, 2> volumeOutputSize(
    QSize viewSize, qreal devicePixelRatio, bool draft) noexcept
{
    // The clamp is also what keeps an absurd ratio harmless: it can only ever
    // produce a size inside the range the ray caster accepts.
    const auto bound = [](double extent) {
        return std::clamp(static_cast<int>(std::lround(extent)), 1,
            maxVolumeOutputDimension);
    };
    if (draft) {
        // Integer halving, as this has always done -- the draft size is not
        // where the ratio belongs.
        return {bound(viewSize.width() / 2), bound(viewSize.height() / 2)};
    }
    return {bound(viewSize.width() * devicePixelRatio),
        bound(viewSize.height() * devicePixelRatio)};
}

RealBox volumeVisibleRegion(const RealBox& domain,
    const std::array<std::optional<RealBox>, 3>& viewRegions) noexcept
{
    auto region = domain;
    for (const auto& view : viewRegions) {
        const auto& visible = view ? *view : domain;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            region.lower[axis]
                = std::max(region.lower[axis], visible.lower[axis]);
            region.upper[axis]
                = std::min(region.upper[axis], visible.upper[axis]);
        }
    }
    // See the declaration: an axis the views do not share goes back to the
    // domain. Written as `!(upper > lower)` so a NaN edge takes this path too.
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!(region.upper[axis] > region.lower[axis])) {
            region.lower[axis] = domain.lower[axis];
            region.upper[axis] = domain.upper[axis];
        }
    }
    return region;
}

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
        if (!m_window) {
            return;
        }
        // Same size: this is the dock relaying itself out, which showFrame and
        // the "Rendering..." label can both cause, not the user resizing. The
        // frame in the view still matches the view, so rendering again would
        // be a loop fed by its own output.
        if (m_window->viewSize() == m_lastRenderViewSize) {
            return;
        }
        // Until the first frame is up nothing is being stretched: these are
        // the window's own opening layout passes, and drafting them would
        // open on a half-size frame instead of the view-sized one.
        if (!m_frameShown) {
            scheduleRender();
            return;
        }
        beginInteraction();
    });
    // The counterpart: a change that is already complete -- a drag's release,
    // the quality combo, the alpha checkbox -- renders in full at once.
    const auto endInteraction = [this] {
        m_settle->stop();
        m_interacting = false;
        scheduleRender();
    };
    connect(window, &VolumeWindow::interactionEnded, this, endInteraction);
    // A scale change is a discrete event, not a drag, so it takes the
    // immediate full-frame path rather than drafting: there is nothing more
    // coming that a draft would be a placeholder for. It deliberately does
    // not go through the viewResized handler, whose first act is to dismiss
    // an unchanged logical size as layout churn -- which is exactly what a
    // scale change looks like there.
    connect(window, &VolumeWindow::viewScaleChanged, this, endInteraction);
    connect(window, &VolumeWindow::qualityChanged, this, endInteraction);
    // Toggling the region limit moves the box in one step, in both
    // directions, so it renders at once rather than drafting.
    connect(window, &VolumeWindow::regionLimitChanged, this, endInteraction);
    connect(window, &VolumeWindow::samplingChanged, this, endInteraction);
    connect(window, &VolumeWindow::paletteAlphaChanged, this, endInteraction);
    // A close from the title bar: closeWindow drops this connection before
    // closing, so reaching here means the user closed the window and nothing
    // else has handled it.
    m_windowDestroyed = connect(window, &QObject::destroyed, this,
        [this] { forgetWindow(); });
    pushGeometry();
    window->show();
    // The other trigger for a scale change, and on Qt 6.4 -- the minimum this
    // builds against -- the only one, since QEvent::DevicePixelRatioChange
    // arrived in 6.6. Moving a window to another screen always emits this,
    // which is the case that matters; it fires for a same-scale move too,
    // costing one full frame on a rare, user-initiated action -- cheaper than
    // a frame left at the wrong resolution. The handle exists only once the
    // window is shown.
    if (auto* const handle = window->windowHandle()) {
        connect(handle, &QWindow::screenChanged, this, endInteraction);
    }
    window->raise();
    window->activateWindow();
    scheduleRender();
}

void VolumeController::closeWindow()
{
    if (m_window) {
        auto* window = m_window.data();
        m_window = nullptr;
        // Here, not in the window's destroyed handler: WA_DeleteOnClose only
        // defers the delete, so that handler runs a turn later. Until it does,
        // the render in flight still matches m_generation -- and a window
        // opened in between would be handed the closed window's frame, then
        // have its own scheduled render disarmed by the late handler.
        QObject::disconnect(m_windowDestroyed);
        forgetWindow();
        window->close();
    }
}

void VolumeController::forgetWindow()
{
    cancel();
    m_frameShown = false;
    m_lastRenderViewSize = QSize{};
    m_lastRenderRegion = RealBox{};
    // The frame belonged to the window that is going away: keeping it holds a
    // whole pixel buffer for the life of the process and leaves lastFrame()
    // describing a window that no longer exists.
    m_lastFrame = VolumeFrame{};
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
        // Whether this session can be asked how to sample: a local one always
        // can, a server speaking an older protocol cannot. Pushed here so it
        // follows the dataset, the way the palette's alpha ramp does.
        m_window->setSamplingSelectable(dataset->supportsVolumeSampling());
    }
    pushPalette();
    slicePositionsChanged();
    slicePlanesVisibilityChanged();
}

void VolumeController::pushPalette()
{
    if (m_window && m_hooks.palette) {
        const auto& palette = m_hooks.palette();
        m_window->setColorPalette(&palette);
        m_window->setPaletteHasAlpha(palette.hasAlphaRamp());
    }
}

void VolumeController::configureForDataset()
{
    refreshActionEnabled();
    if (!m_window) {
        // No window to reconfigure, but the frame still described the dataset
        // being replaced.
        m_lastFrame = VolumeFrame{};
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
    //
    // Playback abandons it without stopping the render throttle. Stopping it
    // here and starting it again below pushes the pending render out by a
    // full interval on every frame, so at frame intervals shorter than the
    // throttle's own -- the Speed slider goes down to 1 ms -- it never
    // elapses and nothing renders at all. Left armed it fires on schedule and
    // renders whichever frame is current by then, which is exactly what it
    // does for a continuous drag.
    const bool playing = m_hooks.sequencePlaying && m_hooks.sequencePlaying();
    if (playing) {
        abandonInFlight();
    } else {
        cancel();
    }
    pushGeometry();
    // Sequence playback lands here once per frame. Dropping the frame each
    // time left the window blank for the whole run -- a full ray cast never
    // finished before the next frame cancelled it -- so the frame that is up
    // stays up until the next draft replaces it. Nothing is shown against a
    // geometry it does not belong to: a push that moved the domain drops the
    // frame in IsoWidget::setGeometry, whatever this decides.
    if (playing) {
        scheduleRender();
        return;
    }
    m_window->clearFrame();
    // The frame just cleared belonged to the outgoing dataset; lastFrame()
    // would otherwise keep reporting it as this one's.
    m_lastFrame = VolumeFrame{};
    m_frameShown = false;
    m_lastRenderViewSize = QSize{};
    scheduleRender();
}

void VolumeController::frameSwitchStarted()
{
    // See the declaration: playback must not lose the pending render, because
    // this runs once per frame and re-arming the throttle on each one means it
    // never fires. The frame in flight goes either way -- it was built for the
    // frame being replaced.
    if (m_hooks.sequencePlaying && m_hooks.sequencePlaying()) {
        abandonInFlight();
    } else {
        cancel();
    }
}

void VolumeController::regionChanged()
{
    // Nothing to do unless the window is asking to be limited: with the toggle
    // off the region is the domain whatever the views do, and this is called
    // for every scheduled slice request -- a zoom, a pan, a slice move, a
    // field change all arrive here.
    if (!m_window || !m_window->limitToVisibleRegion()) {
        return;
    }
    const auto dataset = m_hooks.dataset ? m_hooks.dataset() : nullptr;
    if (!dataset) {
        return;
    }
    const auto region = m_hooks.visibleRegion
        ? m_hooks.visibleRegion()
        : datasetSampleBounds(dataset->metadata());
    if (region == m_lastRenderRegion) {
        return;
    }
    scheduleRender();
}

void VolumeController::refresh()
{
    if (!m_window) {
        return;
    }
    pushPalette();
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
    // closeWindow cancels and drops the frame through forgetWindow; cancelling
    // here as well would bump the generation twice for one teardown.
    closeWindow();
    m_lastFrame = VolumeFrame{};
    refreshActionEnabled();
}

void VolumeController::abandonInFlight()
{
    ++m_generation;
    m_stopSource.request_stop();
    m_stopSource = StopSource{};
    // The pending rerun was for the work being abandoned. A caller that still
    // wants one asks again.
    m_rerun = false;
}

void VolumeController::cancel()
{
    abandonInFlight();
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
    m_lastRenderViewSize = viewSize;

    // The request, less its range: that is resolved on the worker, since
    // File/Level statistics may be a round trip away on a remote session.
    VolumeRenderRequest request;
    request.dataset = dataset->id();
    request.field = field->first;
    request.maximumLevel = std::clamp(level.maximumLevel, 0, metadata.finestLevel);
    request.composition = level.composition;
    // The whole domain, or what the slice views show when the window asks to
    // be limited to it. Remembered so regionChanged can spot the next move.
    request.region = m_window->limitToVisibleRegion() && m_hooks.visibleRegion
        ? m_hooks.visibleRegion()
        : datasetSampleBounds(metadata);
    m_lastRenderRegion = request.region;
    request.camera = m_window->camera();
    // Mid-interaction (a moving camera, a resize, a dragged slider) gets a
    // half-size, single-sample draft; a settled view the full frame at the
    // view's size. Sequence playback drafts for the same reason: the next
    // frame arrives before a full ray cast could finish, so asking for one
    // means every frame is cancelled unseen.
    const bool draft = m_interacting
        || (m_hooks.sequencePlaying && m_hooks.sequencePlaying());
    request.outputSize = volumeOutputSize(
        viewSize, m_window->viewDevicePixelRatio(), draft);
    // No request.logarithmic here: the choice built below carries it, and
    // the pipeline sets it from there before each attempt's range resolve.
    request.transfer = makeVolumeTransferFunction(
        m_hooks.palette ? m_hooks.palette() : builtinPalette(BuiltinPalette::Rainbow),
        m_window->ramp());
    request.samplesPerVoxel = draft ? 1 : quality.samplesPerVoxel;
    // Drafts too: with fewer samples per voxel there is more distance between
    // them, which is exactly where reading one voxel per sample terraces.
    request.sampling = m_window->sampling();
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
                    m_window->showFrame(
                        m_lastFrame, result.request.camera, status);
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
                    const auto message = tr("Cannot render the volume: %1")
                        .arg(exceptionMessage(error));
                    // Into the window as well as out to the host: this window
                    // is raised over the main window, whose status bar and
                    // Diagnostics dock are where renderFailed lands, so on its
                    // own the view would simply stop changing.
                    m_window->showFailure(message);
                    emit renderFailed(message);
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
    //
    // The Visible mode is left to the renderer, which resolves it from the
    // grid it sampled. That is a different span than the slices and the Color
    // Scale show, and the guide says so. Making them agree wants the resolved
    // range published on RangeController::Selection, written where the colour
    // scale itself is written, so it arrives beside the mode it belongs to and
    // for the selection then in effect. Fetching it through a separate hook
    // was tried and withdrawn: it needs a staleness key rebuilt by hand here,
    // and that key cannot be made right -- after a cache fallback the level
    // combo reads "Level 0 only" while the plane came from a finest-available
    // request, so it never matches again.
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
    // The metrics are microseconds (Volume.hpp says why), shown in whichever
    // unit carries the digits that mean something. A draft is tens of
    // milliseconds and its tenth is the difference between instant and not; a
    // hundred milliseconds up, the tenth is noise on a line that changes every
    // frame; past a second, four digits of milliseconds is not a number anyone
    // reads. Something just short of a second has to round somewhere -- 999.6
    // is either "1000 ms" or "1.00 s" -- and it reads better as the second,
    // which is why the rounding happens before the unit is chosen below.
    const auto milliseconds = static_cast<double>(frame.metrics.renderMicroseconds
                                  + frame.metrics.sampleMicroseconds)
        / 1000.0;
    // Rounded before the threshold is tested, not after: 999.6 ms prints as
    // a whole number, and testing the unrounded value would let it print
    // "1000 ms" -- the second this switch exists to avoid claiming.
    const auto whole = milliseconds >= 100.0;
    const auto shown = whole ? std::round(milliseconds) : milliseconds;
    const auto elapsed = shown >= 1000.0
        ? tr("%1 s").arg(shown / 1000.0, 0, 'f', 2)
        : tr("%1 ms").arg(shown, 0, 'f', whole ? 0 : 1);
    return tr("%1  level %2  range [%3, %4]%5\ngrid %6 x %7 x %8 (%9)  %10")
        .arg(fieldName)
        .arg(result.request.maximumLevel)
        .arg(range.minimum)
        .arg(range.maximum)
        .arg(range.logarithmic ? tr(" log") : QString())
        .arg(frame.metrics.gridDims[0])
        .arg(frame.metrics.gridDims[1])
        .arg(frame.metrics.gridDims[2])
        .arg(frame.metrics.gridFromCache ? tr("cached") : tr("sampled"))
        .arg(elapsed);
}

} // namespace amrvis::qt
