#include "MainWindowInternal.hpp"

namespace amrvis::qt {

void MainWindow::requestVisibleSyncForTest()
{
    syncVisibleRanges();
}

void MainWindow::armVisibleSyncGateForTest()
{
    visible_sync_test::releaseGrants.store(0);
    visible_sync_test::passed.store(0);
    visible_sync_test::waiting.store(0);
    visible_sync_test::failNext.store(false);  // no throw carries over
    visible_sync_test::gateArmed.store(true);
}

void MainWindow::releaseVisibleSyncGateForTest()
{
    // Grant exactly one parked worker leave to proceed.
    visible_sync_test::releaseGrants.fetch_add(1);
}

void MainWindow::disarmVisibleSyncGateForTest()
{
    // Free everything (used on test exit so no worker stays parked).
    visible_sync_test::gateArmed.store(false);
    visible_sync_test::failNext.store(false);
}

void MainWindow::failNextVisibleSyncForTest()
{
    visible_sync_test::failNext.store(true);
}

bool MainWindow::visibleSyncWorkerWaitingForTest() const
{
    return visible_sync_test::waiting.load() > 0;
}

void MainWindow::adjustActiveRequestsForTest(int delta)
{
    m_diagnosticsModel->adjustActivity(delta);
}

std::uint64_t MainWindow::activeViewRenderGenerationForTest() const
{
    return m_activeView == nullptr ? 0 : m_activeView->renderGeneration;
}

std::uint64_t MainWindow::visibleSyncStaleSkipsForTest() const noexcept
{
    return m_visibleSyncStaleSkips;
}

void MainWindow::configureContourSyncForTest(
    int count, bool logarithmic, std::array<double, 3> slicePositions)
{
    if (!m_dataset) {
        return;
    }
    m_slicePosition3d = slicePositions;
    // Set range/log through the controller (requestSlice reads it) without
    // signals, so only the single scheduleSliceRequest below re-slices.
    m_range->setSelection({RangeMode::Visible, std::nullopt, logarithmic});
    m_displayMode = DisplayMode::RasterContours;
    m_contourCount = count;
    scheduleSliceRequest(false);
}

std::vector<MainWindow::ContourViewProbe>
MainWindow::contourViewProbesForTest()
{
    std::vector<ContourViewProbe> probes;
    for (const auto* state : currentViews()) {
        ContourViewProbe probe;
        probe.displayMinimum = state->displayMinimum;
        probe.displayMaximum = state->displayMaximum;
        probe.logarithmic = state->displayLogarithmic;
        for (const auto& polyline : state->contourPolylines) {
            probe.contourLevels.push_back(polyline.value);
        }
        std::sort(probe.contourLevels.begin(), probe.contourLevels.end());
        probe.contourLevels.erase(
            std::unique(probe.contourLevels.begin(), probe.contourLevels.end()),
            probe.contourLevels.end());
        probes.push_back(std::move(probe));
    }
    return probes;
}

void MainWindow::enableVisibleRasterForTest()
{
    if (!m_dataset) {
        return;
    }
    m_range->setSelection(
        {RangeMode::Visible, std::nullopt, m_range->logarithmic()});
    m_displayMode = DisplayMode::Raster;
    scheduleSliceRequest(false);
}

void MainWindow::zoomActiveViewForTest()
{
    if (!m_dataset || m_activeView == nullptr) {
        return;
    }
    const auto bounds = datasetSampleBounds(m_dataset->metadata());
    auto subregion = bounds;
    const auto axes = displayAxes(m_activeView->normal);
    for (std::size_t k = 0; k < 2; ++k) {
        const auto axis = static_cast<std::size_t>(axes[k]);
        subregion.lower[axis] = 0.5 * (bounds.lower[axis] + bounds.upper[axis]);
        subregion.upper[axis] = bounds.upper[axis];
    }
    m_activeView->visibleRegion = subregion;
    scheduleSliceRequest(*m_activeView, true);
}

bool MainWindow::animationDockVisibleForTest() const
{
    return m_animationDock != nullptr && m_animationDock->isVisible();
}

void MainWindow::setAnimationDockVisibleForTest(bool visible)
{
    if (m_animationDock != nullptr) {
        m_animationDock->setVisible(visible);
    }
}

QStringList MainWindow::paletteMenuLabelsForTest() const
{
    return m_paletteController->menuLabels();
}

QStringList MainWindow::paletteSelectorLabelsForTest() const
{
    return m_paletteController->selectorLabels();
}

QString MainWindow::viewPlaceholderForTest()
{
    QString shared;
    for (const auto* state : allViewStates()) {
        if (state->view->hasImage()) {
            return {};
        }
        const auto& text = state->view->placeholderText();
        if (shared.isEmpty()) {
            shared = text;
        } else if (shared != text) {
            return {};
        }
    }
    return shared;
}

bool MainWindow::activeViewRasterMatchesDisplayRangeForTest()
{
    if (m_activeView == nullptr) {
        return false;
    }
    const auto& state = *m_activeView;
    if (state.plane->width <= 0 || state.plane->height <= 0
        || !state.view->hasImage()) {
        return false;
    }
    const auto reference = renderScalarPlane(*state.plane, ScalarRenderSettings{
        .minimum = state.displayMinimum,
        .maximum = state.displayMaximum,
        .logarithmic = state.displayLogarithmic,
        .palette = &m_paletteController->palette()
    });
    if (!reference.valid()) {
        return false;
    }
    // Same buffer->view transform showSlice uses, so this stays in lockstep
    // with however the raster is actually displayed.
    return displayImageFor(reference) == state.view->image();
}

bool MainWindow::activeViewUsesViewportBoundedOutputForTest() const
{
    if (!std::dynamic_pointer_cast<remote::RemoteDatasetSession>(m_dataset)
        || m_activeView == nullptr || m_activeView->plane->width <= 0
        || m_activeView->plane->height <= 0) {
        return false;
    }
    const auto expected = sliceOutputSize(*m_activeView, true);
    return m_activeView->plane->width == expected[0]
        && m_activeView->plane->height == expected[1];
}

bool MainWindow::activeViewUsesNativeOutputForTest() const
{
    if (m_activeView == nullptr || m_activeView->plane->width <= 0
        || m_activeView->plane->height <= 0) {
        return false;
    }
    const auto expected = nativeOutputSize(*m_activeView);
    return m_activeView->plane->width == expected[0]
        && m_activeView->plane->height == expected[1];
}

bool MainWindow::allViewsUseViewportBoundedOutputForTest() const
{
    if (!std::dynamic_pointer_cast<remote::RemoteDatasetSession>(m_dataset)) {
        return false;
    }
    const std::array<const PlaneViewState*, 3> threeDimensional{
        &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
    const auto check = [&](const PlaneViewState& state) {
        if (state.plane->width <= 1 || state.plane->height <= 1) {
            return false;
        }
        const auto expected = sliceOutputSize(state, true);
        return state.plane->width == expected[0]
            && state.plane->height == expected[1];
    };
    if (m_viewDimension == 2) {
        return check(m_view2d);
    }
    return m_viewDimension == 3
        && std::all_of(threeDimensional.begin(), threeDimensional.end(),
            [&](const auto* state) { return check(*state); });
}

int MainWindow::slicesInFlightForTest() const
{
    return slicesInFlight();
}

bool MainWindow::sliceRequestPendingForTest() const
{
    // The debounce timer is normally active while a request is queued, but some
    // paths stop it without clearing the queue (see openDataset), so check the
    // pending views too -- the header promises "queued behind the debounce".
    return (m_sliceDebounce != nullptr && m_sliceDebounce->isActive())
        || m_pendingAllViews || !m_pendingViews.empty();
}

bool MainWindow::allViewsFixedScaleRasterCoversViewportForTest() const
{
    const auto check = [](const PlaneViewState& state) {
        const auto* view = state.view;
        if (view == nullptr || !view->hasImage()
            || view->transformMode() != ImageView::TransformMode::FixedScale
            || view->viewport() == nullptr) {
            return false;
        }
        const auto raster = QRectF(view->mapFromScene(
            view->imageSceneRect()).boundingRect());
        const auto domain = QRectF(view->mapFromScene(
            view->sceneRect()).boundingRect());
        // Everything the viewport shows of the domain must be backed by the
        // raster; one pixel of slack absorbs the integer mapping round-off.
        const auto shown = QRectF(view->viewport()->rect())
            .intersected(domain).adjusted(1.0, 1.0, -1.0, -1.0);
        return shown.isEmpty() || raster.contains(shown);
    };
    if (m_viewDimension == 2) {
        return check(m_view2d);
    }
    const std::array<const PlaneViewState*, 3> threeDimensional{
        &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
    return m_viewDimension == 3
        && std::all_of(threeDimensional.begin(), threeDimensional.end(),
            [&](const auto* state) { return check(*state); });
}

bool MainWindow::activeViewHasPhysicalAspectForTest(
    double expectedAspect) const
{
    if (!m_dataset || m_activeView == nullptr
        || m_activeView->plane->width <= 0
        || m_activeView->plane->height <= 0 || !(expectedAspect > 0.0)) {
        return false;
    }
    const auto actualAspect = static_cast<double>(m_activeView->plane->width)
        / m_activeView->plane->height;
    return std::abs(actualAspect - expectedAspect)
        <= 0.02 * expectedAspect;
}

bool MainWindow::fabStateClearedForTest() const
{
    return m_fabNavigator->cleared()
        && !windowTitle().endsWith(QStringLiteral(" FAB"));
}

void MainWindow::setGridBoxesVisibleForTest(bool visible)
{
    m_boxesAction->setChecked(visible);
}

std::size_t MainWindow::activeViewGridBoxCountForTest() const
{
    return m_activeView == nullptr
        ? 0 : m_activeView->view->gridBoxCount();
}

void MainWindow::rubberBandZoomActiveViewForTest()
{
    if (m_activeView == nullptr || m_activeView->plane->width <= 0
        || m_activeView->plane->height <= 0) {
        return;
    }
    const auto width = static_cast<double>(m_activeView->plane->width);
    const auto height = static_cast<double>(m_activeView->plane->height);
    rubberBandZoom(*m_activeView,
        QRectF(0.25 * width, 0.25 * height, 0.5 * width, 0.5 * height));
}

void MainWindow::rubberBandZoomRectangularActiveViewForTest()
{
    if (m_activeView == nullptr || m_activeView->plane->width <= 0
        || m_activeView->plane->height <= 0) {
        return;
    }
    const auto width = static_cast<double>(m_activeView->plane->width);
    const auto height = static_cast<double>(m_activeView->plane->height);
    rubberBandZoom(*m_activeView,
        QRectF(0.275 * width, 0.35 * height, 0.45 * width, 0.2 * height));
}

void MainWindow::rubberBandZoomTallActiveViewForTest()
{
    if (m_activeView == nullptr || m_activeView->plane->width <= 0
        || m_activeView->plane->height <= 0) {
        return;
    }
    // The transpose of the rectangular selection above. A wide and a tall
    // selection err on opposite sides when the arrival is framed through a
    // padded window, so a framing regression needs both signs covered.
    const auto width = static_cast<double>(m_activeView->plane->width);
    const auto height = static_cast<double>(m_activeView->plane->height);
    rubberBandZoom(*m_activeView,
        QRectF(0.35 * width, 0.275 * height, 0.2 * width, 0.45 * height));
}

bool MainWindow::activeViewRasterSnugForTest() const
{
    if (m_activeView == nullptr || m_activeView->view == nullptr
        || !m_activeView->view->hasImage()
        || m_activeView->view->viewport() == nullptr) {
        return false;
    }
    const auto* view = m_activeView->view;
    const auto shown = view->mapFromScene(
        view->imageSceneRect()).boundingRect();
    const auto viewport = view->viewport()->rect();
    // A raster fitted to the pane touches both borders on its limiting axis
    // and never spills past the pane. fitInView keeps a hardcoded 2-pixel
    // margin per side, hence the tolerances: one pixel of overflow for
    // round-off, six pixels of slack (the 2x2 margin plus rounding) before
    // the limiting axis counts as falling short of the pane. Six is
    // deliberate: framing the arrival through a window that was itself
    // produced by a fitInView applies the margin twice, which reads as
    // eight-plus pixels of slack and must fail here.
    if (!viewport.adjusted(-1, -1, 1, 1).contains(shown)) {
        return false;
    }
    const auto slackX = viewport.width() - shown.width();
    const auto slackY = viewport.height() - shown.height();
    return std::min(slackX, slackY) <= 6;
}

bool MainWindow::allViewsRubberBandZoomedForTest()
{
    const auto views = currentViews();
    return views.size() > 1
        && rubberBandZoomedViewCountForTest() == views.size();
}

std::size_t MainWindow::rubberBandZoomedViewCountForTest()
{
    const auto views = currentViews();
    return static_cast<std::size_t>(
        std::count_if(views.begin(), views.end(), [](const auto* state) {
            return state->visibleRegion.has_value();
        }));
}

bool MainWindow::activeViewHasFocusForTest() const
{
    return m_activeView != nullptr && m_activeView->view != nullptr
        && m_activeView->view->hasFocus();
}

void MainWindow::focusLevelSelectorForTest()
{
    if (m_levelSelector != nullptr) {
        m_levelSelector->setFocus(::Qt::OtherFocusReason);
    }
}

void MainWindow::selectSphericalDisplayForTest(int mode)
{
    if (m_sphericalDisplayGroup == nullptr) {
        return;
    }
    for (auto* action : m_sphericalDisplayGroup->actions()) {
        if (action->data().toInt() == mode) {
            action->trigger();
            return;
        }
    }
}

void MainWindow::clearFocusForTest()
{
    // A freshly shown window gives the image view focus on its own, which
    // hides whether the open path takes it deliberately. Clearing first is
    // what makes that observable.
    if (auto* focused = QApplication::focusWidget()) {
        focused->clearFocus();
    }
}

void MainWindow::setActiveViewScaleForTest(int factor)
{
    if (m_activeView != nullptr) {
        m_activeView->view->setFixedScale(factor);
    }
}

void MainWindow::selectFixedScaleForTest(int factor)
{
    if (m_scaleGroup == nullptr) {
        return;
    }
    const auto label = plainScaleLabel(factor);
    for (auto* action : m_scaleGroup->actions()) {
        auto text = action->text();
        text.remove(QLatin1Char('&'));
        if (text == label) {
            action->trigger();
            return;
        }
    }
}

void MainWindow::selectToolbarFixedScaleForTest(int factor)
{
    if (m_scaleButton == nullptr || m_scaleButton->menu() == nullptr) {
        return;
    }
    const auto label = plainScaleLabel(factor);
    for (auto* action : m_scaleButton->menu()->actions()) {
        auto text = action->text();
        text.remove(QLatin1Char('&'));
        if (text == label) {
            action->trigger();
            return;
        }
    }
}

QString MainWindow::scaleUiLabelForTest() const
{
    return m_scaleButton == nullptr ? QString() : m_scaleButton->text();
}

QString MainWindow::scaleMenuCheckedLabelForTest() const
{
    if (m_scaleGroup == nullptr) {
        return {};
    }
    auto* checked = m_scaleGroup->checkedAction();
    if (checked == nullptr) {
        return {};
    }
    auto text = checked->text();
    text.remove(QLatin1Char('&'));
    return text;
}

QRectF MainWindow::datasetPhysicalDomainForTest() const
{
    if (!m_openMetadata || m_openMetadata->levels.empty()
        || m_activeView == nullptr) {
        return {};
    }
    const auto domain = datasetSampleBounds(*m_openMetadata);
    const auto axes = displayAxes(m_activeView->normal);
    const auto x = static_cast<std::size_t>(axes[0]);
    const auto y = static_cast<std::size_t>(axes[1]);
    return QRectF(QPointF(domain.lower[x], domain.lower[y]),
        QPointF(domain.upper[x], domain.upper[y]));
}

double MainWindow::activeViewFinestCellSizeForTest() const
{
    if (!m_openMetadata || m_openMetadata->levels.empty()
        || m_activeView == nullptr) {
        return 0.0;
    }
    const auto& finest = m_openMetadata->levels[static_cast<std::size_t>(
        std::max(0, m_openMetadata->finestLevel))];
    return finest.cellSize[static_cast<std::size_t>(
        displayAxes(m_activeView->normal)[0])];
}

void MainWindow::wheelActiveViewForTest(int notches)
{
    if (m_activeView == nullptr || m_activeView->view->viewport() == nullptr) {
        return;
    }
    auto* viewport = m_activeView->view->viewport();
    const auto centre = viewport->rect().center();
    QWheelEvent event(QPointF(centre), viewport->mapToGlobal(QPointF(centre)),
        QPoint(), QPoint(0, notches * 120), ::Qt::NoButton, ::Qt::NoModifier,
        ::Qt::NoScrollPhase, false);
    QApplication::sendEvent(viewport, &event);
}

bool MainWindow::activeViewVirtualCanvasActiveForTest() const
{
    return m_activeView != nullptr
        && m_activeView->view->virtualCanvasActive();
}

bool MainWindow::fixedScaleStateMatchesForTest(int factor) const
{
    if (m_activeView == nullptr || m_scaleGroup == nullptr
        || m_scaleButton == nullptr) {
        return false;
    }
    // The radio item always carries the plain factor; the button carries the
    // clamped label where one applies, so the two are compared against
    // different strings on purpose.
    const auto wanted = plainScaleLabel(factor);
    const auto expectedButton
        = fixedScaleLabel(factor, effectiveFixedScale(factor));
    auto* checked = m_scaleGroup->checkedAction();
    auto checkedText = checked == nullptr ? QString() : checked->text();
    checkedText.remove(QLatin1Char('&'));
    const auto* view = m_activeView->view;
    return view->transformMode() == ImageView::TransformMode::FixedScale
        && view->fixedScaleFactor() == factor
        && std::fabs(view->transform().m11() - factor) <= 1.0e-12
        && m_scaleButton->text() == expectedButton && checkedText == wanted;
}

void MainWindow::wheelZoomAndPanActiveViewForTest()
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return;
    }
    m_activeView->view->zoomBy(1.5);
    m_activeView->view->panViewport(QPoint(11, -7));
}

void MainWindow::shiftDragActiveViewForTest(int dx, int dy)
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return;
    }
    auto* const viewport = m_activeView->view->viewport();
    if (viewport == nullptr) {
        return;
    }
    const QPoint start = viewport->rect().center();
    const QPoint finish = start + QPoint(dx, dy);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(start),
        viewport->mapToGlobal(start), Qt::LeftButton, Qt::LeftButton,
        Qt::ShiftModifier);
    QApplication::sendEvent(viewport, &press);
    QMouseEvent move(QEvent::MouseMove, QPointF(finish),
        viewport->mapToGlobal(finish), Qt::NoButton, Qt::LeftButton,
        Qt::ShiftModifier);
    QApplication::sendEvent(viewport, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(finish),
        viewport->mapToGlobal(finish), Qt::LeftButton, Qt::NoButton,
        Qt::ShiftModifier);
    QApplication::sendEvent(viewport, &release);
}

