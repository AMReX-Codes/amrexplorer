#include "MainWindowInternal.hpp"

// Companion: a second local 3-D plotfile shown beside the primary in the same
// window. The two must share a plane -- their domains touch along one axis
// and overlap along the other two (PairGeometry) -- and each keeps its own
// session, field and level selection, range and colour bar (DatasetLayer).
// In the two panels that show the perpendicular axis both rasters are drawn
// as tiles stacked at their physical positions; the panel normal to it shows
// whichever layer holds the slice position. The companion is a secondary load
// onto an installed primary, never part of a dataset open.

namespace amrvis::qt {

namespace {

QRectF toQRectF(const SceneRect& rect)
{
    return QRectF(rect.x, rect.y, rect.width, rect.height);
}

} // namespace

void MainWindow::chooseCompanion()
{
    if (!primary().session) {
        reportBackgroundError(tr("Open a plotfile first."));
        return;
    }
    const auto settings = makeSettings();
    const auto directory = QFileDialog::getExistingDirectory(
        this, tr("Open companion plotfile"),
        settings.value(QStringLiteral("lastOpenDirectory")).toString());
    if (directory.isEmpty()) {
        return;
    }
    auto path = std::filesystem::path(directory.toStdString());
    for (auto candidate = path; !candidate.empty();
        candidate = candidate.parent_path()) {
        if (isAmrexPlotfile(candidate)) {
            path = candidate;
            break;
        }
        if (candidate.parent_path() == candidate) {
            break;
        }
    }
    openCompanion(path);
}

void MainWindow::openCompanion(const std::filesystem::path& path)
{
    const auto refuse = [this](const QString& reason) {
        reportBackgroundError(tr("Cannot open companion: %1").arg(reason));
        emit companionOpenFinished(false);
    };
    if (!m_controlsReady || !primary().session || !primary().openMetadata) {
        refuse(tr("open a plotfile first"));
        return;
    }
    if (std::dynamic_pointer_cast<remote::RemoteDatasetSession>(primary().session)) {
        refuse(tr("a remote dataset cannot take a companion"));
        return;
    }
    if (m_sequenceController->hasSequence()) {
        refuse(tr("a plotfile sequence cannot take a companion"));
        return;
    }
    if (!canOpenCompanion()) {
        refuse(tr("the open dataset is not a three-dimensional plotfile"));
        return;
    }
    // Only the previous load is stopped; a companion already on show stays
    // until the new one has read and paired, so a refusal leaves it as it was.
    m_companionStopSource.request_stop();
    m_companionStopSource = StopSource{};
    const auto cancellation = m_companionStopSource.get_token();
    const auto generation = m_generation;
    const auto companionGeneration = ++m_companionGeneration;
    // The companion renders with the window's display settings but its own
    // default field and level; derived fields stay with the primary.
    FrameSliceSpec spec;
    spec.palette = m_paletteController->palette();
    spec.displayMode = m_displayMode;
    spec.includeGridBoxes = m_boxesAction->isChecked();
    spec.contourCount = m_contourCount;
    if (spec.displayMode == DisplayMode::VelocityVectors) {
        spec.displayMode = DisplayMode::Raster;  // see requestSlice
    }
    // Log is shared with the primary; the range mode starts at File.
    spec.logarithmic = primary().range->logarithmic();
    spec.slicePositions = m_slicePosition3d;
    spec.defaultPositions = false;
    const auto primaryMetadata = primary().openMetadata;
    m_diagnosticsModel->adjustActivity(1);
    statusBar()->showMessage(tr("Loading companion %1...").arg(
        QString::fromStdString(path.filename().string())));

    auto* watcher = new QFutureWatcher<CompanionLoad>(this);
    connect(watcher, &QFutureWatcher<CompanionLoad>::finished, this,
        [this, watcher, path, generation, companionGeneration, cancellation] {
            m_diagnosticsModel->adjustActivity(-1);
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                auto load = watcher->future().takeResult();
                // Stale when the primary changed or a newer companion open
                // superseded this one; the session dies with `load`.
                if (generation == m_generation
                    && companionGeneration == m_companionGeneration
                    && !cancellation.stop_requested()) {
                    installCompanion(path, std::move(load));
                } else {
                    m_diagnosticsModel->noteStaleResult();
                }
            } catch (const std::exception& error) {
                if (generation == m_generation
                    && companionGeneration == m_companionGeneration
                    && !cancellation.stop_requested()) {
                    statusBar()->clearMessage();
                    reportBackgroundError(tr("Cannot open companion: %1")
                            .arg(exceptionMessage(error)));
                    emit companionOpenFinished(false);
                } else {
                    m_diagnosticsModel->noteStaleResult();
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [path, spec = std::move(spec), cancellation, generation, companionGeneration,
            primaryMetadata]() mutable {
            CompanionLoad load;
            load.metadata = readDatasetMetadata(path, cancellation);
            auto pairing = pairGeometry(*primaryMetadata, *load.metadata.metadata);
            if (!pairing.geometry) {
                throw std::runtime_error(pairing.error);
            }
            load.geometry = *pairing.geometry;
            // One native raster per panel, as a local open makes: the
            // companion's tiles are placed by the layout, never resampled.
            const auto& metadata = *load.metadata.metadata;
            const auto bounds = datasetSampleBounds(metadata);
            spec.outputSizes.clear();
            for (int normal = 0; normal < 3; ++normal) {
                spec.outputSizes.push_back(
                    finestNativeOutputSize(metadata, bounds, normal));
            }
            // A dataset id no primary load can produce, and one no earlier
            // companion used: the high bit marks the companion, the middle
            // bits count companion opens, the low bits carry the primary
            // generation it was opened beside.
            const DatasetId id{(std::uint64_t{1} << 63)
                | (companionGeneration << 40) | (generation & ((std::uint64_t{1} << 40) - 1))};
            // The data root a plain open resolves to: the plotfile directory
            // itself (a Header path's parent otherwise).
            auto root = std::filesystem::is_directory(path) ? path : path.parent_path();
            if (root.empty()) {
                root = ".";
            }
            load.result = executeFrameLoad(path, id, spec, initialCacheBudget(),
                cancellation, load.metadata, std::move(root));
            return load;
        }));
}

void MainWindow::installCompanion(
    const std::filesystem::path& path, CompanionLoad load)
{
    if (m_layers[1].active) {
        tearDownCompanion(/*replacing=*/true);
    }
    // The Dataset window tabulates the active view's dataset at its slice
    // position; with two datasets on the panels it would mix them, so it is
    // closed as an open does (it is unavailable while a companion is shown).
    closeDatasetWindow();
    // Zoom is view-only with a companion, so the primary's rasters must cover
    // their whole domain; a rubber-band selection made before is re-sliced.
    for (auto* state : primaryViews()) {
        if (state->visibleRegion.has_value()) {
            state->visibleRegion.reset();
            state->view->setVirtualCanvas(std::nullopt);
            scheduleSliceRequest(*state);
        }
    }
    auto& layer = m_layers[1];
    layer.session = load.result.dataset;
    ++layer.sessionEpoch;
    layer.openMetadata = load.metadata.metadata;
    layer.fileVersion = load.metadata.fileVersion;
    layer.path = path;
    layer.name = datasetDisplayName(path);
    layer.active = true;
    m_pair = load.geometry;
    if (load.result.displays.size() != layer.planeViews.size()) {
        closeCompanion();
        reportBackgroundError(
            tr("Cannot open companion: slice count does not match the panels"));
        emit companionOpenFinished(false);
        return;
    }
    configureCompanionControls();
    updatePairLayouts();
    updatePairedIsoGeometry();
    // Kept from a replaced companion, the shared position may lie outside
    // the new union: back to its edge, and the primary's panel there follows.
    // The 3-D view took the position above, so a clamp is published again.
    bool positionClamped = false;
    for (int axis = 0; axis < 3; ++axis) {
        const auto a = static_cast<std::size_t>(axis);
        const auto& union_ = m_pair->unionBounds;
        const auto clamped = std::clamp(m_slicePosition3d[a], union_.lower[a],
            std::nextafter(union_.upper[a], union_.lower[a]));
        if (clamped != m_slicePosition3d[a]) {
            m_slicePosition3d[a] = clamped;
            positionClamped = true;
            scheduleSliceRequest(primary().planeViews[a]);
        }
    }
    if (positionClamped) {
        publishSlicePositions();
    }
    // The primary's tiles move from the raster-at-origin scene onto the shared
    // canvas before the companion's land beside them.
    applyPairLayouts();
    for (std::size_t index = 0; index < layer.planeViews.size(); ++index) {
        auto& state = layer.planeViews[index];
        state.visibleRegion.reset();
        state.planeSessionEpoch = layer.sessionEpoch;
        showSlice(state, std::move(load.result.displays[index]), layer.sessionEpoch);
        // Following the primary's range from the start when a replaced
        // companion did: the load rendered the file range.
        if (m_companionFollowsPrimary) {
            const auto& lead = primary().planeViews[index];
            refreshFollowingCompanion(index, lead.displayMinimum,
                lead.displayMaximum, lead.displayLogarithmic);
        }
    }
    updateShownLayers();
    updatePairedModeControls();
    configureSlicePositionControls();
    updateCrosshairs();
    updateScaleBarAvailability();
    updateScaleBars();
    refreshScaleReport();
    if (m_activeView != nullptr) {
        syncActiveViewColorControls(*m_activeView);
    }
    updateWindowTitle();
    refreshMetadataDisplay();
    m_companionToolbar->setVisible(true);
    layer.colorBar->setVisible(!m_companionFollowsPrimary);
    statusBar()->showMessage(tr("Companion %1 opened").arg(layer.name), 5000);
    emit companionOpenFinished(true);
}

void MainWindow::closeCompanion()
{
    tearDownCompanion(/*replacing=*/false);
}

void MainWindow::tearDownCompanion(bool replacing)
{
    // A load still running for a replacement must not install after this.
    ++m_companionGeneration;
    m_companionStopSource.request_stop();
    auto& layer = m_layers[1];
    if (!layer.active) {
        return;
    }
    for (auto& state : layer.planeViews) {
        state.stopSource.request_stop();
        ++state.sliceGeneration;
        state.plane = std::make_shared<const ScalarPlane>();
        state.contourPlane = std::make_shared<const ScalarPlane>();
        ++state.renderGeneration;
        state.contourPolylines.clear();
        state.fieldName.clear();
        state.visibleRegion.reset();
        state.vectorSegments.clear();
        state.gridBoxes.clear();
        state.cachedRequest = {};
        state.hasCachedRequest = false;
        if (state.view != nullptr) {
            state.view->removeTile(state.tile);
        }
    }
    m_pendingViews.erase(std::remove_if(m_pendingViews.begin(), m_pendingViews.end(),
        [&layer](const PlaneViewState* state) {
            return state->layer == 1 && &layer.planeViews[0] <= state
                && state <= &layer.planeViews[2];
        }), m_pendingViews.end());
    if (m_activeView != nullptr && m_activeView->layer == 1) {
        setActiveView(primary().planeViews[static_cast<std::size_t>(
            m_activeView->normal)]);
    }
    layer.active = false;
    layer.session.reset();
    ++layer.sessionEpoch;
    layer.openMetadata.reset();
    layer.fileVersion.clear();
    layer.path.clear();
    layer.name.clear();
    layer.perpendicularScale = 1.0;
    // A sync still on a worker keeps its in-flight flag: its completion
    // clears it and drops the outcome (the session is gone), and a companion
    // installed meanwhile queues behind it through the rerun flag rather
    // than dispatching a second worker. Only the pending rerun is dropped.
    layer.visibleSyncRerun = false;
    layer.pendingRangeStore.reset();
    m_pair.reset();
    // The range cache is keyed by dataset id; the next companion beside this
    // primary must not read this one's cached union.
    m_displayCoordinator.invalidateRangeCache();
    if (layer.range != nullptr) {
        layer.range->reset();
        layer.range->setControlsReady(false);
    }
    if (m_companionToolbar != nullptr) {
        m_companionToolbar->setVisible(false);
    }
    if (layer.colorBar != nullptr) {
        layer.colorBar->clearRange();
        layer.colorBar->setVisible(false);
    }
    if (!replacing) {
        if (m_companionFollowBox != nullptr) {
            const QSignalBlocker blocker(m_companionFollowBox);
            m_companionFollowBox->setChecked(false);
        }
        m_companionFollowsPrimary = false;
    }
    // Back to one tile per panel in the classic scene. The primary's tile is
    // shown first: hidden behind the companion's band, it would not count in
    // the rect that placing it fits.
    for (auto* state : primaryViews()) {
        state->view->setTileVisible(state->tile, true);
    }
    applyPairLayouts();
    if (m_controlsReady && primary().session) {
        const auto& metadata = primary().session->metadata();
        if (metadata.dimension == 3) {
            m_isoWidget->setGeometry(metadata);
            // The shared position may sit in the companion's part of the
            // union; back inside the primary, and that panel re-sliced. A
            // replacement keeps it for the new union (see installCompanion).
            if (!replacing) {
                const auto bounds = datasetSampleBounds(metadata);
                for (int axis = 0; axis < 3; ++axis) {
                    const auto a = static_cast<std::size_t>(axis);
                    const auto clamped = std::clamp(m_slicePosition3d[a],
                        bounds.lower[a], std::nextafter(bounds.upper[a], bounds.lower[a]));
                    if (clamped != m_slicePosition3d[a]) {
                        m_slicePosition3d[a] = clamped;
                        scheduleSliceRequest(primary().planeViews[a]);
                    }
                }
            }
            publishSlicePositions();
        }
        updatePairedModeControls();
        configureSlicePositionControls();
        applyDisplayStretches();
        updateCrosshairs();
        updateWindowTitle();
        refreshMetadataDisplay();
    }
}

void MainWindow::configureCompanionControls()
{
    auto& layer = m_layers[1];
    if (!layer.session || layer.fieldSelector == nullptr) {
        return;
    }
    const QSignalBlocker fieldBlocker(layer.fieldSelector);
    const QSignalBlocker levelBlocker(layer.levelSelector);
    const auto& metadata = layer.session->metadata();
    layer.fieldSelector->clear();
    const auto stored = layer.session->storedFieldCount();
    for (std::size_t field = 0; field < stored && field < metadata.fields.size();
        ++field) {
        layer.fieldSelector->addItem(
            QString::fromStdString(metadata.fields[field].name),
            static_cast<unsigned int>(field));
    }
    layer.fieldSelector->setCurrentIndex(0);
    populateLevelCombo(layer.levelSelector, metadata.finestLevel);
    layer.levelSelector->setCurrentIndex(0);
    layer.fieldSelector->setEnabled(true);
    layer.levelSelector->setEnabled(true);
    layer.range->reset();
    layer.range->setTrackedField(layer.fieldSelector->currentText());
    // Log is shared with the primary; the companion's own checkbox is hidden
    // (see the toolbar construction) and mirrors it.
    layer.range->setSelection({RangeMode::File, std::nullopt,
        primary().range->logarithmic()});
    layer.range->setControlsReady(!m_companionFollowsPrimary);
    layer.range->setNumberFormat(m_displayFormat);
    layer.colorBar->setPalette(&m_paletteController->palette());
    layer.colorBar->setNumberFormat(m_numberFormat);
    if (m_companionLabel != nullptr) {
        m_companionLabel->setText(tr("%1:").arg(layer.name));
    }
    updateRangeModeAvailability(layer);
}

void MainWindow::scheduleLayerSliceRequests(DatasetLayer& layer)
{
    if (!layer.active || m_viewDimension != 3) {
        return;
    }
    for (auto& state : layer.planeViews) {
        scheduleSliceRequest(state);
    }
}

void MainWindow::updatePairLayouts()
{
    if (!m_pair) {
        return;
    }
    // The primary's perpendicular factor is its axis factor, the companion's
    // its own.
    const auto p = static_cast<std::size_t>(m_pair->perpendicularAxis);
    const std::array<double, 2> perpendicular{
        m_axisScale[p], m_layers[1].perpendicularScale};
    for (int normal = 0; normal < 3; ++normal) {
        m_pairLayouts[static_cast<std::size_t>(normal)] = PairLayout(
            *m_pair, normal, m_aspectMode, m_axisScale, perpendicular);
    }
}

void MainWindow::applyPairLayouts()
{
    if (m_viewDimension != 3) {
        return;
    }
    for (auto* state : currentViews()) {
        auto* view = state->view;
        if (view == nullptr || !view->hasTileImage(state->tile)
            || state->plane->width <= 0) {
            continue;
        }
        if (m_pair) {
            const auto& layout = pairLayout(state->normal);
            view->setDisplayStretch(1.0, 1.0);
            view->placeTile(state->tile,
                toQRectF(layout.sceneRectForRegion(state->layer,
                    state->plane->physicalRegion)),
                toQRectF(layout.canvasRect()));
        } else {
            const auto& image = view->image(state->tile);
            view->placeTile(state->tile,
                QRectF(QPointF(0.0, 0.0), QSizeF(image.size())), std::nullopt);
            applyDisplayStretch(*state);
        }
    }
}

bool MainWindow::stateShown(const PlaneViewState& state) const noexcept
{
    if (!m_pair || state.normal != m_pair->perpendicularAxis) {
        return true;
    }
    return m_pair->layerAt(m_slicePosition3d[static_cast<std::size_t>(
               m_pair->perpendicularAxis)])
        == state.layer;
}

void MainWindow::updateShownLayers()
{
    if (m_viewDimension != 3) {
        return;
    }
    for (auto* state : currentViews()) {
        if (state->view != nullptr) {
            state->view->setTileVisible(state->tile, stateShown(*state));
        }
    }
    // The colour controls follow the layer on show when the active panel is
    // the one that switched.
    if (m_activeView != nullptr && m_pair
        && m_activeView->normal == m_pair->perpendicularAxis
        && !stateShown(*m_activeView)) {
        for (auto* state : statesForPanel(m_activeView->normal)) {
            if (stateShown(*state)) {
                setActiveView(*state);
                break;
            }
        }
    }
}

void MainWindow::updatePairedIsoGeometry()
{
    const auto& companion = m_layers[1];
    if (!m_pair || !primary().session || !companion.session) {
        return;
    }
    // The isometric view in the panels' proportions: with the ocean 30 times
    // shallower than the atmosphere is tall and both 70 km wide, physical
    // units would flatten the ocean to a line.
    const PairDisplayMap map(*m_pair, m_aspectMode, m_axisScale,
        {m_axisScale[static_cast<std::size_t>(m_pair->perpendicularAxis)],
            companion.perpendicularScale});
    m_isoWidget->setPairedGeometry(primary().session->metadata(),
        companion.session->metadata(),
        [map](std::size_t dataset, const Real3& point) {
            return map.displayFromPhysical(dataset, point);
        });
    publishSlicePositions();
}

void MainWindow::setCompanionFollowsPrimary(bool follows)
{
    if (m_companionFollowsPrimary == follows) {
        return;
    }
    m_companionFollowsPrimary = follows;
    auto& layer = m_layers[1];
    if (layer.range != nullptr) {
        layer.range->setControlsReady(!follows && layer.active);
    }
    if (layer.colorBar != nullptr) {
        layer.colorBar->setVisible(layer.active && !follows);
    }
    if (layer.active) {
        scheduleLayerSliceRequests(layer);
        if (m_activeView != nullptr) {
            syncActiveViewColorControls(*m_activeView);
        }
    }
}

bool MainWindow::canOpenCompanion() const
{
    if (!m_controlsReady || !primary().session || !primary().openMetadata) {
        return false;
    }
    if (std::dynamic_pointer_cast<remote::RemoteDatasetSession>(primary().session)
        || m_sequenceController->hasSequence()) {
        return false;
    }
    const auto& metadata = primary().session->metadata();
    return metadata.dimension == 3 && metadata.hasPhysicalGeometry && !metadata.isFab;
}

void MainWindow::updatePairedModeControls()
{
    const bool paired = companionOpen();
    if (m_openCompanionAction != nullptr) {
        m_openCompanionAction->setEnabled(canOpenCompanion());
    }
    if (m_closeCompanionAction != nullptr) {
        m_closeCompanionAction->setEnabled(paired);
    }
    // Features that read one dataset stay with the primary alone, so they
    // are withheld while two are shown rather than shown for one of them.
    m_exportAnimationAction->setEnabled(!paired && m_sequenceController->hasSequence());
    m_datasetAction->setEnabled(!paired && m_controlsReady);
    if (paired) {
        if (m_volumeAction != nullptr) {
            m_volumeAction->setEnabled(false);
        }
        if (m_particlesAction != nullptr) {
            m_particlesAction->setEnabled(false);
        }
    } else if (primary().session) {
        // The controllers decide for themselves what the primary supports.
        m_volumeController->configureForDataset();
        m_particleController->configureForDataset(true);
    }
    if (m_expressionEditorAction != nullptr) {
        m_expressionEditorAction->setEnabled(!paired);
    }
    if (paired) {
        // Rubber-band zoom is view-only over two datasets; the synchronized
        // form re-slices the other panels' regions and is left off.
        const QSignalBlocker blocker(m_syncRubberBandZoomAction);
        m_syncRubberBandZoomAction->setChecked(false);
    }
    m_syncRubberBandZoomAction->setEnabled(!paired);
}

} // namespace amrvis::qt
