#include "MainWindowInternal.hpp"
#include "NavigationGeometry.hpp"

#include <QKeySequence>
#include <QScopedValueRollback>

namespace amrvis::qt {
namespace {
bool sameWindow(const QRectF& a, const QRectF& b)
{
    const auto near = [](double x, double y, double span) {
        return std::abs(x - y) <= 1.e-8 * std::max(std::abs(span), 1.e-100);
    };
    return near(a.x(), b.x(), a.width()) && near(a.y(), b.y(), a.height())
        && near(a.width(), b.width(), a.width())
        && near(a.height(), b.height(), a.height());
}
QRectF mapWindow(const QRectF& window, const QRectF& from, const QRectF& to)
{
    if (from.isEmpty()) return to;
    const double sx = to.width() / from.width();
    const double sy = to.height() / from.height();
    return {to.x() + (window.x() - from.x()) * sx,
        to.y() + (window.y() - from.y()) * sy,
        window.width() * sx, window.height() * sy};
}
}

bool MainWindow::NavigationPanel::operator==(const NavigationPanel& other) const
{
    return state == other.state && region == other.region && mode == other.mode
        && factor == other.factor && virtualCanvas == other.virtualCanvas
        && pairWindow == other.pairWindow && sameWindow(window, other.window);
}

MainWindow::NavigationScope::NavigationScope(MainWindow& window, bool joinGesture)
    : m_window(window), m_owner(false)
{
    if (!joinGesture && window.m_navigationBefore
        && window.m_navigationKind != ImageView::NavigationKind::Action) {
        window.finishNavigation();
    }
    m_owner = !window.m_navigationBefore && !window.m_restoringNavigation;
    if (m_owner) window.beginNavigation(ImageView::NavigationKind::Action, nullptr);
}
MainWindow::NavigationScope::~NavigationScope()
{
    if (m_owner) m_window.finishNavigation();
}

void MainWindow::setupNavigation()
{
    m_navigationTimer = new QTimer(this);
    m_navigationTimer->setSingleShot(true);
    m_navigationTimer->setInterval(300);
    connect(m_navigationTimer, &QTimer::timeout, this, &MainWindow::finishNavigation);
    // Back and Forward lead the toolbar, as in browsers and file managers.
    auto* const first = m_sliceToolbar->actions().value(0);
    const auto add = [this, first](const QString& text, bool forward) {
        auto* action = new QAction(text, this);
        action->setShortcut(QKeySequence(forward ? QKeySequence::Forward : QKeySequence::Back));
        action->setObjectName(forward ? QStringLiteral("navigationForwardAction")
                                     : QStringLiteral("navigationBackAction"));
        connect(action, &QAction::triggered, this, [this, forward] { navigate(forward); });
        m_sliceToolbar->insertAction(first, action);
        return action;
    };
    m_navigationBack = add(tr("Back"), false);
    m_navigationForward = add(tr("Forward"), true);
    m_sliceToolbar->insertSeparator(first);
    for (auto* state : allViewStates()) {
        if (state->view && state->layer == 0) connectNavigation(state->view);
    }
    m_fixedCrosshairAction = new QAction(tr("Keep crosshair fixed while panning"), this);
    m_fixedCrosshairAction->setObjectName(QStringLiteral("fixedCrosshairAction"));
    m_fixedCrosshairAction->setCheckable(true);
    connect(m_fixedCrosshairAction, &QAction::toggled, this, [this] { refreshScanAction(); });
    m_fixedCrosshairAction->setIconText(tr("Fixed crosshair"));
    m_sliceToolbar->addSeparator();
    m_sliceToolbar->addAction(m_fixedCrosshairAction);
    refreshNavigationActions();
    refreshScanAction();
}

void MainWindow::connectNavigation(ImageView* view)
{
    connect(view, &ImageView::navigationBegan, this,
        [this, view](ImageView::NavigationKind kind) { beginNavigation(kind, view); });
    connect(view, &ImageView::navigationEnded, this, [this, view](bool wheelBurst) {
        if (m_navigationView != view) return;
        if (wheelBurst && m_navigationKind == ImageView::NavigationKind::Wheel) {
            m_navigationTimer->start();
            refreshNavigationActions();
        } else {
            finishNavigation();
        }
    });
}

QTransform MainWindow::navigationTransform(const PlaneViewState& state) const
{
    // Mapped and paired canvases already use stable display units.
    if (m_pair || isWarped(state.warp)) return {};
    const auto axes = displayAxes(state.normal);
    const auto x = static_cast<std::size_t>(axes[0]);
    const auto y = static_cast<std::size_t>(axes[1]);
    auto region = state.pixmapRegion;
    QRectF scene = state.view->imageSceneRect();
    if (state.view->virtualCanvasActive()) {
        region = datasetSampleBounds(layerFor(state).session->metadata());
        scene = state.view->sceneRect();
    }
    if (scene.isEmpty()) return {};
    const double sx = (region.upper[x] - region.lower[x]) / scene.width();
    const double sy = (region.upper[y] - region.lower[y]) / scene.height();
    return QTransform(sx, 0, 0, sy,
        region.lower[x] - scene.x() * sx, -region.upper[y] - scene.y() * sy);
}

MainWindow::NavigationPanel MainWindow::captureNavigationPanel(PlaneViewState& state) const
{
    NavigationPanel panel;
    panel.state = &state;
    panel.region = state.visibleRegion;
    panel.sceneWindow = state.view->mapToScene(state.view->viewport()->rect()).boundingRect();
    panel.window = navigationTransform(state).mapRect(panel.sceneWindow);
    if (const auto found = m_navigationPending.find(&state); found != m_navigationPending.end()) {
        panel.window = mapWindow(panel.sceneWindow, found->second.sceneWindow, found->second.window);
    }
    panel.mode = state.view->transformMode();
    panel.factor = state.view->fixedScaleFactor();
    panel.virtualCanvas = state.view->virtualCanvasActive();
    if (m_pair) panel.pairWindow = m_pairWindows[static_cast<std::size_t>(state.normal)];
    return panel;
}

MainWindow::NavigationSnapshot MainWindow::captureNavigation()
{
    NavigationSnapshot snapshot;
    for (auto* state : currentViews()) {
        if (state->view && state->view->hasImage() && layerFor(*state).session) {
            snapshot.panels.push_back(captureNavigationPanel(*state));
        }
    }
    snapshot.slicePositions = m_slicePosition3d;
    return snapshot;
}

void MainWindow::beginNavigation(ImageView::NavigationKind kind, ImageView* view)
{
    if (m_restoringNavigation || !primary().session) return;
    if (m_navigationBefore && (kind != m_navigationKind || view != m_navigationView)) {
        finishNavigation();
    }
    if (!m_navigationBefore) {
        m_navigationBefore = captureNavigation();
        m_navigationKind = kind;
        m_navigationView = view;
    }
    m_navigationTimer->stop();
}

void MainWindow::finishNavigation()
{
    if (!m_navigationBefore || m_restoringNavigation) return;
    m_navigationTimer->stop();
    auto before = std::move(*m_navigationBefore);
    m_navigationBefore.reset();
    auto after = captureNavigation();
    m_navigationView = nullptr;
    if (before.panels.size() != after.panels.size()) {
        clearNavigation();
        return;
    }
    if (before.slicePositions == after.slicePositions) {
        before.slicePositions.reset();
        after.slicePositions.reset();
    }
    for (std::size_t i = before.panels.size(); i-- > 0;) {
        if (before.panels[i] == after.panels[i]) {
            before.panels.erase(before.panels.begin() + static_cast<std::ptrdiff_t>(i));
            after.panels.erase(after.panels.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            // Only a slice this gesture asked for may re-frame the view; a
            // view-only change (a local wheel zoom) would be replayed later
            // by an unrelated one, such as a resize's.
            auto& panel = after.panels[i];
            if (sliceExpected(*panel.state)) {
                m_navigationPending[panel.state] = panel;
            } else {
                m_navigationPending.erase(panel.state);
            }
        }
    }
    m_navigationHistory.push(std::move(before), std::move(after));
    refreshNavigationActions();
}

bool MainWindow::sliceExpected(const PlaneViewState& state) const
{
    if (state.pendingRequests > 0) return true;
    return m_sliceDebounce->isActive() && (m_pendingAllViews
        || std::find(m_pendingViews.begin(), m_pendingViews.end(), &state) != m_pendingViews.end());
}

void MainWindow::refreshNavigationActions()
{
    if (!m_navigationBack) return;
    m_navigationBack->setEnabled(m_navigationHistory.canBack() || m_navigationBefore.has_value());
    m_navigationForward->setEnabled(m_navigationHistory.canForward());
}

void MainWindow::clearNavigation()
{
    if (m_restoringNavigation) return;
    // History only: a drag in progress (e.g. across a playback frame) goes on.
    if (m_navigationTimer) m_navigationTimer->stop();
    m_navigationBefore.reset();
    m_navigationPending.clear();
    m_navigationHistory.clear();
    m_navigationView = nullptr;
    refreshNavigationActions();
}

void MainWindow::applyNavigationPanel(const NavigationPanel& panel)
{
    auto& state = *panel.state;
    auto* view = state.view;
    if (panel.virtualCanvas) {
        view->setVirtualCanvas(virtualPlacementFor(state, state.plane->physicalRegion));
    } else if (!m_pair && !isWarped(state.warp)) {
        view->setVirtualCanvas(std::nullopt);
    }
    const auto scene = navigationTransform(state).inverted().mapRect(panel.window);
    QRectF canvas = view->sceneRect();
    if (m_pair) {
        canvas = toQRectF(pairCanvasRect(state.normal));
    } else if (!isWarped(state.warp) && !panel.virtualCanvas) {
        const auto region = panel.region.value_or(
            datasetSampleBounds(layerFor(state).session->metadata()));
        const auto axes = displayAxes(state.normal);
        const auto x = static_cast<std::size_t>(axes[0]);
        const auto y = static_cast<std::size_t>(axes[1]);
        const QRectF footprint(region.lower[x], -region.upper[y],
            region.upper[x] - region.lower[x], region.upper[y] - region.lower[y]);
        // Confine the feedback to the requested raster's footprint now. A
        // transient full-domain canvas would raise scroll bars and size the
        // incoming raster/transform for a viewport about to grow again.
        canvas = navigationTransform(state).inverted().mapRect(footprint);
    }
    view->restoreNavigation(panel.mode, panel.factor, scene, canvas);
}

void MainWindow::reconcilePendingNavigation(PlaneViewState& state)
{
    // A gesture still open when the arrival lands (a wheel zoom right after
    // Back) supersedes the saved window: carry the current one instead.
    if (!m_navigationBefore || m_restoringNavigation) return;
    if (m_panView == &state && m_panDataRefresh) return;
    const auto found = m_navigationPending.find(&state);
    if (found == m_navigationPending.end()) return;
    // A pair or mapped canvas keeps its scene across the arrival: leave it be.
    if (m_pair || isWarped(state.warp)) {
        m_navigationPending.erase(found);
    } else {
        found->second = captureNavigationPanel(state);
    }
}

void MainWindow::restorePendingNavigation(PlaneViewState& state)
{
    const auto found = m_navigationPending.find(&state);
    if (found == m_navigationPending.end()) return;
    // Copy before invoking view setters; they can emit demand signals.
    const auto panel = found->second;
    const QScopedValueRollback<bool> restoring(m_restoringNavigation, true);
    applyNavigationPanel(panel);
    m_navigationPending.erase(&state);
}

void MainWindow::navigate(bool forward)
{
    if (m_panView) endPanDrag(*m_panView, m_panSceneDelta);
    finishNavigation();
    const auto* entry = forward ? m_navigationHistory.forward() : m_navigationHistory.back();
    if (!entry) return;
    const auto snapshot = forward ? entry->after : entry->before;
    const QScopedValueRollback<bool> restoring(m_restoringNavigation, true);
    // Cancel at intent time, before the debounce: no old arrival is eligible
    // to alter the navigation being restored in that interval.
    for (auto* state : currentViews()) {
        const bool moved = std::any_of(snapshot.panels.begin(), snapshot.panels.end(),
            [state](const auto& panel) { return panel.state == state; });
        const bool sliced = snapshot.slicePositions && (*snapshot.slicePositions)[
            static_cast<std::size_t>(state->normal)] != m_slicePosition3d[
                static_cast<std::size_t>(state->normal)];
        if (!moved && !sliced) continue;
        state->stopSource.request_stop();
        ++state->sliceGeneration;
        ++state->renderGeneration;
        state->view->cancelSelection();
    }
    if (snapshot.slicePositions) {
        setSlicePositions(*snapshot.slicePositions);
    }
    for (const auto& panel : snapshot.panels) {
        panel.state->visibleRegion = panel.region;
        if (m_pair) m_pairWindows[static_cast<std::size_t>(panel.state->normal)] = panel.pairWindow;
    }
    for (auto panel : snapshot.panels) {
        applyNavigationPanel(panel);
        panel.sceneWindow = panel.state->view->mapToScene(
            panel.state->view->viewport()->rect()).boundingRect();
        m_navigationPending[panel.state] = panel;
        scheduleSliceRequest(*panel.state);
    }
    updateCrosshairs();
    refreshScaleReport();
    refreshNavigationActions();
}

void MainWindow::shiftNavigationWindow(PlaneViewState& state,
    const RealBox& before, const RealBox& after)
{
    if (m_pair || isWarped(state.warp) || state.view->virtualCanvasActive()) return;
    auto panel = captureNavigationPanel(state);
    const auto axes = displayAxes(state.normal);
    const auto x = static_cast<std::size_t>(axes[0]);
    const auto y = static_cast<std::size_t>(axes[1]);
    panel.window.translate(after.lower[x] - before.lower[x], before.lower[y] - after.lower[y]);
    panel.region = after;
    m_navigationPending[&state] = panel;
}

MainWindow::ScanScope::ScanScope(MainWindow& window, PlaneViewState& state)
    : m_window(window), m_state(state)
{
    if (window.m_temporaryScan || (window.m_fixedCrosshairAction
            && window.m_fixedCrosshairAction->isChecked())) {
        m_anchor = window.scanAnchor(state);
    }
}
MainWindow::ScanScope::~ScanScope()
{
    if (m_anchor) m_window.scanAtAnchor(m_state, *m_anchor);
}

std::optional<QPointF> MainWindow::scanAnchor(PlaneViewState& state)
{
    if (m_viewDimension != 3 || !m_slicePlanesAction || !m_slicePlanesAction->isChecked()
        || !state.view || !state.view->hasImage() || !layerFor(state).session
        || layerFor(state).session->metadata().coordinateSystem != 0) return std::nullopt;
    for (const auto* other : statesForPanel(state.normal)) {
        if (isWarped(other->warp)) return std::nullopt;
    }
    const auto axes = displayAxes(state.normal);
    const auto x = static_cast<std::size_t>(axes[0]);
    const auto y = static_cast<std::size_t>(axes[1]);
    QPointF point(m_slicePosition3d[x], -m_slicePosition3d[y]);
    if (const auto placed = tilePlacement(state); placed && placed->pair) {
        const auto layer = m_pair->layerAt(m_slicePosition3d[
            static_cast<std::size_t>(m_pair->perpendicularAxis)]);
        point = {placed->pair->sceneFromPhysical(layer, axes[0], m_slicePosition3d[x]),
            placed->pair->sceneFromPhysical(layer, axes[1], m_slicePosition3d[y])};
        // A crosshair in a gap between unequal companions has no samples to
        // scan from: clampScanPath would cancel every pan.
        const auto others = statesForPanel(state.normal);
        if (std::none_of(others.begin(), others.end(), [&](const auto* other) {
                return stateShown(*other)
                    && toQRectF(placed->pair->tileRect(other->layer)).contains(point);
            })) return std::nullopt;
    }
    if (!m_pair) {
        const auto& region = state.visibleRegion.value_or(state.plane->physicalRegion);
        if (m_slicePosition3d[x] < region.lower[x] || m_slicePosition3d[x] >= region.upper[x]
            || m_slicePosition3d[y] < region.lower[y] || m_slicePosition3d[y] >= region.upper[y])
            return std::nullopt;
    }
    const auto window = captureNavigationPanel(state).window;
    if (window.isEmpty() || !window.contains(point)) return std::nullopt;
    return QPointF((point.x() - window.x()) / window.width(),
        (point.y() - window.y()) / window.height());
}

void MainWindow::scanAtAnchor(PlaneViewState& state, const QPointF& anchor)
{
    const auto window = captureNavigationPanel(state).window;
    QPointF point(window.x() + anchor.x() * window.width(),
        window.y() + anchor.y() * window.height());
    const auto axes = displayAxes(state.normal);
    auto positions = m_slicePosition3d;
    positions[static_cast<std::size_t>(axes[0])] = point.x();
    positions[static_cast<std::size_t>(axes[1])] = -point.y();
    if (const auto placed = tilePlacement(state); placed && placed->pair) {
        auto layer = m_pair->layerAt(m_slicePosition3d[
            static_cast<std::size_t>(m_pair->perpendicularAxis)]);
        const QPointF previous(
            placed->pair->sceneFromPhysical(layer, axes[0], m_slicePosition3d[static_cast<std::size_t>(axes[0])]),
            placed->pair->sceneFromPhysical(layer, axes[1], m_slicePosition3d[static_cast<std::size_t>(axes[1])]));
        std::vector<QRectF> domains;
        for (auto* other : statesForPanel(state.normal)) {
            if (stateShown(*other)) domains.push_back(toQRectF(placed->pair->tileRect(other->layer)));
        }
        const auto clipped = clampScanPath(previous, point, domains);
        if (clipped != point) {
            const auto correction = clipped - point;
            const auto& framed = m_pairWindows[static_cast<std::size_t>(state.normal)];
            if (framed) {
                applyPairZoomWindow(state.normal, framed->translated(correction), false);
            } else {
                const auto scene = state.view->mapToScene(state.view->viewport()->rect()).boundingRect();
                state.view->centerOn(scene.center() + correction);
            }
            point = clipped;
        }
        // A scan across the interface uses the scale of the band entered.
        for (std::size_t candidate = 0; candidate < 2; ++candidate) {
            if (toQRectF(placed->pair->tileRect(candidate)).contains(point)) {
                layer = candidate;
                break;
            }
        }
        positions[static_cast<std::size_t>(axes[0])]
            = placed->pair->physicalFromScene(layer, axes[0], point.x());
        positions[static_cast<std::size_t>(axes[1])]
            = placed->pair->physicalFromScene(layer, axes[1], point.y());
    }
    const auto domain = m_pair ? m_pair->unionBounds
        : datasetSampleBounds(primary().session->metadata());
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto tolerance = 64 * std::numeric_limits<double>::epsilon()
            * std::max(std::abs(m_slicePosition3d[axis]),
                domain.upper[axis] - domain.lower[axis]);
        if (std::abs(positions[axis] - m_slicePosition3d[axis]) <= tolerance)
            positions[axis] = m_slicePosition3d[axis];
    }
    if (const auto found = m_navigationPending.find(&state); found != m_navigationPending.end()) {
        found->second.scan = true;
    }
    setSlicePositions(positions);
    updateCrosshairs(state);
}

void MainWindow::applyScanStep(PlaneViewState& state, const QPointF& direction)
{
    if (!scanAnchor(state)) return;
    const QScopedValueRollback<bool> scan(m_temporaryScan, true);
    applyPanStep(state, direction);
}

void MainWindow::refreshScanAction()
{
    if (m_fixedCrosshairAction) {
        // A checked mode stays enabled so it can always be turned off.
        m_fixedCrosshairAction->setEnabled(m_fixedCrosshairAction->isChecked()
            || (m_activeView && scanAnchor(*m_activeView).has_value()));
        m_fixedCrosshairAction->setToolTip(tr(
            "Pan under the crosshair and scan the other two slices. "
            "Requires visible Cartesian 3-D slice guides; Shift+arrow scans temporarily."));
    }
}

} // namespace amrvis::qt