bool MainWindow::activeViewScrollBarsVisibleForTest() const
{
    if (m_activeView == nullptr) {
        return false;
    }
    // Scroll range, not widget visibility: under the as-needed policy a
    // non-empty range is what shows the bars, and the range updates
    // synchronously while the widgets only follow on the next layout pass.
    const auto* view = m_activeView->view;
    return view->horizontalScrollBar()->maximum()
            > view->horizontalScrollBar()->minimum()
        || view->verticalScrollBar()->maximum()
            > view->verticalScrollBar()->minimum();
}

QRectF MainWindow::activeViewVisibleDataWindowForTest() const
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return {};
    }
    const auto* view = m_activeView->view;
    const auto& plane = *m_activeView->plane;
    if (plane.width < 1 || plane.height < 1) {
        return {};
    }
    // Raster pixels, not scene units: a remote fixed scale hosts the fetched
    // raster on a whole-domain virtual canvas whose scene coordinates are
    // finest cells, so intersecting the viewport's scene rect with the pixel
    // rect directly would report a window in the wrong space (and offset by
    // the fetched window's position in the domain) whenever the canvas
    // scrolls.
    //
    // The second clamp is not redundant with the one inside visibleImageRect:
    // that one clamps to the pixmap, while the normalization below divides by
    // the plane's dimensions, and a supersampled spherical warp makes the
    // pixmap larger than the plane. Clamping to the plane rect keeps the
    // fractions in range there, exactly as this probe did before.
    const auto visible = view->visibleImageRect().intersected(
        QRectF(0.0, 0.0, plane.width, plane.height));
    const auto axes = displayAxes(m_activeView->normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto& region = plane.physicalRegion;
    const auto xExtent = region.upper[xAxis] - region.lower[xAxis];
    const auto yExtent = region.upper[yAxis] - region.lower[yAxis];
    const auto x0 = region.lower[xAxis]
        + visible.left() / plane.width * xExtent;
    const auto x1 = region.lower[xAxis]
        + visible.right() / plane.width * xExtent;
    const auto y0 = region.upper[yAxis]
        - visible.bottom() / plane.height * yExtent;
    const auto y1 = region.upper[yAxis]
        - visible.top() / plane.height * yExtent;
    return QRectF(QPointF(x0, y0), QPointF(x1, y1)).normalized();
}

