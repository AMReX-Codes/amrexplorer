#include "MainWindowInternal.hpp"

namespace amrvis::qt {

namespace {

void populateLevelCombo(QComboBox* combo, int finestLevel)
{
    combo->clear();
    combo->addItem(QObject::tr("Finest available"), -1);
    // "Level N only" is redundant when there is only one level; the whole
    // block is skipped for finestLevel == 0 so the combo shows just the
    // "Finest available" entry.
    if (finestLevel <= 0) {
        return;
    }
    // "Update to Level N" (composite 0..N) in reverse order, from
    // finestLevel-1 down to 1; only when there are at least three levels.
    for (int level = finestLevel - 1; level >= 1; --level) {
        combo->addItem(QObject::tr("Levs 0-%1").arg(level),
            kUpdateToLevelOffset + level);
    }
    for (int level = 0; level <= finestLevel; ++level) {
        combo->addItem(QObject::tr("Level %1 only").arg(level), level);
    }
}

} // namespace

void MainWindow::enableDatasetControls(const DatasetMetadata& metadata)
{
    m_controlsReady = true;
    m_fieldSelector->setEnabled(true);
    m_levelSelector->setEnabled(true);
    m_rangeMode->setEnabled(true);
    m_logarithmic->setEnabled(true);
    m_boxesAction->setEnabled(true);
    m_slicePlanesAction->setEnabled(metadata.dimension == 3);
    const auto userRange = static_cast<RangeMode>(
        m_rangeMode->currentData().toInt()) == RangeMode::User;
    m_rangeMinimum->setEnabled(userRange);
    m_rangeMaximum->setEnabled(userRange);
    rebuildLevelMenu();
    m_levelMenu->setEnabled(true);
    m_contoursAction->setEnabled(true);
    m_datasetAction->setEnabled(true);
}

void MainWindow::configureSliceControls()
{
    if (!m_dataset) {
        return;
    }
    const QSignalBlocker fieldBlocker(m_fieldSelector);
    const QSignalBlocker levelBlocker(m_levelSelector);
    const auto& metadata = m_dataset->metadata();

    m_fieldSelector->clear();
    for (std::size_t field = 0; field < metadata.fields.size(); ++field) {
        m_fieldSelector->addItem(QString::fromStdString(metadata.fields[field].name),
            static_cast<unsigned int>(field));
    }
    m_fieldSelector->setCurrentIndex(0);

    populateLevelCombo(m_levelSelector, metadata.finestLevel);
    m_levelSelector->setCurrentIndex(0);

    enableDatasetControls(metadata);

    rebuildVariableMenu();
    updateRangeModeAvailability();

    // Switch the stacked page to match the dataset dimension and, for 3-D,
    // reveal the shared slice position controls and the iso wireframe.
    const auto isThreeDimensional = metadata.dimension == 3;
    m_syncRubberBandZoomAction->setVisible(isThreeDimensional);
    m_stack->setCurrentIndex(isThreeDimensional ? 1 : 0);
    m_animationPanel->setSweepVisible(isThreeDimensional);
    updateAnimationDockVisibility();
    configureSlicePositionControls();
    if (isThreeDimensional) {
        m_isoWidget->setGeometry(metadata);
        m_isoWidget->setSlicePositions(m_slicePosition3d[0], m_slicePosition3d[1],
            m_slicePosition3d[2]);
    }
    ensureVectorFieldDefaults();
}

void MainWindow::setSlicePositionControlsVisible(bool visible)
{
    m_slicePositionControls->setVisible(visible);
    if (m_positionSeparator != nullptr) {
        m_positionSeparator->setVisible(visible);
    }
}

void MainWindow::configureSlicePositionControls()
{
    if (!m_dataset) {
        setSlicePositionControlsVisible(false);
        return;
    }
    setSlicePositionControlsVisible(true);
    const auto& md = m_dataset->metadata();

    if (md.dimension != 3) {
        // 2-D: dim rather than hide — there is no slice depth to control,
        // but the user can see Position is a 3-D-only concept.
        m_slicePositionControls->setEnabled(false);
        return;
    }

    const auto level = sliceIndexLevel();
    if (level < 0 || static_cast<std::size_t>(level) >= md.levels.size()) {
        m_slicePositionControls->setEnabled(false);
        return;
    }

    m_slicePositionControls->setEnabled(true);
    const auto& levelMd = md.levels[static_cast<std::size_t>(level)];
    for (std::size_t axis = 0; axis < 3; ++axis) {
        auto* spin = m_sliceSpinboxes[axis];
        const QSignalBlocker blocker(spin);
        // Cell-centered: indices from domain.lower to domain.upper inclusive.
        // Nodal data would have one extra node at the upper end: domain.upper+1.
        const auto iMin = levelMd.domain.lower[axis];
        const auto iMax = levelMd.domain.upper[axis];
        spin->setRange(iMin, iMax);
        spin->setSingleStep(1);
        spin->setValue(sliceIndexForPosition(md, level,
            static_cast<int>(axis), m_slicePosition3d[axis]));
    }
}

int MainWindow::sliceIndexLevel() const
{
    if (!m_dataset || m_dataset->metadata().dimension != 3) {
        return -1;
    }
    const auto levelData = m_levelSelector->currentData().toInt();
    return decodeLevelData(levelData, m_dataset->metadata().finestLevel).maximumLevel;
}

void MainWindow::setSlicePosition(int axis, double value)
{
    if (!m_dataset || m_dataset->metadata().dimension != 3) {
        return;
    }
    const auto ax = static_cast<std::size_t>(axis);
    const auto domain = datasetSampleBounds(m_dataset->metadata());
    const auto position = std::clamp(value, domain.lower[ax],
        std::nextafter(domain.upper[ax], domain.lower[ax]));
    m_slicePosition3d[ax] = position;
    {
        const QSignalBlocker blocker(m_sliceSpinboxes[ax]);
        const auto level = sliceIndexLevel();
        if (level >= 0 && static_cast<std::size_t>(level)
            < m_dataset->metadata().levels.size()) {
            m_sliceSpinboxes[ax]->setValue(sliceIndexForPosition(
                m_dataset->metadata(), level, axis, position));
        }
    }
    m_isoWidget->setSlicePositions(m_slicePosition3d[0], m_slicePosition3d[1],
        m_slicePosition3d[2]);
    // The cached full-domain Visible range is now stale — and so is any
    // pending deferred store, whose union was computed from pre-move planes.
    m_displayCoordinator.invalidateRangeCache();
    m_pendingRangeStore.reset();
    // The other two views only need their crosshair guides redrawn; the view
    // normal to the moved axis gets a fresh (debounced) slice.
    updateCrosshairs();
    scheduleSliceRequest(m_planeViews[ax]);
}

void MainWindow::scheduleSliceRequest(bool rasterDirty)
{
    if (m_controlsReady && m_dataset) {
        // Any slice-affecting UI change funnels through here; a prefetched
        // frame rendered against the old spec is obsolete.
        m_sequenceController->invalidatePrefetch();
        // If a sequence frame is still loading, restart it so the in-flight
        // load is rebuilt from the new spec instead of finishing stale.
        if (m_sequenceController->inFlight()
            && m_sequenceController->currentIndex() >= 0) {
            goToSequenceFrame(m_sequenceController->currentIndex(), true);
            return;
        }
        m_pendingRasterDirty = m_pendingRasterDirty || rasterDirty;
        m_pendingAllViews = true;
        m_sliceDebounce->start();
    }
}

void MainWindow::scheduleSliceRequest(PlaneViewState& state, bool rasterDirty)
{
    if (m_controlsReady && m_dataset) {
        m_sequenceController->invalidatePrefetch();
        // If a sequence frame is still loading, restart it so the in-flight
        // load is rebuilt from the new spec instead of finishing stale.
        if (m_sequenceController->inFlight()
            && m_sequenceController->currentIndex() >= 0) {
            goToSequenceFrame(m_sequenceController->currentIndex(), true);
            return;
        }
        m_pendingRasterDirty = m_pendingRasterDirty || rasterDirty;
        if (std::find(m_pendingViews.begin(), m_pendingViews.end(), &state)
            == m_pendingViews.end()) {
            m_pendingViews.push_back(&state);
        }
        m_sliceDebounce->start();
    }
}

void MainWindow::flushSliceRequests()
{
    std::vector<PlaneViewState*> targets;
    if (m_pendingAllViews) {
        targets = currentViews();
    } else {
        targets = m_pendingViews;
    }
    m_pendingAllViews = false;
    m_pendingViews.clear();
    const auto rasterDirty = m_pendingRasterDirty;
    m_pendingRasterDirty = false;
    for (auto* state : targets) {
        requestSlice(*state, rasterDirty);
    }
}

void MainWindow::requestSlice(PlaneViewState& state, bool rasterDirty)
{
    if (!m_controlsReady || !m_dataset
        || m_fieldSelector->currentIndex() < 0
        || m_levelSelector->currentIndex() < 0) {
        return;
    }
    updateRangeModeAvailability();

    const auto dataset = m_dataset;
    const auto& metadata = dataset->metadata();
    SliceRequest request;
    request.dataset = dataset->id();
    request.field.value = m_fieldSelector->currentData().toUInt();
    request.normalDirection = state.normal;
    if (metadata.dimension == 3) {
        request.physicalPosition
            = m_slicePosition3d[static_cast<std::size_t>(state.normal)];
    }
    request.visibleRegion = state.visibleRegion.value_or(
        datasetSampleBounds(metadata));
    request.outputSize = sliceOutputSize(state);
    const auto level = m_levelSelector->currentData().toInt();
    const auto [composition, maximumLevel] = decodeLevelData(
        level, metadata.finestLevel);
    request.composition = composition;
    request.maximumLevel = maximumLevel;
    request.includeGridBoxes = m_boxesAction->isChecked();
    request.sphericalSupersample = m_sphericalSupersample;
    request.sphericalDisplay = m_sphericalDisplay;

    const auto requestedRangeMode = static_cast<RangeMode>(
        m_rangeMode->currentData().toInt());
    const auto rangeMode = effectiveRangeMode(dataset, request.field,
        maximumLevel, composition, requestedRangeMode);
    std::optional<std::pair<double, double>> userRange;
    if (rangeMode == RangeMode::User) {
        userRange = std::pair{m_rangeMinimum->value(), m_rangeMaximum->value()};
    }
    const auto logarithmic = m_logarithmic->isChecked();
    const auto palette = m_palette;
    const auto displayMode = m_displayMode;
    // Each 3-D panel uses a different pair of vector components:
    //   XY (normal=2) → U,V   XZ (normal=1) → U,W   YZ (normal=0) → V,W
    // 2-D always uses U,V.
    const auto u = static_cast<std::uint32_t>(std::max(m_vectorUField, 0));
    const auto v = static_cast<std::uint32_t>(std::max(m_vectorVField, 0));
    const auto w = static_cast<std::uint32_t>(std::max(m_vectorWField, 0));
    const auto vectorUField = (metadata.dimension == 3 && state.normal == 0) ? v : u;
    const auto vectorVField = (metadata.dimension == 3)
        ? (state.normal == 2 ? v : w) : v;
    const auto contourCount = m_contourCount;

    const auto fromCache = state.hasCachedRequest
        && state.plane->width > 0
        && sameSliceSpec(state.cachedRequest, request)
        && state.cachedVectorVField == vectorVField
        && state.cachedVectorUField == vectorUField
        && displayMode == state.cachedMode
        && (!isContourMode(displayMode) || state.contourFinePlane->width > 0)
        && (displayMode != DisplayMode::VelocityVectors
            || (!state.vectorSegments.empty()
                && contourCount == state.cachedContourCount
                // Cached glyphs are layout-specific for a spherical dataset:
                // R-Z segments carry display (R, Z) coordinates while the
                // logical layouts carry plane pixels, so a display-mode switch
                // must regenerate them. (A supersample change is fine: R-Z
                // segments are resolution-independent physical coordinates.)
                && (!displayIsSpherical()
                    || (state.cachedRequest.sphericalDisplay
                            == request.sphericalDisplay)
                    || (state.cachedRequest.sphericalDisplay
                            != SphericalDisplay::RZ
                        && request.sphericalDisplay != SphericalDisplay::RZ))));

    state.stopSource.request_stop();
    state.stopSource = StopSource{};
    const auto cancellation = state.stopSource.get_token();
    const auto generation = m_generation;
    const auto sliceGeneration = ++state.sliceGeneration;
    ++state.pendingRequests;
    ++m_activeRequests;
    const auto tag = m_viewDimension == 3
        ? tr(" (%1)").arg(state.label) : QString();
    statusBar()->showMessage(tr("Loading %1%2...").arg(
        m_fieldSelector->currentText(), tag));
    updateDiagnostics();

    QFuture<SliceDisplayResult> future;
    if (fromCache) {
        // Cheap path: re-range, re-render, and re-contour the cached planes
        // on a worker; no SliceQuery runs at all. The captures are shared_ptr
        // snapshots — refcount bumps, not the former ~110 MB plane deep copy
        // on the GUI thread. A newer arrival can safely replace the view's
        // pointers meanwhile; this worker keeps reading its own snapshots.
        // (refreshCachedSlice's by-value parameters still copy the planes,
        // but on the worker thread.)
        future = QtConcurrent::run([dataset, request,
            displayPlane = state.plane,
            contourPlane = state.contourPlane,
            contourFinePlane = state.contourFinePlane,
            contourFineFactor = state.contourFineFactor,
            vectors = state.vectorSegments,
            rangeMode, userRange, logarithmic, palette, displayMode,
            vectorUField, vectorVField, contourCount, rasterDirty,
            cancellation]() mutable {
            return refreshCachedSlice(dataset, request, *displayPlane,
                *contourPlane, *contourFinePlane,
                contourFineFactor, std::move(vectors), rangeMode, userRange,
                logarithmic, palette, displayMode, vectorUField, vectorVField,
                contourCount, rasterDirty, cancellation);
        });
    } else {
        future = QtConcurrent::run(
            [dataset, request, rangeMode, userRange, logarithmic, palette,
                cancellation, displayMode, vectorUField, vectorVField,
                contourCount]() mutable {
            // The pipeline owns the whole non-cached slice worker, including
            // the cache-pressure level fallback (see
            // cache-budget-exceeded-hard-fails-after-load).
            return executeSliceWithFallback(dataset, request, rangeMode,
                userRange, logarithmic, palette, displayMode, vectorUField,
                vectorVField, contourCount, cancellation);
        });
    }

    auto* watcher = new QFutureWatcher<SliceDisplayResult>(this);
    connect(watcher, &QFutureWatcher<SliceDisplayResult>::finished, this,
        [this, watcher, dataset, generation, sliceGeneration, cancellation,
         &state, rangeMode] {
            --state.pendingRequests;
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                // takeResult, not result(): result() returns a reference into
                // the future and copying out of it duplicates every plane in
                // the arrival before showSlice has even seen it.
                auto result = watcher->future().takeResult();
                if (generation == m_generation
                    && sliceGeneration == state.sliceGeneration) {
                    // Cache the full-domain range whenever we get a non-zoomed
                    // Visible-range slice; reuse it for zoomed (subregion)
                    // slices so the color bar stays stable during pan and zoom.
                    // Whether this slice covered the full domain must come from
                    // the request that produced it, not live view state:
                    // rubber-band zoom / pan / reset-zoom mutate
                    // state.visibleRegion without bumping sliceGeneration (they
                    // only schedule the debounced re-slice), so a slice in
                    // flight when one of those fires would be misclassified --
                    // caching a subregion range as the full-domain range on a
                    // reset, or dropping the full-domain range on a zoom
                    // (range-cache-staleness-races).
                    const bool isFullDomain = result.request.visibleRegion
                        == datasetSampleBounds(dataset->metadata());
                    const DisplayCoordinator::RangeKey rangeKey{
                        result.request.dataset, result.request.field,
                        result.request.maximumLevel,
                        result.request.composition};
                    const auto cachedRange = !isFullDomain
                        && rangeMode == RangeMode::Visible
                            ? m_displayCoordinator.cachedFullDomainRange(
                                rangeKey)
                            : std::nullopt;
                    if (cachedRange) {
                        // The subregion result was produced against its own
                        // range; realign it to the reused full-domain range
                        // so it matches the colorbar. In 3-D the shared-range
                        // sync below realigns every panel, so only 2-D (which
                        // it skips) realigns the raster and contours here —
                        // that also avoids rendering each 3-D panel twice.
                        DisplayCoordinator::realignArrivalToRange(result,
                            *cachedRange, m_palette, m_viewDimension != 3);
                    }
                    // showSlice takes the arrival by value; the fallback levels
                    // are still needed below, so copy them out first rather
                    // than reading them back off a moved-from result.
                    const auto fallbackToLevel = result.cacheFallbackToLevel;
                    const auto fallbackFromLevel = result.cacheFallbackFromLevel;
                    showSlice(state, std::move(result));
                    syncVisibleRanges();
                    // Cache the full-domain range. In 3-D the store defers to
                    // the (async) shared-range sync's completion so the union
                    // across all panels is captured; 2-D has no later sync
                    // and stores the panel's own range immediately.
                    if (isFullDomain && rangeMode == RangeMode::Visible
                        && state.plane->width > 0) {
                        if (m_viewDimension == 3) {
                            m_pendingRangeStore = rangeKey;
                        } else {
                            m_displayCoordinator.storeFullDomainRange(rangeKey,
                                {state.displayMinimum, state.displayMaximum});
                        }
                    }
                    const auto cache = dataset->cacheMetrics();
                    m_cacheBudgetBytes = cache.budgetBytes;
                    m_cacheResidentBytes = cache.residentBytes;
                    m_cachePinnedBytes = cache.pinnedBytes;
                    m_cacheEvictions = cache.evictions;
                    // A cache-pressure fallback lowered the composite level;
                    // reflect it in the level combo (no re-slice) and inform the
                    // user, matching the initial-load handling.
                    if (fallbackToLevel >= 0) {
                        if (selectCacheFallbackLevel(
                                m_levelSelector, fallbackToLevel)) {
                            configureSlicePositionControls();
                            updateRangeModeAvailability();
                            syncMenuChecks();
                        }
                        statusBar()->showMessage(cacheFallbackMessage(
                            *dataset, fallbackFromLevel, fallbackToLevel));
                    }
                } else {
                    ++m_staleResults;
                }
            } catch (const std::exception& error) {
                if (generation == m_generation
                    && sliceGeneration == state.sliceGeneration
                    && !cancellation.stop_requested()) {
                    reportBackgroundError(
                        tr("Cannot load slice: %1").arg(exceptionMessage(error)));
                } else {
                    ++m_staleResults;
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
            // The interactive re-slice batch has drained once no view has work
            // in flight; the smoke test waits on this to read settled state.
            if (m_activeRequests == 0) {
                emit interactiveSlicesSettled();
            }
        });
    watcher->setFuture(future);
}

void MainWindow::updateGridBoxes(PlaneViewState& state)
{
    std::vector<GridBoxOverlay> overlays;
    if (!m_boxesAction->isChecked() || !m_dataset || !state.view->hasImage()
        || state.plane->width <= 0 || state.plane->height <= 0) {
        state.view->setGridBoxes(overlays);
        return;
    }

    const auto& metadata = m_dataset->metadata();
    const auto& plane = *state.plane;
    const auto axes = displayAxes(state.normal);
    const auto rawLevel = m_levelSelector->currentData().toInt();
    const auto [composition, maximumLevel] = decodeLevelData(
        rawLevel, metadata.finestLevel);
    const auto firstLevel = composition == CompositionPolicy::ExactLevel
        ? maximumLevel : 0;
    const auto lastLevel = maximumLevel;

    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto xExtent = plane.physicalRegion.upper[xAxis]
        - plane.physicalRegion.lower[xAxis];
    const auto yExtent = plane.physicalRegion.upper[yAxis]
        - plane.physicalRegion.lower[yAxis];
    const bool spherical = displayIsSpherical();
    const auto mapping = planeMapping(state);
    for (const auto& gridBox : state.gridBoxes) {
        const auto levelIndex = gridBox.level;
        if (levelIndex < firstLevel || levelIndex > lastLevel) {
            continue;
        }
        const auto& physicalBox = gridBox.physicalRegion;
        const auto xLower = physicalBox.lower[xAxis];
        const auto xUpper = physicalBox.upper[xAxis];
        const auto yLower = physicalBox.lower[yAxis];
        const auto yUpper = physicalBox.upper[yAxis];
        const auto color = levelIndex == firstLevel
            ? QColor(Qt::white)
            : QColor::fromRgb(static_cast<QRgb>(
                m_palette.levelColor(levelIndex, lastLevel)));
        if (spherical) {
            // xAxis is r, yAxis is theta. Branch on the state's mode (the
            // raster on screen), not m_sphericalDisplay (the menu
            // selection): between a mode change and the re-rendered
            // arrival the two disagree, and the overlay must match the
            // displayed raster.
            if (!(xUpper > xLower) || !(yUpper > yLower)) {
                continue;
            }
            if (state.sphericalDisplay == SphericalDisplay::RZ) {
                // Warped wedge: a curved annular sector.
                GridBoxOverlay overlay;
                overlay.color = color;
                overlay.path = sphericalSectorPath(
                    mapping, xLower, xUpper, yLower, yUpper);
                overlays.push_back(std::move(overlay));
            } else {
                // Logical r-theta / theta-r: an axis-aligned rectangle.
                QRectF rect(mapping.sceneFromLogical(xLower, yLower),
                    mapping.sceneFromLogical(xUpper, yUpper));
                rect = rect.normalized();
                if (!rect.isEmpty()) {
                    overlays.push_back({rect, color, QPainterPath{}});
                }
            }
            continue;
        }
        const auto pixelX0 = std::round(
            (xLower - plane.physicalRegion.lower[xAxis])
                / xExtent * plane.width);
        const auto pixelX1 = std::round(
            (xUpper - plane.physicalRegion.lower[xAxis])
                / xExtent * plane.width);
        const auto pixelY0 = std::round(plane.height
            - (yUpper - plane.physicalRegion.lower[yAxis])
                / yExtent * plane.height);
        const auto pixelY1 = std::round(plane.height
            - (yLower - plane.physicalRegion.lower[yAxis])
                / yExtent * plane.height);
        if (pixelX0 == pixelX1 || pixelY0 == pixelY1) {
            continue;
        }
        QRectF rectangle(QPointF(pixelX0, pixelY0), QPointF(pixelX1, pixelY1));
        rectangle = rectangle.normalized().intersected(
            QRectF(0.0, 0.0, plane.width, plane.height));
        if (!rectangle.isEmpty()) {
            overlays.push_back({rectangle, color, QPainterPath{}});
        }
    }
    state.view->setGridBoxes(overlays);
}

void MainWindow::updateGridBoxes()
{
    for (auto* state : currentViews()) {
        updateGridBoxes(*state);
    }
}

void MainWindow::updateCrosshairs(PlaneViewState& state)
{
    std::optional<QLineF> vertical;
    std::optional<QLineF> horizontal;
    QColor verticalColor;
    QColor horizontalColor;
    if (m_dataset && m_dataset->metadata().dimension == 3
        && state.plane->width > 0 && state.plane->height > 0) {
        const auto axes = displayAxes(state.normal);
        const auto xAxis = static_cast<std::size_t>(axes[0]);
        const auto yAxis = static_cast<std::size_t>(axes[1]);
        const auto& region = state.plane->physicalRegion;
        const auto width = static_cast<double>(state.plane->width);
        const auto height = static_cast<double>(state.plane->height);
        // The vertical guide marks the slice position of the axis pointing
        // horizontally in this view, and vice versa; each guide takes that
        // axis' legacy palette color and hides outside the displayed region.
        const auto xPosition = m_slicePosition3d[xAxis];
        if (xPosition >= region.lower[xAxis] && xPosition <= region.upper[xAxis]) {
            const auto t = (xPosition - region.lower[xAxis])
                / (region.upper[xAxis] - region.lower[xAxis]);
            vertical = QLineF(t * width, 0.0, t * width, height);
            verticalColor = sliceAxisColor(axes[0]);
        }
        const auto yPosition = m_slicePosition3d[yAxis];
        if (yPosition >= region.lower[yAxis] && yPosition <= region.upper[yAxis]) {
            const auto t = (yPosition - region.lower[yAxis])
                / (region.upper[yAxis] - region.lower[yAxis]);
            const auto sceneY = height * (1.0 - t);
            horizontal = QLineF(0.0, sceneY, width, sceneY);
            horizontalColor = sliceAxisColor(axes[1]);
        }
    }
    state.view->setCrosshairs(vertical, horizontal, verticalColor,
        horizontalColor);
}

void MainWindow::updateCrosshairs()
{
    for (auto* state : currentViews()) {
        updateCrosshairs(*state);
    }
}

void MainWindow::showMetadata(
    const PlotfileMetadataResult& result, const std::filesystem::path& path)
{
    m_metadataTree->clear();
    const auto& metadata = *result.metadata;
    const auto addValue = [this](const QString& name, const QString& value) {
        new QTreeWidgetItem(m_metadataTree, {name, value});
    };

    // Standalone FABs and MultiFabs carry neither a simulation time nor an
    // AMR hierarchy, so those rows (and the per-level listing below) would
    // show invented values; they are skipped for such data.
    const bool standalone = !metadata.hasPhysicalGeometry;
    addValue(tr("Dataset"), QString::fromStdString(path.string()));
    addValue(tr("Format"), QString::fromStdString(result.fileVersion));
    addValue(tr("Dimension"), QString::number(metadata.dimension));
    if (!standalone) {
        addValue(tr("Time"), QString::number(metadata.time, 'g', 17));
        addValue(tr("Finest level"), QString::number(metadata.finestLevel));
    }

    auto* fields = new QTreeWidgetItem(
        m_metadataTree, {tr("Fields"), QString::number(metadata.fields.size())});
    for (const auto& field : metadata.fields) {
        const char* centering = "cell";
        switch (field.centering) {
        case amrvis::Centering::Node: centering = "node"; break;
        case amrvis::Centering::FaceX: centering = "face-x"; break;
        case amrvis::Centering::FaceY: centering = "face-y"; break;
        case amrvis::Centering::FaceZ: centering = "face-z"; break;
        case amrvis::Centering::EdgeX: centering = "edge-x"; break;
        case amrvis::Centering::EdgeY: centering = "edge-y"; break;
        case amrvis::Centering::EdgeZ: centering = "edge-z"; break;
        case amrvis::Centering::Mixed: centering = "mixed"; break;
        case amrvis::Centering::Cell: break;
        }
        new QTreeWidgetItem(fields, {
            QString::fromStdString(field.name),
            QString::fromLatin1(centering)
        });
    }

    if (standalone) {
        const auto& level = metadata.levels.front();
        addValue(tr("Grids"), tr("%1 grid(s), %2").arg(level.boxes.size()).arg(
            QString::fromStdString(level.dataPath)));
    } else {
        auto* levels = new QTreeWidgetItem(m_metadataTree,
            {tr("Levels"), QString::number(metadata.levels.size())});
        for (const auto& level : metadata.levels) {
            new QTreeWidgetItem(levels, {
                tr("Level %1").arg(level.level),
                tr("%1 grid(s), %2").arg(level.boxes.size()).arg(
                    QString::fromStdString(level.dataPath))
            });
        }
    }
    m_metadataTree->expandAll();

    m_openMetadata = result.metadata;
    m_fileVersion = result.fileVersion;
    updateWindowTitle();

    m_lastFilesRead = result.metrics.filesRead;
    m_lastBytesRead = result.metrics.bytesRead;
    statusBar()->showMessage(standalone
        ? tr("Metadata loaded: %1 field(s), %2 grid(s)")
              .arg(metadata.fields.size())
              .arg(metadata.levels.front().boxes.size())
        : tr("Metadata loaded: %1 field(s), %2 level(s)")
              .arg(metadata.fields.size())
              .arg(metadata.levels.size()));
}

std::optional<QRectF> MainWindow::preservedDataWindow(
    const PlaneViewState& state, const ScalarPlane& incoming) const
{
    // Spherical scenes are warped (R, Z) while the plane geometry is logical
    // (r, theta); the linear viewport->physical->scene re-frame below does not
    // apply. Spherical never re-slices a subregion (zoom is view-only), so the
    // plain Preserve transform is already correct.
    if (displayIsSpherical()) {
        return std::nullopt;
    }
    // A virtual canvas needs no re-frame, and would be mismapped by one. Its
    // scene is the whole domain in finest cells and is anchored to the domain,
    // not to the raster: applyPlacement re-positions the incoming raster within
    // that unchanged scene and deliberately leaves the view where it is, so the
    // visible window is already preserved. The arithmetic below assumes scene
    // units are raster pixels of the cached plane, which on a canvas they are
    // not.
    //
    // Its call site tests transformMode() == Custom, which used to be taken as
    // excluding the canvas, since applyFixedScale installs one in FixedScale
    // mode. That does not hold: ImageView::zoomBy sets Custom and leaves the
    // placement alone, so one wheel notch over a remote fixed scale reaches
    // here with cell-space coordinates. The guard belongs on the canvas itself
    // rather than on a mode that only usually implies its absence.
    if (state.view->virtualCanvasActive()) {
        return std::nullopt;
    }
    const auto& cached = *state.plane;
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto& oldRegion = cached.physicalRegion;
    const auto& newRegion = incoming.physicalRegion;
    const auto oldExtentX = oldRegion.upper[xAxis] - oldRegion.lower[xAxis];
    const auto oldExtentY = oldRegion.upper[yAxis] - oldRegion.lower[yAxis];
    const auto newExtentX = newRegion.upper[xAxis] - newRegion.lower[xAxis];
    const auto newExtentY = newRegion.upper[yAxis] - newRegion.lower[yAxis];
    // Viewport -> old scene -> physical -> new scene. Scene y runs opposite
    // to physical y: plane row 0 is the bottom row and the displayed raster
    // is mirrored vertically (see displayImageFor), for both planes alike.
    const auto visible = state.view->mapToScene(
        state.view->viewport()->rect()).boundingRect();
    const auto dataX = [&](double sceneX) {
        return oldRegion.lower[xAxis] + sceneX / cached.width * oldExtentX;
    };
    const auto dataY = [&](double sceneY) {
        return oldRegion.upper[yAxis] - sceneY / cached.height * oldExtentY;
    };
    const auto newSceneX = [&](double x) {
        return (x - newRegion.lower[xAxis]) / newExtentX * incoming.width;
    };
    const auto newSceneY = [&](double y) {
        return (newRegion.upper[yAxis] - y) / newExtentY * incoming.height;
    };
    const QRectF window(
        QPointF(newSceneX(dataX(visible.left())),
            newSceneY(dataY(visible.top()))),
        QPointF(newSceneX(dataX(visible.right())),
            newSceneY(dataY(visible.bottom()))));
    if (window.isEmpty()) {
        return std::nullopt;
    }
    return window;
}

std::optional<QRectF> MainWindow::sphericalReframe(
    const PlaneViewState& state, const SliceDisplayResult& display) const
{
    // Only the R-Z warp has a resolution knob (supersampling); r-theta and
    // theta-r never resize in place, and a mode switch changes displayRegion
    // (so the plain refit below applies). Only once a raster is already on
    // screen (the first frame refits), and only when the user has actually
    // zoomed (a fit-to-window view should stay fit and keep auto-fitting).
    if (!displayIsSpherical()
        || display.sphericalDisplay != SphericalDisplay::RZ
        || state.sphericalDisplay != SphericalDisplay::RZ
        || !state.view->hasImage() || state.view->isFitToWindow()
        || !(display.displayRegion == state.displayRegion)) {
        return std::nullopt;
    }
    const auto oldSize = state.view->image().size();
    const QSize newSize(display.image.width, display.image.height);
    if (oldSize.width() <= 0 || oldSize.height() <= 0 || newSize == oldSize) {
        return std::nullopt;  // no resolution change (e.g. a field/range refresh)
    }
    // The physical bounds are unchanged, so the currently-visible physical
    // window occupies a scene rect scaled by the pixmap-resolution ratio.
    const auto visible = state.view->mapToScene(
        state.view->viewport()->rect()).boundingRect();
    const double sx = static_cast<double>(newSize.width())
        / static_cast<double>(oldSize.width());
    const double sy = static_cast<double>(newSize.height())
        / static_cast<double>(oldSize.height());
    return QRectF(visible.x() * sx, visible.y() * sy,
        visible.width() * sx, visible.height() * sy);
}

void MainWindow::showSlice(PlaneViewState& state, SliceDisplayResult display)
{
    if (!display.rasterUnchanged) {
        if (!display.image.valid()) {
            throw std::runtime_error("renderer produced an invalid image");
        }
        // A spherical supersample change keeps the same physical (R, Z) bounds
        // but resizes the warped pixmap. Keep what the user is looking at by
        // re-framing the visible window to the new resolution rather than
        // refitting to the whole sector (the GeometryAware size-change refit).
        if (const auto sphericalWindow = sphericalReframe(state, display)) {
            state.view->setImage(displayImageFor(display.image),
                ImageTransformPolicy::Preserve);
            state.view->zoomToRect(*sphericalWindow);
        } else {
            // Preserve/Refit/GeometryAware from the cached-vs-incoming request
            // pair; the rationale lives with the decision in the coordinator.
            const auto& metadata = m_dataset->metadata();
            const DisplayCoordinator::RasterGeometry incomingGeometry{
                metadata.physicalDomain, metadata.dimension,
                metadata.coordinateSystem, display.request.normalDirection,
                display.sphericalDisplay};
            const auto transformPolicy
                = DisplayCoordinator::rasterTransformPolicy(
                    state.rasterGeometry, incomingGeometry);
            // Preserve Custom mode by capturing the visible physical window
            // through the old plane and re-framing it through the new one.
            // This is required when pixel density changes (issue #45), and it
            // also protects same-density sequence replacements from scene or
            // scrollbar recentering while the pixmap item is replaced.
            std::optional<QRectF> dataWindowInNewScene;
            const auto axes = displayAxes(state.normal);
            const bool ownerChanged = state.hasCachedRequest
                && state.cachedRequest.dataset != display.request.dataset;
            const bool densityChanged = DisplayCoordinator::planeDensitiesDiffer(
                *state.plane, display.slice.plane, axes);
            if (transformPolicy == ImageTransformPolicy::Preserve
                && state.view->transformMode()
                    == ImageView::TransformMode::Custom
                && (ownerChanged || densityChanged)) {
                dataWindowInNewScene = preservedDataWindow(
                    state, display.slice.plane);
            }
            const auto image = displayImageFor(display.image);
            // A view on a virtual canvas keeps it: the raster lands at its
            // cell offset while the scroll position stays put.
            std::optional<ImageView::VirtualPlacement> placement;
            if (state.view->virtualCanvasActive() && !displayIsSpherical()) {
                placement = virtualPlacementFor(
                    state, display.slice.plane.physicalRegion);
            }
            state.view->setImage(image, transformPolicy,
                logicalImageSize(state, display.slice.plane, image),
                placement);
            if (dataWindowInNewScene) {
                state.view->zoomToRect(*dataWindowInNewScene);
            }
            state.rasterGeometry = incomingGeometry;
        }
    }
    // Fresh immutable snapshots: replace the pointers, never mutate the
    // pointees a cached-planes refresh worker may still be reading.
    state.plane
        = std::make_shared<const ScalarPlane>(std::move(display.slice.plane));
    // Spherical warps the raster into physical (R, Z); overlays and the probe
    // map through displayRegion, which for every other system is just the
    // plane's logical bounds (see PlaneMapping).
    state.coordinateSystem = display.coordinateSystem;
    state.sphericalDisplay = display.sphericalDisplay;
    state.displayRegion = display.displayRegion;
    state.contourPlane
        = std::make_shared<const ScalarPlane>(std::move(display.contourPlane));
    state.contourFinePlane = std::make_shared<const ScalarPlane>(
        std::move(display.contourFinePlane));
    state.contourFineFactor = display.contourFineFactor;
    state.contourPolylines = std::move(display.contourPolylines);
    const auto fieldName = QString::fromStdString(display.fieldName);
    state.fieldName = fieldName;
    state.displayMinimum = display.minimum;
    state.displayMaximum = display.maximum;
    state.displayLogarithmic = display.logarithmic;
    state.vectorSegments = std::move(display.vectors);
    if (display.slice.gridBoxesIncluded) {
        state.gridBoxes = std::move(display.slice.gridBoxes);
    }
    // Cache key for the re-render-from-cache path (see requestSlice).
    state.cachedRequest = display.request;
    state.hasCachedRequest = true;
    state.cachedMode = display.mode;
    state.cachedVectorUField = display.vectorUField;
    state.cachedVectorVField = display.vectorVField;
    state.cachedContourCount = display.contourCount;
    if (m_activeView == &state) {
        // Tracks the active view; if log was requested but fell back to linear,
        // the checkbox reflects that log did not apply.
        syncActiveViewColorControls(state);
    }
    // The straight-line profile tool works on the logical r-theta / theta-r
    // grid but not on the warped R-Z view.
    state.view->setLineToolEnabled(!displayIsSphericalWarp());
    // The 2-D Spherical menu (and, within it, Supersampling only in R-Z mode)
    // is available only for spherical datasets.
    updateSphericalControls();
    if (m_viewDimension == 2) {
        // The 2-D view carries no axis indicator normally; spherical labels its
        // horizontal/vertical axes per display mode (R-Z, r-theta, or theta-r).
        if (displayIsSpherical()) {
            const auto labels = sphericalAxisLabels(state.sphericalDisplay);
            state.view->setAxisIndicator(labels[0], labels[1]);
        } else {
            state.view->setAxisIndicator(QString(), QString());
        }
    }
    updateGridBoxes(state);
    updateOverlay(state);
    updateParticleOverlay(state);
    // This view's region may have changed; refresh every view's guides.
    updateCrosshairs();

    // setImage demotes Custom to Fit when the coordinator returns Refit -- a
    // spherical r-theta to R-Z switch does it -- so the raster funnel has to
    // restate the scale too, not only the sequence path that calls this in a
    // loop.
    refreshScaleReport();

    m_lastBlocksRead = display.slice.metrics.blocksRead;
    m_lastCacheHits = display.slice.metrics.cacheHits;
    m_lastPayloadBytesRead = display.slice.metrics.payloadBytesRead;
    statusBar()->clearMessage();
}

void MainWindow::syncVisibleRanges()
{
    if (m_viewDimension != 3 || !m_dataset) {
        return;
    }
    const auto rangeMode = static_cast<RangeMode>(
        m_rangeMode->currentData().toInt());
    if (rangeMode != RangeMode::Visible) {
        return;
    }
    if (m_visibleSyncInFlight) {
        // Coalesce: rerun with fresh state once the in-flight worker lands
        // instead of stacking a worker per arrival.
        m_visibleSyncRerun = true;
        return;
    }

    // The coordinator resolves the shared range (the cached full-domain
    // range when current, so the color bar stays stable during zoom and pan;
    // else the union of the panels' finite extrema) and produces every
    // panel's raster and contours realigned to it. Only the cheap cached-
    // range lookup runs here — the coordinator stays confined to the GUI
    // thread; the heavy half (extrema scans, contour re-extraction, up to
    // three 16 Mpx renders, and the QImage flips) runs on a worker over the
    // panels' immutable plane snapshots.
    const FieldId currentField{m_fieldSelector->currentData().toUInt()};
    const auto rawLevel = m_levelSelector->currentData().toInt();
    const auto [composition, maximumLevel] = decodeLevelData(
        rawLevel, m_dataset->metadata().finestLevel);
    const auto cachedRange = m_displayCoordinator.cachedFullDomainRange(
        {m_dataset->id(), currentField, maximumLevel, composition});

    struct PanelSnapshot {
        std::shared_ptr<const ScalarPlane> plane;
        std::shared_ptr<const ScalarPlane> contourFinePlane;
        int contourFineFactor = 1;
        std::array<int, 2> outputSize{0, 0};
    };
    std::array<PlaneViewState*, 3> views{
        &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
    std::array<PanelSnapshot, 3> snapshots;
    for (std::size_t index = 0; index < views.size(); ++index) {
        const auto* state = views[index];
        snapshots[index] = {state->plane, state->contourFinePlane,
            state->contourFineFactor, state->cachedRequest.outputSize};
    }

    struct SyncOutcome {
        std::optional<DisplayCoordinator::SharedRangeSync> sync;
        std::array<QImage, 3> images;   // display-ready (flipped) rasters
    };

    m_visibleSyncInFlight = true;
    // The sync participates in the interactive batch: the settled signal
    // (which the smoke tests use to read synchronized state) must not fire
    // until its results are applied.
    ++m_activeRequests;
    const auto generation = m_generation;
    auto* watcher = new QFutureWatcher<SyncOutcome>(this);
    connect(watcher, &QFutureWatcher<SyncOutcome>::finished, this,
        [this, watcher, generation, snapshots, views] {
            m_visibleSyncInFlight = false;
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            auto outcome = watcher->future().takeResult();
            const auto nowMode = static_cast<RangeMode>(
                m_rangeMode->currentData().toInt());
            const bool current = generation == m_generation
                && m_viewDimension == 3 && m_dataset
                && nowMode == RangeMode::Visible;
            if (current && outcome.sync) {
                const auto [globalMin, globalMax] = outcome.sync->range;
                bool activeApplied = false;
                for (std::size_t index = 0; index < views.size(); ++index) {
                    auto* state = views[index];
                    auto& update = outcome.sync->panels[index];
                    // Apply only while the snapshot this update was computed
                    // from is still the displayed plane (pointer identity);
                    // a superseded panel's own arrival re-syncs.
                    if (!update.applies
                        || state->plane != snapshots[index].plane) {
                        continue;
                    }
                    state->displayMinimum = globalMin;
                    state->displayMaximum = globalMax;
                    // One shared log flag across the panel set (see
                    // shared-log-range-render-throw-fails-load): keep every
                    // panel's stored flag, and thus the color bar below, in
                    // agreement with the raster the sync just rendered.
                    state->displayLogarithmic = outcome.sync->logarithmic;
                    if (update.contoursRecomputed) {
                        state->contourPolylines
                            = std::move(update.contourPolylines);
                    }
                    if (!outcome.images[index].isNull()) {
                        // Same plane, re-colored: a virtual canvas keeps its
                        // placement so the scroll position stays put.
                        std::optional<ImageView::VirtualPlacement> placement;
                        if (state->view->virtualCanvasActive()
                            && !displayIsSpherical()) {
                            placement = virtualPlacementFor(
                                *state, state->plane->physicalRegion);
                        }
                        state->view->setImage(outcome.images[index],
                            ImageTransformPolicy::GeometryAware,
                            logicalImageSize(*state, *state->plane,
                                outcome.images[index]),
                            placement);
                        // setImage clears the scene overlays; restore them.
                        updateGridBoxes(*state);
                        updateOverlay(*state);
                        updateParticleOverlay(*state);
                    }
                    activeApplied = activeApplied || state == m_activeView;
                }
                if (activeApplied && m_activeView->plane->width > 0) {
                    const auto fieldName = m_fieldSelector->currentText();
                    const auto label = m_activeView->displayLogarithmic
                        ? fieldName + tr(" (log)") : fieldName;
                    m_colorBar->setLogarithmic(
                        m_activeView->displayLogarithmic);
                    m_colorBar->setFieldRange(label, globalMin, globalMax);
                    const QSignalBlocker minBlocker(m_rangeMinimum);
                    const QSignalBlocker maxBlocker(m_rangeMaximum);
                    m_rangeMinimum->setValue(globalMin);
                    m_rangeMaximum->setValue(globalMax);
                }
                // The deferred full-domain range store (see the slice-arrival
                // completion): the union is only known here. When a rerun is
                // queued the union is about to be recomputed with newer
                // planes — leave the store for the rerun's completion.
                if (m_pendingRangeStore && !m_visibleSyncRerun) {
                    // Only store if the pending key still describes the current
                    // (dataset, field, level, composition): a full-domain
                    // arrival's key can outlive its own sync (e.g. its
                    // syncVisibleRanges early-returned because the mode was
                    // File), and this sync's union is for the *current* field,
                    // so storing it under the stale key would poison that
                    // field's cached range (range-cache-staleness-races).
                    const FieldId liveField{
                        m_fieldSelector->currentData().toUInt()};
                    const auto [liveComposition, liveMaximumLevel] =
                        decodeLevelData(m_levelSelector->currentData().toInt(),
                            m_dataset->metadata().finestLevel);
                    const DisplayCoordinator::RangeKey liveKey{
                        m_dataset->id(), liveField, liveMaximumLevel,
                        liveComposition};
                    if (*m_pendingRangeStore == liveKey) {
                        m_displayCoordinator.storeFullDomainRange(
                            *m_pendingRangeStore, outcome.sync->range);
                    }
                    m_pendingRangeStore.reset();
                }
            } else if (generation != m_generation) {
                m_pendingRangeStore.reset();
                m_visibleSyncRerun = false;
            }
            updateDiagnostics();
            watcher->deleteLater();
            if (m_visibleSyncRerun) {
                m_visibleSyncRerun = false;
                syncVisibleRanges();
            }
            if (m_activeRequests == 0) {
                emit interactiveSlicesSettled();
            }
        });
    watcher->setFuture(QtConcurrent::run([cachedRange, snapshots,
        logarithmic = m_logarithmic->isChecked(),
        contourMode = isContourMode(m_displayMode),
        contourCount = m_contourCount, palette = m_palette] {
        std::array<DisplayCoordinator::PanelSyncInput, 3> inputs;
        for (std::size_t index = 0; index < snapshots.size(); ++index) {
            const auto& snapshot = snapshots[index];
            inputs[index] = {snapshot.plane.get(),
                snapshot.contourFinePlane.get(), snapshot.contourFineFactor,
                snapshot.outputSize};
        }
        SyncOutcome outcome;
        outcome.sync = DisplayCoordinator::renderPanelsToSharedRange(
            cachedRange, inputs, logarithmic, contourMode, contourCount,
            palette);
        if (outcome.sync) {
            for (std::size_t index = 0; index < inputs.size(); ++index) {
                const auto& image = outcome.sync->panels[index].image;
                if (image.valid() && image.width > 0) {
                    outcome.images[index] = displayImageFor(image);
                }
            }
        }
        return outcome;
    }));
}

void MainWindow::choosePlotfileSequence()
{
    const auto settings = makeSettings();
    // Select the plotfile directories directly with click / Ctrl-click /
    // Shift-click. QFileDialog::Directory only permits selecting more than one
    // directory on the non-native dialog, so disable the native one and force
    // extended selection on every file-list view (both the icon/list view and
    // the detail/tree view). The selected directories are validated as AMReX
    // plotfiles (Header + Level_N) by openSequence.
    QFileDialog dialog(this,
        tr("Open Plotfile Sequence — select two or more plotfile directories"),
        settings.value(QStringLiteral("lastOpenDirectory")).toString());
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    for (auto* view : dialog.findChildren<QListView*>()) {
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    }
    for (auto* view : dialog.findChildren<QTreeView*>()) {
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    }
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto selected = dialog.selectedFiles();
    if (selected.isEmpty()) {
        return;
    }
    std::vector<std::filesystem::path> frames;
    frames.reserve(static_cast<std::size_t>(selected.size()));
    for (const auto& directory : selected) {
        frames.push_back(std::filesystem::path(directory.toStdString()));
    }
    auto writableSettings = makeSettings();
    writableSettings.setValue(QStringLiteral("lastOpenDirectory"),
        QFileInfo(selected.first()).absolutePath());
    openSequence(frames);
}

void MainWindow::openSequence(const std::vector<std::filesystem::path>& frames)
{
    auto sorted = frames;
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.filename() < rhs.filename();
        });
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    const auto valid = std::all_of(sorted.begin(), sorted.end(),
        [](const auto& frame) { return isAmrexPlotfile(frame); });
    if (sorted.size() < 2 || !valid) {
        emit sequenceFrameFailed();
        QMessageBox::warning(this, tr("Cannot open sequence"),
            tr("Select two or more plotfile directories, each containing a "
               "Header."));
        return;
    }

    prepareSequence(sorted.size());
    m_sequenceController->open(std::move(sorted));
}

void MainWindow::prepareSequence(std::size_t frameCount)
{
    // A sequence replaces any standalone FAB/MultiFab with plotfile frames.
    // Both local and remote entry points bypass openDatasetImpl, so establish
    // their complete state transition in one place after validation.
    setPlaybackMode(PlaybackMode::None);
    closeSequence();
    resetRangeState();
    resetFabState();
    m_particleStopSource.request_stop();
    m_particleSamples.clear();
    m_selectedParticleSpecies.clear();
    m_particleSelectionInitialized = false;
    m_particleLoading = false;
    m_particleProgress->setVisible(false);
    ++m_particleGeneration;
    m_remoteSequenceConnectionGeneration = 0;

    m_animationPanel->setSequenceFrameCount(static_cast<int>(frameCount));
    m_animationPanel->setSequenceVisible(true);
    updateAnimationDockVisibility();
    // Line plot curves are snapshots of the previous dataset; drop the window.
    auto* linePlotWindow = m_linePlotWindow;
    m_linePlotWindow = nullptr;
    if (linePlotWindow != nullptr) {
        linePlotWindow->close();
    }
    // Both overlay dialogs describe the outgoing dataset -- the particles one
    // lists its species, whose selection state was just reset above, and the
    // contours one lists its fields. openDatasetImpl closes them for a plain
    // open; this path never runs that, so a sequence open would otherwise leave
    // them on screen writing the old dataset's names back on Apply. Frame steps
    // do not come through here, so both survive stepping, as intended.
    auto* particlesDialog = m_particlesDialog;
    m_particlesDialog = nullptr;
    if (particlesDialog != nullptr) {
        particlesDialog->close();
    }
    auto* contoursDialog = m_contoursDialog;
    m_contoursDialog = nullptr;
    if (contoursDialog != nullptr) {
        contoursDialog->close();
    }
}

void MainWindow::openRemoteSequence(std::string host, std::uint16_t port,
    const std::vector<std::string>& remotePaths, std::string token)
{
    if (remotePaths.size() < 2
        || std::any_of(remotePaths.begin(), remotePaths.end(),
            [](const auto& path) { return path.empty(); })) {
        emit sequenceFrameFailed();
        QMessageBox::warning(this, tr("Cannot open remote sequence"),
            tr("Enter two or more plotfile paths as they appear on %1:%2. "
               "Remote frames are named by their path on the server, not "
               "chosen from a local file dialog.")
                .arg(QString::fromStdString(host))
                .arg(port));
        return;
    }

    m_remoteHost = host;
    m_remotePort = port;
    m_remoteToken = token;
    prepareSequence(remotePaths.size());
    m_remoteSequence = true;

    std::vector<std::filesystem::path> frames;
    frames.reserve(remotePaths.size());
    for (const auto& path : remotePaths) {
        frames.emplace_back(path);
    }
    struct SharedRemoteConnection {
        std::mutex mutex;
        std::shared_ptr<remote::Connection> connection;
        std::uint64_t generation = 0;
    };
    auto shared = std::make_shared<SharedRemoteConnection>();
    auto loader = [shared, host = std::move(host), port,
                      token = std::move(token)](
                      const std::filesystem::path& path, DatasetId,
                      const FrameSliceSpec& spec, StopToken cancellation) {
        std::shared_ptr<remote::Connection> connection;
        std::uint64_t connectionGeneration = 0;
        {
            std::scoped_lock lock(shared->mutex);
            connection = shared->connection;
            connectionGeneration = shared->generation;
        }
        if (!connection || !connection->connected()) {
            // Connect outside the shared-state mutex. Foreground loads and
            // prefetches may overlap, and neither should inherit the other's
            // network wait or lose its own cancellation deadline.
            auto candidate = std::make_shared<remote::Connection>(host, port,
                remote::ConnectionOptions{
                    .clientName = "AMReXplorer Qt sequence",
                    .softwareVersion = kVersion,
                    .sessionToken = token},
                cancellation);
            {
                std::scoped_lock lock(shared->mutex);
                if (!shared->connection || !shared->connection->connected()) {
                    shared->connection = std::move(candidate);
                    ++shared->generation;
                }
                connection = shared->connection;
                connectionGeneration = shared->generation;
            }
            // If another load won the connection race, release this redundant
            // connection only after dropping the shared-state mutex.
            candidate.reset();
        }
        auto session = remote::RemoteDatasetSession::open(
            std::move(connection), path.string(), initialCacheBudget(),
            cancellation);
        auto result = executeSessionFrameLoad(
            std::move(session), spec, cancellation);
        result.connectionGeneration = connectionGeneration;
        return result;
    };
    m_sequenceController->open(std::move(frames), std::move(loader));
}

void MainWindow::closeSequence()
{
    if (m_playbackMode == PlaybackMode::Sequence) {
        setPlaybackMode(PlaybackMode::None);
    }
    m_sequenceController->close();
    m_remoteSequence = false;
    m_animationPanel->setSequenceVisible(false);
    updateAnimationDockVisibility();
}

void MainWindow::updateAnimationDockVisibility()
{
    // The Animation panel hosts the 3-D slice-sweep controls and the
    // plotfile-sequence controls. Keep it visible only when one of those
    // applies; otherwise it is dead space.
    const auto sequenceActive = m_sequenceController->hasSequence();
    const auto threeD = m_dataset != nullptr
        && m_dataset->metadata().dimension == 3;
    const auto applies = sequenceActive || threeD;
    // Only act on a transition. This runs again for every sequence frame, by
    // way of configureSequenceControls, and forcing the dock visible there
    // reopened it after the user hid it mid-playback. Whether the panel applies
    // at all is ours to decide; whether it is shown while it applies is theirs.
    //
    // An empty panel is hidden unconditionally, never on a transition. Both
    // control groups are hidden when neither reason holds, so an edge trigger
    // parked dead space for the session in the false -> false direction: open
    // the panel from the View menu with no dataset (or after a failed open),
    // then open a 2-D plotfile, and nothing moved the flags, so an empty dock
    // stayed. Deciding whether the panel applies at all is ours.
    if (!applies) {
        m_animationDockSequence = false;
        m_animationDockThreeD = false;
        m_animationDock->setVisible(false);
        return;
    }
    // While it does apply, only a change in *why* re-asserts it. Testing one
    // "applies" flag missed the true -> true direction: open a 3-D plotfile,
    // hide the dock, then open a plotfile sequence, and neither the close nor
    // the first frame is a transition, so the transport arrived in a dock
    // nothing would reopen. Hiding the sweep controls is not a standing refusal
    // of the transport that replaces them -- but it is one of the sweep
    // controls themselves, which is why this stays an edge trigger.
    if (sequenceActive == m_animationDockSequence
        && threeD == m_animationDockThreeD) {
        return;
    }
    m_animationDockSequence = sequenceActive;
    m_animationDockThreeD = threeD;
    m_animationDock->setVisible(true);
}

void MainWindow::stepSequence(int direction)
{
    m_sequenceController->step(direction);
}

void MainWindow::goToSequenceFrame(int index, bool forceRestart)
{
    m_sequenceController->goToFrame(index, forceRestart);
}

void MainWindow::displayFrameResult(InitialSliceResult& result,
    bool defaultPositions)
{
    if (result.connectionGeneration != 0
        && result.connectionGeneration
            != m_remoteSequenceConnectionGeneration) {
        // DatasetId is allocated by the server and can restart at one after a
        // reconnect. Drop every dataset-scoped display range before publishing
        // a frame from a new connection generation so an old ID cannot alias.
        m_displayCoordinator.invalidateRangeCache();
        m_pendingRangeStore.reset();
        m_remoteSequenceConnectionGeneration = result.connectionGeneration;
    }
    m_dataset = result.dataset;
    m_particleSamples = std::move(result.particles);
    configureParticleControls(true);
    const auto& metadata = m_dataset->metadata();
    m_viewDimension = metadata.dimension;

    // Refresh the metadata dock and the window title (frame name + time).
    PlotfileMetadataResult frameMetadata;
    frameMetadata.metadata = std::make_shared<DatasetMetadata>(metadata);
    frameMetadata.metrics = result.dataset->metadataReadMetrics();
    frameMetadata.fileVersion = !result.fileVersion.empty()
        ? result.fileVersion : m_fileVersion;
    showMetadata(frameMetadata, m_datasetPath);

    configureSequenceControls(defaultPositions);
    if (selectCacheFallbackLevel(m_levelSelector, result.cacheFallbackToLevel)) {
        configureSlicePositionControls();
        updateRangeModeAvailability();
        syncMenuChecks();
    }
    const auto views = currentViews();
    if (result.displays.size() != views.size()) {
        throw std::runtime_error("frame slice count does not match the view set");
    }
    for (std::size_t index = 0; index < views.size(); ++index) {
        showSlice(*views[index], std::move(result.displays[index]));
    }
    const auto cache = m_dataset->cacheMetrics();
    m_cacheBudgetBytes = cache.budgetBytes;
    m_cacheResidentBytes = cache.residentBytes;
    m_cachePinnedBytes = cache.pinnedBytes;
    m_cacheEvictions = cache.evictions;
    validateVectorMode();
    // Frames need not share a domain, and the clamped scale report is computed
    // from one. A scale picked on an earlier frame otherwise kept that frame's
    // number.
    refreshScaleReport();
    if (result.cacheFallbackToLevel >= 0) {
        statusBar()->showMessage(cacheFallbackMessage(
            *result.dataset, result.cacheFallbackFromLevel,
            result.cacheFallbackToLevel));
    }
}

void MainWindow::configureSequenceControls(bool defaultPositions)
{
    if (!m_dataset) {
        return;
    }
    const auto& metadata = m_dataset->metadata();
    // Preserve the user's selections across frames: the field index if it
    // still exists, the level by its combo data (falling back to finest
    // available when this frame has fewer levels).
    const auto previousField = m_controlsReady && m_fieldSelector->count() > 0
        ? m_fieldSelector->currentIndex() : 0;
    const auto previousLevel = m_controlsReady
        && m_levelSelector->currentIndex() >= 0
            ? m_levelSelector->currentData().toInt() : -1;
    {
        const QSignalBlocker fieldBlocker(m_fieldSelector);
        const QSignalBlocker levelBlocker(m_levelSelector);
        m_fieldSelector->clear();
        for (std::size_t field = 0; field < metadata.fields.size(); ++field) {
            m_fieldSelector->addItem(
                QString::fromStdString(metadata.fields[field].name),
                static_cast<unsigned int>(field));
        }
        m_fieldSelector->setCurrentIndex(
            std::clamp(previousField, 0, m_fieldSelector->count() - 1));
        m_levelSelector->clear();
        populateLevelCombo(m_levelSelector, metadata.finestLevel);
        const auto levelIndex = m_levelSelector->findData(previousLevel);
        m_levelSelector->setCurrentIndex(levelIndex >= 0 ? levelIndex : 0);
    }

    // 3-D keeps the user's slice positions (clamped into the new domain);
    // the first 3-D frame of a session starts at the domain midpoints.
    const auto isThreeDimensional = metadata.dimension == 3;
    m_syncRubberBandZoomAction->setVisible(isThreeDimensional);
    if (isThreeDimensional) {
        const auto domain = datasetSampleBounds(metadata);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            m_slicePosition3d[axis] = defaultPositions
                ? domain.lower[axis]
                    + 0.5 * (domain.upper[axis] - domain.lower[axis])
                : std::clamp(m_slicePosition3d[axis], domain.lower[axis],
                    std::nextafter(domain.upper[axis], domain.lower[axis]));
        }
        m_isoWidget->setGeometry(metadata);
        m_isoWidget->setSlicePositions(m_slicePosition3d[0], m_slicePosition3d[1],
            m_slicePosition3d[2]);
    }
    m_stack->setCurrentIndex(isThreeDimensional ? 1 : 0);
    m_animationPanel->setSweepVisible(isThreeDimensional);
    updateAnimationDockVisibility();
    configureSlicePositionControls();

