#include "MainWindowInternal.hpp"

#include <QKeySequence>
#include <QScopedValueRollback>
#include <QToolButton>

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
    const auto add = [this](const QString& text, bool forward) {
        auto* action = new QAction(text, this);
        action->setObjectName(forward ? QStringLiteral("navigationForwardAction")
                                     : QStringLiteral("navigationBackAction"));
        connect(action, &QAction::triggered, this, [this, forward] { navigate(forward); });
        auto* button = new QToolButton(m_sliceToolbar);
        button->setDefaultAction(action);
        QAction* before = nullptr;
        for (auto* item : m_sliceToolbar->actions()) {
            const auto* widget = m_sliceToolbar->widgetForAction(item);
            if (widget && widget->objectName() == QStringLiteral("lineOrientationButton")) {
                before = item;
                break;
            }
        }
        m_sliceToolbar->insertWidget(before, button);
        return action;
    };
    m_navigationBack = add(tr("Back"), false);
    m_navigationForward = add(tr("Forward"), true);
    for (auto* state : allViewStates()) {
        if (state->view && state->layer == 0) connectNavigation(state->view);
    }
    refreshNavigationActions();
}

void MainWindow::connectNavigation(ImageView* view)
{
    for (bool forward : {false, true}) {
        auto* shortcut = new QAction(view);
        shortcut->setShortcut(QKeySequence(forward ? QKeySequence::Forward : QKeySequence::Back));
        shortcut->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        view->addAction(shortcut);
        connect(shortcut, &QAction::triggered, this, [this, forward] { navigate(forward); });
    }
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
    for (std::size_t i = before.panels.size(); i-- > 0;) {
        if (before.panels[i] == after.panels[i]) {
            before.panels.erase(before.panels.begin() + static_cast<std::ptrdiff_t>(i));
            after.panels.erase(after.panels.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            auto& panel = after.panels[i];
            m_navigationPending[panel.state] = panel;
        }
    }
    m_navigationHistory.push(std::move(before), std::move(after));
    refreshNavigationActions();
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
    if (m_navigationTimer) m_navigationTimer->stop();
    if (m_panDebounce) m_panDebounce->stop();
    m_panView = nullptr;
    m_panDataRefresh = false;
    m_navigationBefore.reset();
    m_navigationPending.clear();
    m_navigationHistory.clear();
    m_navigationView = nullptr;
    for (auto* state : allViewStates()) {
        if (state->view) state->view->cancelSelection();
    }
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
        if (!moved) continue;
        state->stopSource.request_stop();
        ++state->sliceGeneration;
        ++state->renderGeneration;
        state->view->cancelSelection();
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

} // namespace amrvis::qt