void MainWindow::panActiveViewForTest(
    double sceneDeltaX, double sceneDeltaY)
{
    if (m_activeView == nullptr) {
        return;
    }
    const QPointF delta(sceneDeltaX, sceneDeltaY);
    // A real drag reports the mouse movement in viewport pixels alongside
    // the scene delta; the view-only pan path (virtual canvases included)
    // scrolls by exactly those pixels.
    const auto* view = m_activeView->view;
    const QPoint viewportDelta(
        static_cast<int>(std::round(sceneDeltaX * view->transform().m11())),
        static_cast<int>(std::round(sceneDeltaY * view->transform().m22())));
    beginPanDrag(*m_activeView);
    updatePanDrag(*m_activeView, delta, viewportDelta);
    endPanDrag(*m_activeView, delta);
}

qreal MainWindow::activeViewScaleForTest() const
{
    return m_activeView != nullptr ? m_activeView->view->transform().m11() : 0.0;
}

bool MainWindow::activeViewIsFitToWindowForTest()
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return false;
    }
    const auto before = m_activeView->view->transform();
    m_activeView->view->fitToWindow();
    return before == m_activeView->view->transform();
}

bool MainWindow::activeViewShowsWholeImageForTest() const
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return false;
    }
    auto* view = m_activeView->view;
    const auto visible = view->mapToScene(
        view->viewport()->rect()).boundingRect();
    // Half-a-scene-pixel slack absorbs fitInView rounding at the borders.
    return visible.adjusted(-0.5, -0.5, 0.5, 0.5).contains(
        view->imageSceneRect());
}