    // The active view must belong to the new dimension's view set. This fires
    // on the transition into a sequence, not per frame, so it is also where
    // the sequence takes focus for the arrow-key pan: opening a sequence
    // bypasses requestInitialSlice entirely, and without this the keys stayed
    // dead until the user clicked a panel.
    const auto views = currentViews();
    if (std::find(views.begin(), views.end(), m_activeView) == views.end()) {
        setActiveView(isThreeDimensional ? m_planeViews[2] : m_view2d);
        focusActiveViewForPanning();
    }

    enableDatasetControls(metadata);
    m_exportAnimationAction->setEnabled(true);
    rebuildVariableMenu();
    ensureVectorFieldDefaults();
    updateRangeModeAvailability();
}

void MainWindow::commitFieldRange(std::uint32_t field)
{
    FieldRange range;
    range.mode = static_cast<RangeMode>(m_rangeMode->currentData().toInt());
    if (range.mode == RangeMode::User) {
        range.userRange = std::pair{m_rangeMinimum->value(), m_rangeMaximum->value()};
    }
    m_fieldRanges[field] = std::move(range);
}

void MainWindow::applyFieldRange(std::uint32_t field)
{
    const auto it = m_fieldRanges.find(field);
    const auto range = (it != m_fieldRanges.end()) ? it->second : FieldRange{};
    {
        const QSignalBlocker modeBlocker(m_rangeMode);
        const QSignalBlocker minBlocker(m_rangeMinimum);
        const QSignalBlocker maxBlocker(m_rangeMaximum);
        m_rangeMode->setCurrentIndex(
            m_rangeMode->findData(static_cast<int>(range.mode)));
        if (range.userRange.has_value()) {
            m_rangeMinimum->setValue(range.userRange->first);
            m_rangeMaximum->setValue(range.userRange->second);
        }
    }
    const auto isUser = range.mode == RangeMode::User;
    m_rangeMinimum->setEnabled(isUser && m_controlsReady);
    m_rangeMaximum->setEnabled(isUser && m_controlsReady);
}

