#include "MainWindowInternal.hpp"

// Companion: a second 3-D plotfile shown beside the primary in the same
// window, local or remote beside a primary of either kind; a remote one comes
// over the primary's own connection when the primary is remote, else over the
// window's remote session. The two must share a plane -- their domains touch along one axis
// and overlap along the other two (PairGeometry) -- and each keeps its own
// session, field and level selection, range and colour bar (DatasetLayer).
// In the two panels that show the perpendicular axis both rasters are drawn
// as tiles stacked at their physical positions; the panel normal to it shows
// whichever layer holds the slice position. The companion is a secondary load
// onto an installed primary, never part of a dataset open.

namespace amrvis::qt {

namespace {

bool sameRegion(const std::optional<RealBox>& a, const std::optional<RealBox>& b)
{
    if (a.has_value() != b.has_value()) {
        return false;
    }
    return !a || (a->lower == b->lower && a->upper == b->upper);
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

void MainWindow::chooseRemoteCompanion()
{
    if (!primary().session) {
        reportBackgroundError(tr("Open a plotfile first."));
        return;
    }
    // A remote primary's companion comes from its own server, so the dialog
    // holds the session to that destination; a local primary takes one from
    // any server, over the window's remote session (started there if none).
    const auto remote
        = std::dynamic_pointer_cast<remote::RemoteDatasetSession>(primary().session);
    if (remote && remote->connection() != m_remoteSession->connection()) {
        reportBackgroundError(tr("Cannot open companion: the remote session "
                                 "that opened %1 has ended")
                .arg(datasetDisplayName(m_datasetPath)));
        return;
    }
    // Likewise while a remote companion is on show: starting a session on
    // another server would end the one it rides before the replacement has
    // paired, and a refusal must leave it as it was.
    const auto companion
        = std::dynamic_pointer_cast<remote::RemoteDatasetSession>(m_layers[1].session);
    const bool sameServer = remote != nullptr
        || (companion && companion->connection() == m_remoteSession->connection());
    m_remoteSession->promptCompanion(this, sameServer);
}

void MainWindow::openCompanion(const std::filesystem::path& path)
{
    openCompanionImpl(CompanionSource{path, std::nullopt});
}

void MainWindow::openRemoteCompanion(std::string remotePath)
{
    // Over the primary's own connection when it is remote (its server is the
    // companion's), else over the window's remote session.
    const auto remote
        = std::dynamic_pointer_cast<remote::RemoteDatasetSession>(primary().session);
    auto connection = remote ? remote->connection() : m_remoteSession->connection();
    CompanionSource source{std::filesystem::path(remotePath),
        RemoteOpen{std::move(connection), std::move(remotePath)}};
    openCompanionImpl(std::move(source));
}

std::optional<QString> MainWindow::companionSourceRefusal(
    const CompanionSource& source) const
{
    if (!source.remote) {
        return std::nullopt;  // a local plotfile pairs with either kind
    }
    const auto& connection = source.remote->connection;
    if (!connection) {
        return tr("open a remote session first "
                  "(File > Open Remote Companion Plotfile...)");
    }
    const auto remote
        = std::dynamic_pointer_cast<remote::RemoteDatasetSession>(primary().session);
    if (remote && connection != remote->connection()) {
        return tr("a remote companion must come from the server the open "
                  "dataset came from");
    }
    if (!connection->connected()) {
        return tr("the remote session has ended: %1")
            .arg(QString::fromStdString(connection->disconnectReason()));
    }
    return std::nullopt;
}

bool MainWindow::companionSessionHasCurrentDefinitions() const
{
    const auto& layer = m_layers[1];
    return !layer.active || !layer.session
        || !layer.session->supportsDerivedFields()
        || layer.session->derivedFieldDefinitions() == m_derivedFields->definitions();
}

void MainWindow::reloadCompanionIfDefinitionsMoved()
{
    if (m_closing || !m_derivedFields->available()
        || companionSessionHasCurrentDefinitions()) {
        return;
    }
    // Once per list per session, as the primary's memo (reloadIfDefinitionsMoved).
    if (m_companionReloadAskedFor
        && m_companionReloadAskedFor->first == m_derivedFields->definitions()
        && m_companionReloadAskedFor->second == m_layers[1].sessionEpoch) {
        return;
    }
    m_companionReloadAskedFor
        = {m_derivedFields->definitions(), m_layers[1].sessionEpoch};
    QTimer::singleShot(0, this, [this] {
        if (m_closing || !m_derivedFields->available()
            || companionSessionHasCurrentDefinitions()) {
            return;
        }
        if (!reloadCompanion()) {
            m_companionReloadAskedFor.reset();
        }
    });
}

bool MainWindow::sameCompanionSelections(
    const CompanionRestore& a, const CompanionRestore& b)
{
    if (a.fieldName != b.fieldName || a.levelData != b.levelData
        || a.rangeMode != b.rangeMode || a.userRange != b.userRange
        || a.slicePositions != b.slicePositions) {
        return false;
    }
    for (std::size_t normal = 0; normal < 3; ++normal) {
        if (!sameRegion(a.regions[normal], b.regions[normal])) {
            return false;
        }
    }
    return true;
}

MainWindow::CompanionRestore MainWindow::currentCompanionSelections() const
{
    const auto& layer = m_layers[1];
    CompanionRestore selections;
    selections.fieldName = layer.fieldSelector->currentText();
    selections.levelData = layer.levelSelector->currentData().toInt();
    const auto selection = layer.range->selection();
    selections.rangeMode = selection.mode;
    selections.userRange = selection.userRange;
    selections.slicePositions = m_slicePosition3d;
    for (std::size_t normal = 0; normal < 3; ++normal) {
        selections.regions[normal] = layer.planeViews[normal].visibleRegion;
    }
    return selections;
}

bool MainWindow::reloadCompanion()
{
    auto& layer = m_layers[1];
    if (!layer.active || !layer.session) {
        return false;
    }
    auto restore = currentCompanionSelections();
    CompanionSource source{layer.path, std::nullopt};
    if (const auto remote
        = std::dynamic_pointer_cast<remote::RemoteDatasetSession>(layer.session)) {
        source.remote = RemoteOpen{remote->connection(), remote->remotePath()};
    }
    openCompanionImpl(std::move(source), std::move(restore));
    return true;
}

void MainWindow::openCompanionImpl(
    CompanionSource source, std::optional<CompanionRestore> restore)
{
    const auto refuse = [this](const QString& reason) {
        reportBackgroundError(tr("Cannot open companion: %1").arg(reason));
        emit companionOpenFinished(false);
    };
    if (!m_controlsReady || !primary().session || !primary().openMetadata) {
        refuse(tr("open a plotfile first"));
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
    if (const auto reason = companionSourceRefusal(source)) {
        refuse(*reason);
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
    // default field and level. The derived-field list is the window's, so it
    // is installed here too; a definition this dataset cannot resolve is left
    // out and shown greyed in its selector (see configureCompanionControls).
    FrameSliceSpec spec;
    if (derivedFieldsReachNextLoad()) {
        spec.derivedFields = m_derivedFields->definitions();
    }
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
    // With the mapped grid on, a companion that carries node positions is
    // drawn on them from the start: the whole node box at the viewport's
    // pixels (a replaced companion's windows do not carry over).
    spec.mappedGrid = m_mappedGrid && !displayIsSpherical();
    if (spec.mappedGrid) {
        for (const auto& state : m_layers[1].planeViews) {
            spec.displayWindows.push_back(RealBox{});
            spec.displayPixels.push_back(viewportPixelSize(state));
        }
    }
    if (restore) {
        // A reload renders the selections it is putting back from the first
        // slice: the field by name (resolveSpecField), the level and range.
        spec.fieldName = restore->fieldName.toStdString();
        spec.levelSelection = restore->levelData;
        spec.rangeMode = restore->rangeMode;
        spec.userRange = restore->userRange;
        spec.visibleRegions.assign(restore->regions.begin(), restore->regions.end());
    }
    if (source.remote) {
        // A remote raster is sized to the panel it lands on, as a remote
        // open's is; the companion's states share the primary's panels, so
        // their viewports are known before the install.
        spec.outputSizesAreViewportBounds = true;
        for (const auto& state : m_layers[1].planeViews) {
            spec.outputSizes.push_back(stretchedViewportPixelSize(state));
        }
    }
    const auto primaryMetadata = primary().openMetadata;
    m_diagnosticsModel->adjustActivity(1);
    statusBar()->showMessage(
        (restore ? tr("Reloading companion %1...") : tr("Loading companion %1..."))
            .arg(datasetDisplayName(source.path)));

    const auto path = source.path;
    auto* watcher = new QFutureWatcher<CompanionLoad>(this);
    connect(watcher, &QFutureWatcher<CompanionLoad>::finished, this,
        [this, watcher, path, generation, companionGeneration, cancellation,
            restore = std::move(restore)] {
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
                    installCompanion(path, std::move(load), restore);
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
                    // A reload that failed did not happen: the list is still
                    // uninstalled, so the next Apply may ask again.
                    m_companionReloadAskedFor.reset();
                    emit companionOpenFinished(false);
                } else {
                    m_diagnosticsModel->noteStaleResult();
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    // A dataset id no primary load can produce, and one no earlier companion
    // used: the high bit marks the companion, the middle bits count companion
    // opens, the low bits carry the primary generation it was opened beside.
    // A remote companion takes the server's id instead, which cannot alias
    // the primary's: ids count up per connection, and both ride the same one.
    const DatasetId id{(std::uint64_t{1} << 63)
        | (companionGeneration << 40) | (generation & ((std::uint64_t{1} << 40) - 1))};
    watcher->setFuture(QtConcurrent::run(
        [source = std::move(source), spec = std::move(spec), cancellation, id,
            primaryMetadata]() mutable {
            return source.remote
                ? loadRemoteCompanion(*source.remote, std::move(spec),
                      *primaryMetadata, cancellation)
                : loadLocalCompanion(source.path, std::move(spec),
                      *primaryMetadata, id, cancellation);
        }));
}

MainWindow::CompanionLoad MainWindow::loadLocalCompanion(
    const std::filesystem::path& path, FrameSliceSpec spec,
    const DatasetMetadata& primaryMetadata, DatasetId id, StopToken cancellation)
{
    CompanionLoad load;
    load.metadata = readDatasetMetadata(path, cancellation);
    auto pairing = pairGeometry(primaryMetadata, *load.metadata.metadata);
    if (!pairing.geometry) {
        throw std::runtime_error(pairing.error);
    }
    load.geometry = *pairing.geometry;
    // One native raster per panel, as a local open makes: the companion's
    // tiles are placed by the layout, never resampled.
    const auto& metadata = *load.metadata.metadata;
    const auto bounds = datasetSampleBounds(metadata);
    spec.outputSizes.clear();
    for (int normal = 0; normal < 3; ++normal) {
        // A reload's zoomed region, else the whole domain.
        const auto n = static_cast<std::size_t>(normal);
        const auto region = n < spec.visibleRegions.size() && spec.visibleRegions[n]
            ? *spec.visibleRegions[n]
            : bounds;
        spec.outputSizes.push_back(finestNativeOutputSize(metadata, region, normal));
    }
    // The data root a plain open resolves to: the plotfile directory itself
    // (a Header path's parent otherwise).
    auto root = std::filesystem::is_directory(path) ? path : path.parent_path();
    if (root.empty()) {
        root = ".";
    }
    load.result = executeFrameLoad(path, id, spec, initialCacheBudget(),
        cancellation, load.metadata, std::move(root));
    return load;
}

MainWindow::CompanionLoad MainWindow::loadRemoteCompanion(
    const RemoteOpen& remote, FrameSliceSpec spec,
    const DatasetMetadata& primaryMetadata, StopToken cancellation)
{
    // Said the way loadRemoteFrame says it: "not connected" is what a user
    // can act on, a bare transact failure is not.
    if (!remote.connection || !remote.connection->connected()) {
        throw std::runtime_error("remote session is not connected: "
            + (remote.connection ? remote.connection->disconnectReason()
                                 : std::string("no connection")));
    }
    auto session = openRemoteSessionForLoad(
        remote.connection, remote.remotePath, spec.derivedFields, cancellation);
    CompanionLoad load;
    load.metadata.metadata
        = std::make_shared<const DatasetMetadata>(session->metadata());
    load.metadata.metrics = session->metadataReadMetrics();
    load.metadata.fileVersion = session->fileVersion();
    auto pairing = pairGeometry(primaryMetadata, *load.metadata.metadata);
    if (!pairing.geometry) {
        // The session goes with the exception, closing its server handle.
        throw std::runtime_error(pairing.error);
    }
    load.geometry = *pairing.geometry;
    load.result = executeSessionFrameLoad(std::move(session), spec, cancellation);
    return load;
}

void MainWindow::installCompanion(const std::filesystem::path& path,
    CompanionLoad load, const std::optional<CompanionRestore>& restore)
{
    // A reload's selections as they stand now: the load rendered `restore`,
    // and whatever moved meanwhile -- a field picked, the position dragged,
    // a zoom -- wins over it below.
    const auto live = restore && m_layers[1].active
        ? std::optional<CompanionRestore>(currentCompanionSelections())
        : restore;
    if (m_layers[1].active) {
        tearDownCompanion(/*replacing=*/true);
    }
    // A new companion starts at its own scale; only a reload keeps the one
    // set for it (tearDownCompanion leaves it for either).
    if (!restore) {
        m_layers[1].perpendicularScale = 1.0;
    }
    // The Dataset window tabulates the active view's dataset at its slice
    // position; with two datasets on the panels it would mix them, so it is
    // closed as an open does (it is unavailable while a companion is shown).
    closeDatasetWindow();
    // A companion open starts both datasets at the whole domain, as any open
    // does: a rubber-band selection made before is re-sliced whole. Its
    // region was one raster's; the pair's zoom is the panel's (see
    // pairRubberBandZoom). A reload keeps the pair's zoom.
    for (auto* state : primaryViews()) {
        // A pair never sits on a virtual canvas: a remote fixed scale's
        // whole-domain canvas goes too, or the export would follow it past
        // the pair's framed window.
        state->view->setVirtualCanvas(std::nullopt);
        if (!restore && state->visibleRegion.has_value()) {
            state->visibleRegion.reset();
            scheduleSliceRequest(*state);
        }
    }
    if (!restore) {
        m_pairWindows = {};
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
    // A reload's regions go back before the layouts are updated: the framed
    // windows are rebuilt from the regions on show then (updatePairLayouts),
    // and with only the primary's left they would shrink to its part.
    for (std::size_t index = 0; index < layer.planeViews.size(); ++index) {
        layer.planeViews[index].visibleRegion
            = live ? live->regions[index] : std::nullopt;
    }
    if (load.result.displays.size() != layer.planeViews.size()) {
        closeCompanion();
        reportBackgroundError(
            tr("Cannot open companion: slice count does not match the panels"));
        emit companionOpenFinished(false);
        return;
    }
    configureCompanionControls(load.result.displays.empty()
            ? std::nullopt
            : std::optional<std::uint32_t>(
                  load.result.displays.front().request.field.value),
        live);
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
    // The load rendered the selections a reload set out with; anything that
    // moved while it ran is sliced again against the installed session.
    if (restore && live && !sameCompanionSelections(*restore, *live)) {
        scheduleLayerSliceRequests(layer);
    }
    // A warped primary tile moved onto the pair's canvas without a view
    // change to redraw it: asked for the window it now shows.
    for (auto* state : primaryViews()) {
        if (isWarped(state->warp)) {
            updateMappedDemand(*state);
        }
    }
    updateShownLayers();
    updatePairedModeControls();
    updateMappedGridControls();
    updateAspectControls();
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
    statusBar()->showMessage(
        (restore ? tr("Companion %1 reloaded") : tr("Companion %1 opened")).arg(layer.name),
        5000);
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
        state.warp = DisplayWarp::None;
        state.gridNodes.reset();
        state.displaySourceIndex.reset();
        state.mappedCanvasBounds.reset();
        state.mappedWindow = {};
        state.mappedWindowPixels = {0, 0};
        state.mappedNodeBounds = {};
        state.pixmapRegion = {};
        state.displayRegion = {};
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
    if (!replacing) {
        layer.perpendicularScale = 1.0;
    }
    // A sync still on a worker keeps its in-flight flag: its completion
    // clears it and drops the outcome (the session is gone), and a companion
    // installed meanwhile queues behind it through the rerun flag rather
    // than dispatching a second worker. Only the pending rerun is dropped.
    layer.visibleSyncRerun = false;
    layer.pendingRangeStore.reset();
    m_pair.reset();
    if (!replacing) {
        m_pairWindows = {};
    }
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
            updateIsoGeometry();
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
        updateMappedGridControls();
        updateAspectControls();
        configureSlicePositionControls();
        // A warped primary goes back on its own canvas and is drawn again
        // for what the viewport shows (applyDisplayStretches); a warped
        // companion's leaving gives the line tool back.
        applyDisplayStretches();
        for (auto* state : primaryViews()) {
            updateLineToolAvailability(*state);
        }
        if (!replacing) {
            // A remote primary's fixed scale rides a demand-driven virtual
            // canvas, which the pair displaced (installCompanion); each panel
            // still at a fixed scale goes back on its own, at its own factor,
            // and a panel zoomed meanwhile is left as it is.
            for (auto* state : primaryViews()) {
                auto* view = state->view;
                if (view == nullptr || !layerIsRemote(*state) || displayIsSpherical()
                    || isWarped(state->warp)
                    || view->transformMode() != ImageView::TransformMode::FixedScale) {
                    continue;
                }
                view->setVirtualCanvas(
                    virtualPlacementFor(*state, state->plane->physicalRegion));
                view->setFixedScale(view->fixedScaleFactor());
                if (remoteDemandCanvas(*state)) {
                    updateRemoteFixedScaleDemand(*state);
                }
            }
        }
        updateCrosshairs();
        updateWindowTitle();
        refreshMetadataDisplay();
    }
}

void MainWindow::configureCompanionControls(
    std::optional<std::uint32_t> loadedField,
    const std::optional<CompanionRestore>& selections)
{
    auto& layer = m_layers[1];
    if (!layer.session || layer.fieldSelector == nullptr) {
        return;
    }
    const QSignalBlocker fieldBlocker(layer.fieldSelector);
    const QSignalBlocker levelBlocker(layer.levelSelector);
    const auto& metadata = layer.session->metadata();
    // Stored fields, then the derived ones this dataset resolves, the rest
    // greyed with the reason -- the primary's list, built for this layer.
    populateFieldSelector(layer, derivedFieldRows(layer));
    // A reload's field by name when this list has it (picked during the load,
    // perhaps, so newer than what the load rendered); else the row the load
    // rendered -- a name this list dropped shows what took its place rather
    // than a greyed row of the same name.
    auto row = -1;
    if (selections) {
        const auto named = layer.fieldSelector->findText(selections->fieldName);
        if (named >= 0 && layer.fieldSelector->itemData(named).isValid()) {
            row = named;
        }
    }
    if (row < 0 && loadedField) {
        row = layer.fieldSelector->findData(static_cast<unsigned int>(*loadedField));
    }
    selectFieldItem(layer, std::max(row, 0));
    populateLevelCombo(layer.levelSelector, metadata.finestLevel);
    const auto levelRow
        = selections ? layer.levelSelector->findData(selections->levelData) : -1;
    layer.levelSelector->setCurrentIndex(std::max(levelRow, 0));
    layer.fieldSelector->setEnabled(true);
    layer.levelSelector->setEnabled(true);
    layer.range->reset();
    layer.range->setTrackedField(layer.fieldSelector->currentText());
    // Log is shared with the primary; the companion's own checkbox is hidden
    // (see the toolbar construction) and mirrors it.
    layer.range->setSelection({selections ? selections->rangeMode : RangeMode::File,
        selections ? selections->userRange : std::nullopt,
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

RealBox MainWindow::pairDisplayBounds(std::size_t layer) const
{
    auto bounds = m_pair->bounds[layer];
    for (const auto& state : m_layers[layer].planeViews) {
        if (requestedWarpFor(state) != DisplayWarp::MappedGrid || !state.mappedCanvasBounds) {
            continue;
        }
        for (const auto axis : displayAxes(state.normal)) {
            const auto a = static_cast<std::size_t>(axis);
            bounds.lower[a] = std::min(bounds.lower[a], state.mappedCanvasBounds->lower[a]);
            bounds.upper[a] = std::max(bounds.upper[a], state.mappedCanvasBounds->upper[a]);
        }
    }
    return bounds;
}

bool MainWindow::updatePairLayouts()
{
    if (!m_pair) {
        return false;
    }
    // The primary's perpendicular factor is its axis factor, the companion's
    // its own. A mapped grid's pixmap is physical, so the pair is laid out
    // in Physical Size while the grid is on, as one dataset is.
    const auto p = static_cast<std::size_t>(m_pair->perpendicularAxis);
    const std::array<double, 2> perpendicular{
        m_axisScale[p], m_layers[1].perpendicularScale};
    const auto mode = displayIsMapped() ? AspectMode::PhysicalSize : m_aspectMode;
    auto geometry = *m_pair;
    geometry.displayBounds = {pairDisplayBounds(0), pairDisplayBounds(1)};
    const auto previous = m_pairLayouts;
    for (int normal = 0; normal < 3; ++normal) {
        m_pairLayouts[static_cast<std::size_t>(normal)] = PairLayout(
            geometry, normal, mode, m_axisScale, perpendicular);
    }
    // A window is in scene units, which the layout defines: after a change
    // of aspect or axis scale it is the regions' rect under the new one. An
    // unchanged layout (a reload) keeps it as framed: the regions are rounded
    // out to cell edges, and a window rebuilt from them would grow.
    bool changed = false;
    for (std::size_t normal = 0; normal < 3; ++normal) {
        const auto& before = previous[normal];
        const auto& after = m_pairLayouts[normal];
        if (before.tileRect(0) == after.tileRect(0)
            && before.tileRect(1) == after.tileRect(1)) {
            continue;
        }
        changed = true;
        if (m_pairWindows[normal]) {
            m_pairWindows[normal] = pairRegionsRect(static_cast<int>(normal));
        }
    }
    return changed;
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
            // A warped tile's pixmap spans the window it was drawn for.
            const auto& layout = pairLayout(state->normal);
            const auto& region = isWarped(state->warp)
                ? state->pixmapRegion : state->plane->physicalRegion;
            view->setDisplayStretch(1.0, 1.0);
            view->placeTile(state->tile,
                toQRectF(layout.sceneRectForRegion(state->layer, region)),
                toQRectF(pairCanvasRect(state->normal)));
        } else if (const auto placed = tilePlacement(*state)) {
            // A warped tile back on its own canvas (MappedLayout).
            view->setDisplayStretch(1.0, 1.0);
            view->placeTile(state->tile,
                toQRectF(placed->sceneRectForRegion(state->pixmapRegion)),
                toQRectF(placed->canvas));
        } else {
            // The others go back to the classic raster-at-origin scene.
            const auto& image = view->image(state->tile);
            view->placeTile(state->tile,
                QRectF(QPointF(0.0, 0.0), QSizeF(image.size())), std::nullopt);
            applyDisplayStretch(*state);
        }
    }
}

SceneRect MainWindow::pairCanvasRect(int normal) const
{
    const auto& window = m_pairWindows[static_cast<std::size_t>(normal)];
    if (window) {
        return SceneRect{window->x(), window->y(), window->width(), window->height()};
    }
    return pairLayout(normal).canvasRect();
}

std::optional<QRectF> MainWindow::pairRegionsRect(int normal) const
{
    const auto& layout = pairLayout(normal);
    std::optional<QRectF> rect;
    for (const auto& layer : m_layers) {
        const auto& state = layer.planeViews[static_cast<std::size_t>(normal)];
        if (!layer.active || !stateShown(state)) {
            continue;
        }
        std::optional<QRectF> part;
        if (isWarped(state.warp)) {
            // Its demand region is not a zoom; the window it draws for is.
            if (hasMappedWindow(state)) {
                part = toQRectF(layout.sceneRectForRegion(state.layer, state.mappedWindow))
                           .intersected(toQRectF(layout.tileRect(state.layer)));
            }
        } else if (state.visibleRegion) {
            part = toQRectF(layout.sceneRectForRegion(state.layer, *state.visibleRegion));
        }
        if (part) {
            rect = rect ? rect->united(*part) : part;
        }
    }
    return rect;
}

std::array<std::optional<RealBox>, 2> MainWindow::pairRegionsForSceneWindow(
    int normal, const QRectF& window) const
{
    std::array<std::optional<RealBox>, 2> regions;
    if (!m_pair || normal < 0 || normal > 2) {
        return regions;
    }
    const auto& layout = pairLayout(normal);
    const SceneRect rect{window.x(), window.y(), window.width(), window.height()};
    for (std::size_t layer = 0; layer < 2; ++layer) {
        const auto& dataset = m_layers[layer];
        // On the panel normal to the shared plane the layers overlap and one
        // is hidden; it takes no part (see updateShownLayers for when it comes
        // on show).
        const auto& state = dataset.planeViews[static_cast<std::size_t>(normal)];
        if (!dataset.session || !stateShown(state)
            || isWarped(state.warp)) {
            continue;
        }
        const auto region = layout.regionForSceneRect(layer, rect);
        if (region) {
            regions[layer] = snappedPairRegion(layer, normal, *region);
        }
    }
    return regions;
}

std::optional<QRectF> MainWindow::pairFramedWindow(int normal, const QRectF& window,
    const std::array<std::optional<RealBox>, 2>& regions) const
{
    if (!m_pair || normal < 0 || normal > 2) {
        return std::nullopt;
    }
    const auto& layout = pairLayout(normal);
    const SceneRect rect{window.x(), window.y(), window.width(), window.height()};
    std::optional<QRectF> framed;
    for (std::size_t layer = 0; layer < 2; ++layer) {
        const auto& state = m_layers[layer].planeViews[static_cast<std::size_t>(normal)];
        if (!m_layers[layer].session || !stateShown(state)) {
            continue;
        }
        std::optional<QRectF> part;
        if (isWarped(state.warp)) {
            // The window's part over the warped tile, as the view frames it.
            if (layout.regionForSceneRect(layer, rect)) {
                part = window.intersected(toQRectF(layout.tileRect(layer)));
            }
        } else if (regions[layer]) {
            part = toQRectF(layout.sceneRectForRegion(layer, *regions[layer]));
        }
        if (part) {
            framed = framed ? framed->united(*part) : part;
        }
    }
    return framed;
}

RealBox MainWindow::snappedPairRegion(
    std::size_t layer, int normal, const RealBox& region) const
{
    // Local slices are one pixel per finest cell, so the region grows out to
    // this layer's cell edges and covers the window whole; a remote raster
    // is resampled and keeps the exact window (as applyRubberBandZoom does
    // for one dataset). The framed window itself is kept apart from the
    // regions (m_pairWindows), so their rounding never feeds back into it.
    const auto& dataset = m_layers[layer];
    if (!dataset.session
        || layerIsRemote(dataset.planeViews[static_cast<std::size_t>(normal)])) {
        return region;
    }
    const auto& metadata = dataset.session->metadata();
    const auto& finest
        = metadata.levels[static_cast<std::size_t>(std::max(0, metadata.finestLevel))];
    const auto domain = datasetSampleBounds(metadata);
    const auto axes = displayAxes(normal);
    return snapToCellBoundaries(region, domain, finest.cellSize, axes);
}

bool MainWindow::applyPairZoomWindow(int normal, const QRectF& window, bool refit)
{
    // A selection frames what its snapped regions cover; a pan frames the
    // shifted window itself, at its size.
    return applyPairRegions(normal, pairRegionsForSceneWindow(normal, window),
        window, refit);
}

bool MainWindow::applyPairRegions(int normal,
    const std::array<std::optional<RealBox>, 2>& regions,
    const QRectF& window, bool refit)
{
    // Nothing to frame over a window no layer is under (a pan into the
    // corner of the canvas neither tile covers); a pan that is frames the
    // shifted window itself, a selection its parts over the layers.
    const auto covered = pairFramedWindow(normal, window, regions);
    if (!covered) {
        return false;
    }
    const auto framed = refit ? *covered : window;
    std::vector<PlaneViewState*> changed;
    for (auto* state : statesForPanel(normal)) {
        if (isWarped(state->warp)) {
            // A warped tile draws for what the view then shows.
            continue;
        }
        const auto& region = regions[state->layer];
        if (region) {
            state->visibleRegion = region;
            changed.push_back(state);
        } else if (state->visibleRegion) {
            // Past this layer's edge: back to its whole domain, which sits
            // beside the framed window rather than inside it.
            state->visibleRegion.reset();
            changed.push_back(state);
        }
    }
    // The framed window: the regions' union for a selection, the shifted
    // window for a pan. Framed before the rasters arrive, confined so no
    // scroll bars appear meanwhile; each arrival lands at its region's rect
    // on this canvas and the transform keeps the frame (see showSlice's
    // paired arm). A pan keeps the scale -- a fixed one included -- and
    // moves onto the window.
    m_pairWindows[static_cast<std::size_t>(normal)] = framed;
    auto* view = primary().planeViews[static_cast<std::size_t>(normal)].view;
    const auto canvas = toQRectF(pairCanvasRect(normal));
    if (refit) {
        view->zoomToSceneRect(canvas, /*confineScene=*/true);
    } else {
        view->showSceneWindow(canvas);
    }
    for (auto* state : changed) {
        scheduleSliceRequest(*state);
    }
    return true;
}

void MainWindow::pairRubberBandZoom(int normal, const QRectF& sceneRect)
{
    const auto window = sceneRect.normalized();
    if (window.width() < 1.0e-9 || window.height() < 1.0e-9) {
        return;
    }
    const auto regions = pairRegionsForSceneWindow(normal, window);
    if (!applyPairRegions(normal, regions, window, /*refit=*/true)) {
        return;
    }
    const bool synchronize = m_syncRubberBandZoomAction != nullptr
        && m_syncRubberBandZoomAction->isChecked() && m_viewDimension == 3;
    if (synchronize) {
        // Each other panel shares one axis with this one: it takes the
        // selection's extent along that axis, over both layers' parts (a
        // warped layer's is the window cut to it), and keeps its other axis
        // whole -- the physical form of the fractions a single dataset's
        // sync carries (see rubberBandZoom).
        const auto& layout = pairLayout(normal);
        const SceneRect rect{window.x(), window.y(), window.width(), window.height()};
        for (int other = 0; other < 3; ++other) {
            if (other == normal) {
                continue;
            }
            const auto c = static_cast<std::size_t>(3 - normal - other);
            std::optional<std::pair<double, double>> extent;
            for (std::size_t layer = 0; layer < 2; ++layer) {
                const auto& state = m_layers[layer].planeViews[static_cast<std::size_t>(normal)];
                auto region = regions[layer];
                if (!region && m_layers[layer].session && stateShown(state)
                    && isWarped(state.warp)) {
                    region = layout.regionForSceneRect(layer, rect);
                }
                if (!region) {
                    continue;
                }
                extent = extent ? std::pair{std::min(extent->first, region->lower[c]),
                                      std::max(extent->second, region->upper[c])}
                                : std::pair{region->lower[c], region->upper[c]};
            }
            // The selection on the other panel: each shown layer's display
            // bounds cut to the extent, as one scene rect.
            const auto& otherLayout = pairLayout(other);
            std::optional<QRectF> selection;
            for (std::size_t layer = 0; layer < 2 && extent; ++layer) {
                if (!m_layers[layer].session
                    || !stateShown(m_layers[layer].planeViews[static_cast<std::size_t>(other)])) {
                    continue;
                }
                auto region = pairDisplayBounds(layer);
                region.lower[c] = std::max(region.lower[c], extent->first);
                region.upper[c] = std::min(region.upper[c], extent->second);
                const auto span = m_pair->bounds[layer].upper[c] - m_pair->bounds[layer].lower[c];
                if (region.upper[c] - region.lower[c] > 1.0e-9 * span) {
                    const auto part = toQRectF(otherLayout.sceneRectForRegion(layer, region));
                    selection = selection ? selection->united(part) : part;
                }
            }
            if (selection) {
                applyPairZoomWindow(other, *selection, /*refit=*/true);
            }
        }
    }
    // One panel zoomed and the others as they were is Mixed (see rubberBandZoom).
    setScaleUiState(synchronize || m_viewDimension != 3 ? ScaleUiState::Custom
                                                        : ScaleUiState::Mixed);
    m_volumeController->regionChanged();
}

void MainWindow::resetPairPanelZoom(int normal)
{
    std::vector<PlaneViewState*> zoomed;
    for (auto* state : statesForPanel(normal)) {
        if (state->visibleRegion) {
            state->visibleRegion.reset();
            zoomed.push_back(state);
        }
    }
    m_pairWindows[static_cast<std::size_t>(normal)].reset();
    // With no region left the canvas is the layout's again; the tiles go
    // back to their whole-domain places on it and the view frames it.
    applyPairLayouts();
    primary().planeViews[static_cast<std::size_t>(normal)].view->fitToWindow();
    for (auto* state : zoomed) {
        scheduleSliceRequest(*state);
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

bool MainWindow::sliceOnItsWay(const PlaneViewState& state) const
{
    return state.pendingRequests > 0 || m_pendingAllViews
        || std::find(m_pendingViews.begin(), m_pendingViews.end(), &state)
            != m_pendingViews.end();
}

void MainWindow::applyPairTileVisibility(PlaneViewState& state)
{
    if (!m_pair || state.view == nullptr) {
        return;
    }
    if (stateShown(state)) {
        state.view->setTileVisible(state.tile, true);
        for (auto* other : statesForPanel(state.normal)) {
            if (other != &state && !stateShown(*other)) {
                other->view->setTileVisible(other->tile, false);
            }
        }
        return;
    }
    bool hold = false;
    for (auto* other : statesForPanel(state.normal)) {
        hold = hold
            || (other != &state && stateShown(*other) && sliceOnItsWay(*other)
                && !other->view->isTileVisible(other->tile));
    }
    state.view->setTileVisible(state.tile, hold);
}

void MainWindow::updateShownLayers(bool sliceNewlyShown)
{
    if (m_viewDimension != 3) {
        return;
    }
    for (auto* state : currentViews()) {
        if (state->view == nullptr) {
            continue;
        }
        const bool shown = stateShown(*state);
        const bool wasShown = state->view->isTileVisible(state->tile);
        // Newly on show under a framed window, it takes the window's part of
        // its domain and slices for it: crossing the interface keeps the
        // zoom, though the hidden layer took no part in it.
        const auto& window = m_pairWindows[static_cast<std::size_t>(state->normal)];
        if (shown && !wasShown && window && m_pair && sliceNewlyShown) {
            if (isWarped(state->warp)) {
                // A warped tile draws for what the window shows of it; a
                // flat one, its warp fallen back or not yet landed, takes
                // the window's part of its domain below.
                updateMappedDemand(*state);
            } else {
                const SceneRect rect{window->x(), window->y(), window->width(), window->height()};
                if (const auto region
                    = pairLayout(state->normal).regionForSceneRect(state->layer, rect)) {
                    state->visibleRegion = snappedPairRegion(state->layer, state->normal, *region);
                    scheduleSliceRequest(*state);
                }
            }
        }
        // A switch waits for the incoming layer's slice when one is on its
        // way: the tile it holds is stale (a slice at its face, from before
        // the position entered it), and the outgoing tile stays on show until
        // showSlice switches them (applyPairTileVisibility).
        if (shown != wasShown && m_pair) {
            const PlaneViewState* incoming = nullptr;
            for (const auto* other : statesForPanel(state->normal)) {
                if (stateShown(*other)) {
                    incoming = other;
                }
            }
            if (incoming != nullptr && sliceOnItsWay(*incoming)) {
                continue;
            }
        }
        state->view->setTileVisible(state->tile, shown);
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
    const auto mode = displayIsMapped() ? AspectMode::PhysicalSize : m_aspectMode;
    const PairDisplayMap map(*m_pair, mode, m_axisScale,
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
    if (m_sequenceController->hasSequence()) {
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
    if (m_openRemoteCompanionAction != nullptr) {
        m_openRemoteCompanionAction->setEnabled(canOpenCompanion());
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
}

} // namespace amrvis::qt