void MainWindow::viewFabForTest(std::size_t index)
{
    m_fabNavigator->viewEntry(index);
}

bool MainWindow::activeViewIsZoomedForTest() const
{
    return m_activeView != nullptr && m_activeView->visibleRegion.has_value();
}

void MainWindow::setSphericalSupersampleForTest(int factor)
{
    // Mirror the menu handler: record the factor and re-warp the cached planes.
    m_sphericalSupersample = factor;
    if (displayIsSpherical()) {
        scheduleSliceRequest(true);
    }
}

int MainWindow::activeViewImageWidthForTest() const
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return 0;
    }
    return m_activeView->view->image().width();
}

std::array<int, 2> MainWindow::activeViewImageSizeForTest() const
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return {0, 0};
    }
    const auto image = m_activeView->view->image();
    return {image.width(), image.height()};
}

std::array<int, 2> MainWindow::activeViewViewportSizeForTest() const
{
    if (m_activeView == nullptr || m_activeView->view->viewport() == nullptr) {
        return {0, 0};
    }
    const auto* viewport = m_activeView->view->viewport();
    return {viewport->width(), viewport->height()};
}

QImage MainWindow::activeViewViewportImageForTest() const
{
    if (m_activeView == nullptr || m_activeView->view->viewport() == nullptr) {
        return {};
    }
    return m_activeView->view->viewport()->grab().toImage();
}