void MainWindow::resetRangeState()
{
    m_fieldRanges.clear();
    m_trackedField = 0;
    m_displayCoordinator.invalidateRangeCache();
    m_pendingRangeStore.reset();
    const QSignalBlocker modeBlocker(m_rangeMode);
    const QSignalBlocker minBlocker(m_rangeMinimum);
    const QSignalBlocker maxBlocker(m_rangeMaximum);
    m_rangeMode->setCurrentIndex(
        m_rangeMode->findData(static_cast<int>(RangeMode::File)));
    m_rangeMinimum->setValue(0.0);
    m_rangeMaximum->setValue(1.0);
    m_rangeMinimum->setEnabled(false);
    m_rangeMaximum->setEnabled(false);
}

void MainWindow::updateRangeModeAvailability()
{
    if (!m_dataset || m_fieldSelector->currentIndex() < 0
        || m_levelSelector->currentIndex() < 0) {
        return;
    }

    const auto& metadata = m_dataset->metadata();
    const FieldId field{m_fieldSelector->currentData().toUInt()};
    const auto [composition, maximumLevel] = decodeLevelData(
        m_levelSelector->currentData().toInt(), metadata.finestLevel);
    const auto fileAvailable = m_dataset->rangeAvailable(
        RangeRequest{field, maximumLevel, composition, RangeScope::File});
    const auto levelAvailable = m_dataset->rangeAvailable(
        RangeRequest{field, maximumLevel, composition, RangeScope::Level});

    auto* model = qobject_cast<QStandardItemModel*>(m_rangeMode->model());
    if (model == nullptr) {
        return;
    }
    const auto unavailableText = tr(
        "Unavailable because this data does not provide complete range statistics.");
    const auto setAvailable = [&](RangeMode mode, bool available) {
        const auto index = m_rangeMode->findData(static_cast<int>(mode));
        if (index < 0) {
            return;
        }
        if (auto* item = model->item(index)) {
            item->setEnabled(available);
            item->setToolTip(available ? QString() : unavailableText);
        }
    };
    setAvailable(RangeMode::File, fileAvailable);
    setAvailable(RangeMode::Level, levelAvailable);

    const auto current = static_cast<RangeMode>(
        m_rangeMode->currentData().toInt());
    const auto currentAvailable =
        (current != RangeMode::File || fileAvailable)
        && (current != RangeMode::Level || levelAvailable);
    if (currentAvailable) {
        return;
    }

    {
        const QSignalBlocker blocker(m_rangeMode);
        m_rangeMode->setCurrentIndex(
            m_rangeMode->findData(static_cast<int>(RangeMode::Visible)));
    }
    m_rangeMinimum->setEnabled(false);
    m_rangeMaximum->setEnabled(false);
    auto& fieldRange = m_fieldRanges[field.value];
    fieldRange.mode = RangeMode::Visible;
    statusBar()->showMessage(
        tr("Metadata range unavailable; using the visible-data range."));
}

FrameSliceSpec MainWindow::buildFrameSpec()
{
    FrameSliceSpec spec;
    spec.displayMode = m_displayMode;
    spec.palette = m_palette;
    spec.contourCount = m_contourCount;
    spec.sphericalSupersample = m_sphericalSupersample;
    spec.sphericalDisplay = m_sphericalDisplay;
    spec.logarithmic = m_logarithmic->isChecked();
    spec.rangeMode = static_cast<RangeMode>(m_rangeMode->currentData().toInt());
    if (spec.rangeMode == RangeMode::User) {
        spec.userRange = std::pair{m_rangeMinimum->value(),
            m_rangeMaximum->value()};
    }
    spec.field = m_controlsReady && m_fieldSelector->currentIndex() >= 0
        ? m_fieldSelector->currentData().toUInt() : 0U;
    spec.levelSelection = m_controlsReady && m_levelSelector->currentIndex() >= 0
        ? m_levelSelector->currentData().toInt() : -1;
    spec.vectorUField = static_cast<std::uint32_t>(std::max(m_vectorUField, 0));
    spec.vectorVField = static_cast<std::uint32_t>(std::max(m_vectorVField, 0));
    spec.vectorWField = static_cast<std::uint32_t>(std::max(m_vectorWField, 0));
    // Slice positions only carry over between 3-D frames; anything else
    // starts the new dataset at its domain midpoints.
    spec.defaultPositions = m_viewDimension != 3;
    spec.slicePositions = m_slicePosition3d;
    spec.particleSelectionInitialized = m_particleSelectionInitialized;
    if (m_particleSelectionInitialized) {
        spec.particleSpecies = m_selectedParticleSpecies;
    }
    spec.particleFraction = m_particleFraction;
    spec.particleSeed = m_particleSeed;
    spec.includeGridBoxes = m_boxesAction->isChecked();
    const auto views = currentViews();
    spec.visibleRegions.reserve(views.size());
    if (m_remoteSequence) {
        spec.outputSizesAreViewportBounds = true;
        spec.outputSizes.reserve(views.size());
    }
    for (const auto* state : views) {
        spec.visibleRegions.push_back(state->visibleRegion);
        if (m_remoteSequence) {
            spec.outputSizes.push_back(viewportPixelSize(*state));
        }
    }
    return spec;
}