bool MainWindow::activeViewFitsWindowForTest() const
{
    return m_activeView != nullptr && m_activeView->view->hasImage()
        && m_activeView->view->isFitToWindow();
}

void MainWindow::setCacheBudgetForTest(std::uint64_t bytes)
{
    if (m_dataset) {
        // The return (whether resident already fits) is irrelevant here; the
        // next non-cache slice re-pins and triggers the fallback.
        static_cast<void>(m_dataset->setCacheBudget(bytes));
    }
}

std::uint64_t MainWindow::cacheResidentBytesForTest() const
{
    return m_dataset ? m_dataset->cacheMetrics().residentBytes : 0;
}

void MainWindow::setParticleSelectionForTest(
    std::vector<std::string> species, double fraction, std::uint64_t seed)
{
    m_particleController->applySelection(std::move(species), fraction,
        m_particleController->settings().pointSize, seed);
}

std::uint64_t MainWindow::particleSeedForTest() const noexcept
{
    return m_particleController->settings().seed;
}

double MainWindow::particleFractionForTest() const noexcept
{
    return m_particleController->settings().fraction;
}

void MainWindow::setParticlePointSizeForTest(int pointSize)
{
    // A copy: applySelection overwrites the settings it would read from.
    const auto settings = m_particleController->settings();
    m_particleController->applySelection(
        settings.species, settings.fraction, pointSize, settings.seed);
}