void MainWindow::stepSweep(int direction)
{
    if (!m_dataset || m_dataset->metadata().dimension != 3) {
        return;
    }
    const auto axis = m_animationPanel->sweepAxis();
    const auto index = static_cast<std::size_t>(axis);
    const auto& metadata = m_dataset->metadata();
    const auto& level = metadata.levels.back();
    auto sample = sampleIndex(level, axis, m_slicePosition3d[index]) + direction;
    if (sample > level.domain.upper[index]) {
        sample = level.domain.lower[index];
    } else if (sample < level.domain.lower[index]) {
        sample = level.domain.upper[index];
    }
    setSlicePosition(axis, samplePosition(level, axis, sample));
}

void MainWindow::toggleSweepPlayback()
{
    if (m_playbackMode == PlaybackMode::Sweep) {
        setPlaybackMode(PlaybackMode::None);
        return;
    }
    if (!m_dataset || m_dataset->metadata().dimension != 3) {
        return;
    }
    setPlaybackMode(PlaybackMode::Sweep);
}

void MainWindow::toggleSequencePlayback()
{
    if (m_playbackMode == PlaybackMode::Sequence) {
        setPlaybackMode(PlaybackMode::None);
        return;
    }
    if (m_sequenceController->frameCount() < 2) {
        return;
    }
    setPlaybackMode(PlaybackMode::Sequence);
}

void MainWindow::setPlaybackMode(PlaybackMode mode)
{
    m_playbackMode = mode;
    m_animationPanel->setSweepPlaying(mode == PlaybackMode::Sweep);
    m_animationPanel->setSequencePlaying(mode == PlaybackMode::Sequence);
    if (mode == PlaybackMode::None) {
        m_playbackTimer->stop();
    } else {
        m_playbackTimer->start(m_animationPanel->frameDelayMs());
    }
}

void MainWindow::playbackTick()
{
    if (m_playbackMode == PlaybackMode::Sweep) {
        if (!m_dataset || m_dataset->metadata().dimension != 3) {
            setPlaybackMode(PlaybackMode::None);
            return;
        }
        // Skip the tick while the previous slice is still on a worker, so a
        // fast Speed setting cannot pile up requests.
        const auto axis = m_animationPanel->sweepAxis();
        if (m_planeViews[static_cast<std::size_t>(axis)].pendingRequests > 0) {
            return;
        }
        stepSweep(1);
        // Bypass the debounce so each tick issues its slice immediately; the
        // in-flight check above is the throttle.
        flushSliceRequests();
        return;
    }
    if (m_playbackMode == PlaybackMode::Sequence) {
        if (m_sequenceController->frameCount() < 2) {
            setPlaybackMode(PlaybackMode::None);
            return;
        }
        // Skip the tick while the previous frame is still loading.
        if (m_sequenceController->inFlight()) {
            return;
        }
        m_sequenceController->step(1);
    }
}