int MainWindow::particlePointSizeForTest() const noexcept
{
    return m_particleController->settings().pointSize;
}

QColor MainWindow::particleColorForTest(const std::string& species) const
{
    const auto& colors = m_particleController->settings().colors;
    const auto color = colors.find(species);
    return color != colors.end() ? color->second : QColor();
}

void MainWindow::setParticleColorForTest(
    const std::string& species, const QColor& color)
{
    m_particleController->setColor(species, color);
}

bool MainWindow::particleOverlaysUseColorForTest(const QColor& color)
{
    bool found = false;
    for (const auto* state : currentViews()) {
        for (const auto& overlayColor : state->view->pointOverlayColors()) {
            found = true;
            if (overlayColor != color) {
                return false;
            }
        }
    }
    return found;
}

std::size_t MainWindow::particleSampleCountForTest() const
{
    std::size_t count = 0;
    for (const auto& sample : m_particleController->samples()) {
        count += sample.points.size();
    }
    return count;
}

std::size_t MainWindow::particleOverlayCountForTest()
{
    std::size_t count = 0;
    for (const auto* state : currentViews()) {
        count += state->view->pointOverlayCount();
    }
    return count;
}

bool MainWindow::particleLoadingForTest() const noexcept
{
    return m_particleController->loading();
}

bool MainWindow::particleLoadingUiActiveForTest() const
{
    return m_particleController->loadingUiActive();
}

bool MainWindow::particleLoadingUiSettledForTest() const
{
    return m_particleController->loadingUiSettled();
}

void MainWindow::openStandaloneFabForTest(const std::filesystem::path& path)
{
    m_fabNavigator->openStandaloneFab(path);
}

void MainWindow::startAnimationExportForTest(const QString& path,
    bool includeColorBar)
{
    if (m_animationExporter->active()) {
        return;
    }
    beginAnimationExport(path, includeColorBar);
}

int MainWindow::backgroundErrorCountForTest() const
{
    return m_diagnosticsModel->backgroundErrorCount();
}

} // namespace amrvis::qt