void MainWindow::applySpeed()
{
    m_playbackTimer->setInterval(m_animationPanel->frameDelayMs());
}

void MainWindow::reportBackgroundError(const QString& message)
{
    // Non-modal: background-operation failures append to the Diagnostics dock
    // and set a status-bar message instead of a modal dialog that disables the
    // window. Suppressed while closing (stage 1 also guards the handlers).
    if (m_closing) {
        return;
    }
    qWarning("%s", message.toUtf8().constData());
    m_backgroundErrors.append(message);
    constexpr int maximumErrors = 50;
    while (m_backgroundErrors.size() > maximumErrors) {
        m_backgroundErrors.removeFirst();
    }
    statusBar()->showMessage(message.section(QLatin1Char('\n'), 0, 0));
    m_diagnosticsDock->setVisible(true);
    updateDiagnostics();
}

void MainWindow::updateDiagnostics()
{
    auto text = tr("generation: %1\nactive background requests: %2\n"
           "stale results discarded: %3\nmetadata files read: %4\n"
           "metadata bytes read: %5\nblocks read: %6\ncache hits: %7\n"
           "payload bytes read: %8\ncache budget bytes: %9\n"
           "cache resident bytes: %10\ncache pinned bytes: %11\n"
           "cache evictions: %12\nlast frame switch: %13 ms")
            .arg(m_generation)
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
            .arg(m_sequenceController->lastFrameSwitchMs());
    if (m_remotePort != 0) {
        text += tr("\nremote endpoint: %1:%2")
                    .arg(QString::fromStdString(m_remoteHost))
                    .arg(m_remotePort);
        if (auto remoteSession = std::dynamic_pointer_cast<
                remote::RemoteDatasetSession>(m_dataset)) {
            text += tr("\nremote status: %1\nremote path: %2")
                        .arg(remoteSession->connection()->connected()
                                ? tr("connected") : tr("disconnected"),
                            QString::fromStdString(
                                remoteSession->remotePath()));
        } else {
            text += tr("\nremote status: configured");
        }
    }
    for (const auto& line : m_probeLines) {
        text += QLatin1Char('\n');
        text += line;
    }
    for (const auto& error : m_backgroundErrors) {
        text += QLatin1Char('\n');
        text += tr("background error: %1").arg(error);
    }
    m_diagnostics->setPlainText(text);
}

} // namespace amrvis::qt
