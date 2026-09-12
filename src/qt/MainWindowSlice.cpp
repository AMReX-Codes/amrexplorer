#include "MainWindowInternal.hpp"

#include <QStandardItemModel>
#include <utility>

namespace amrvis::qt {

namespace {

// The display-range cache is keyed by dataset id, and a companion's id can
// collide with the primary's: a local primary's is the window's generation, a
// remote companion's the server's counter, both small numbers. The
// companion's key is salted so the two layers never share an entry.
[[nodiscard]] DatasetId rangeCacheDataset(std::size_t layer, DatasetId id) noexcept
{
    return layer == 1 ? DatasetId{id.value ^ (std::uint64_t{1} << 62)} : id;
}

} // namespace

void MainWindow::enableDatasetControls(const DatasetMetadata& metadata)
{
    m_controlsReady = true;
    primary().fieldSelector->setEnabled(true);
    primary().levelSelector->setEnabled(true);
    primary().range->setControlsReady(true);
    m_boxesAction->setEnabled(true);
    updateMappedGridControls();
    updateAspectControls();
    updateScaleBarAvailability();
    updatePairedModeControls();
    m_slicePlanesAction->setEnabled(metadata.dimension == 3);
    rebuildLevelMenu();
    m_levelMenu->setEnabled(true);
    m_contoursAction->setEnabled(true);
    m_datasetAction->setEnabled(true);
}

void MainWindow::configureSliceControls()
{
    if (!primary().session) {
        return;
    }
    const QSignalBlocker fieldBlocker(primary().fieldSelector);
    const QSignalBlocker levelBlocker(primary().levelSelector);
    const auto& metadata = primary().session->metadata();

    // Built once and shared: the field selector and the Variable menu list
    // the same definitions.
    const auto derivedRows = derivedFieldRows();
    populateFieldSelector(derivedRows);
    selectFieldItem(0);
    // The range memory is filed under the field's name, so the widgets have to
    // be told which field they represent here too -- the restored-spec and
    // sequence paths are not the only ones that select a field, and without
    // this the first field's range is committed under an empty name and lost.
    primary().range->setTrackedField(primary().fieldSelector->currentText());

    populateLevelCombo(primary().levelSelector, metadata.finestLevel);
    primary().levelSelector->setCurrentIndex(0);

    enableDatasetControls(metadata);

    rebuildVariableMenu(derivedRows);
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
        updateIsoGeometry();
        publishSlicePositions();
        // A primary reload under a companion: the isometric view keeps
        // outlining both domains, whether or not the companion reloads too.
        if (m_pair && m_layers[1].active) {
            updatePairedIsoGeometry();
        }
    }
    ensureVectorFieldDefaults();
}

bool MainWindow::addUnavailableFieldItem(
    QComboBox* selector, const QString& name, const QString& tooltip)
{
    // Through the model, because a combo box has no per-item enable of its
    // own: an item that is not selectable is skipped by the keyboard and drawn
    // greyed by the style. Without one there is no way to add this row safely,
    // so it is not added -- leaving a definition off a list says less than
    // showing it greyed out, but far less than offering a broken selection.
    auto* model = qobject_cast<QStandardItemModel*>(selector->model());
    if (model == nullptr) {
        return false;
    }
    const auto row = selector->count();
    // No field id: nothing that reads item data can mistake it for one.
    selector->addItem(name);
    selector->setItemData(row, tooltip, Qt::ToolTipRole);
    auto* item = model->item(row);
    if (item == nullptr) {
        selector->removeItem(row);
        return false;
    }
    item->setFlags(
        item->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
    return true;
}

std::size_t MainWindow::storedFieldCount(const DatasetLayer& layer) const
{
    if (!layer.session) {
        return 0;
    }
    return std::min(
        layer.session->storedFieldCount(), layer.session->metadata().fields.size());
}

std::vector<MainWindow::DerivedFieldRow> MainWindow::derivedFieldRows(
    const DatasetLayer& layer) const
{
    std::vector<DerivedFieldRow> rows;
    // Nothing at all where no definition could ever apply: a remote session
    // reports every field as stored, so each one would list as "unavailable
    // for this dataset" beside an editor saying derived fields need a local
    // one -- two explanations of the same fact, and clutter that cannot
    // become usable while this session is open.
    if (!layer.session || !layer.session->supportsDerivedFields()
        || layer.session->metadata().isFab) {
        return rows;
    }
    const auto& fields = layer.session->metadata().fields;
    const auto stored = storedFieldCount(layer);
    const auto& definitions = m_derivedFields->definitions();
    const auto skipped = layer.session->skippedDerivedFields();
    rows.reserve(definitions.size());
    for (const auto& definition : definitions) {
        DerivedFieldRow row;
        row.name = QString::fromStdString(definition.name);
        const auto expression = escapedExpression(definition.expression);
        const auto installed = std::find_if(
            fields.begin() + static_cast<std::ptrdiff_t>(stored), fields.end(),
            [&definition](const FieldMetadata& field) {
                return field.name == definition.name;
            });
        if (installed != fields.end()) {
            row.field = static_cast<std::uint32_t>(
                std::distance(fields.begin(), installed));
            row.tooltip = richTooltip(expression);
            rows.push_back(std::move(row));
            continue;
        }
        // Not installed here, and saying why. The list is shared by every
        // window, so a definition that means nothing here means something
        // next door, and showing it as unavailable says so better than its
        // absence does.
        const auto reason = std::find_if(skipped.begin(), skipped.end(),
            [&definition](const DerivedFieldSkip& entry) {
                return entry.name == definition.name;
            });
        // An empty reason falls back to the wording that promises none: the
        // decoder accepts one (an absent wire string decodes as empty), and
        // "unavailable here: " with nothing after the colon tells the user a
        // reason exists while showing it to them blank.
        const auto haveReason
            = reason != skipped.end() && !reason->reason.empty();
        row.tooltip = richTooltip(haveReason
                ? tr("%1 -- unavailable here: %2")
                      .arg(expression,
                          QString::fromStdString(reason->reason)
                              .toHtmlEscaped())
                : tr("%1 -- unavailable for this dataset").arg(expression));
        rows.push_back(std::move(row));
    }
    return rows;
}

void MainWindow::selectFieldItem(DatasetLayer& layer, int index)
{
    // Not every row is a field: the separator between the stored and the
    // derived ones carries no item data, and neither does a definition this
    // dataset cannot provide. setCurrentIndex skips none of them, and landing
    // on one shows a row whose currentData() is invalid, which every reader of
    // it then takes for field 0 while the range memory files itself under that
    // row's name. So the caller's index is where to start looking rather than
    // what to select: the selection goes to the first field at or after it,
    // and failing that to the nearest one before it.
    const auto count = layer.fieldSelector->count();
    const auto isField = [&layer](int row) {
        return layer.fieldSelector->itemData(row).isValid();
    };
    auto selected = -1;
    for (auto row = std::max(index, 0); row < count; ++row) {
        if (isField(row)) {
            selected = row;
            break;
        }
    }
    for (auto row = std::min(index, count) - 1; selected < 0 && row >= 0;
        --row) {
        if (isField(row)) {
            selected = row;
        }
    }
    // -1 when the list holds no field at all, which leaves nothing selected
    // rather than naming a row that is not one.
    layer.fieldSelector->setCurrentIndex(selected);
}

void MainWindow::populateFieldSelector(
    DatasetLayer& layer, const std::vector<DerivedFieldRow>& rows)
{
    layer.fieldSelector->clear();
    if (!layer.session) {
        return;
    }
    const auto& fields = layer.session->metadata().fields;
    const auto stored = storedFieldCount(layer);
    for (std::size_t field = 0; field < stored; ++field) {
        layer.fieldSelector->addItem(QString::fromStdString(fields[field].name),
            static_cast<unsigned int>(field));
    }

    if (rows.empty()) {
        return;
    }
    // The computed fields are a different kind of thing from the ones the
    // plotfile holds; the rule is worth showing rather than leaving to be
    // inferred from the order.
    layer.fieldSelector->insertSeparator(layer.fieldSelector->count());
    for (const auto& row : rows) {
        if (!row.field) {
            static_cast<void>(addUnavailableFieldItem(layer.fieldSelector, row.name, row.tooltip));
            continue;
        }
        const auto index = layer.fieldSelector->count();
        layer.fieldSelector->addItem(
            row.name, static_cast<unsigned int>(*row.field));
        layer.fieldSelector->setItemData(index, row.tooltip, Qt::ToolTipRole);
    }
}

bool MainWindow::openSessionHasCurrentDefinitions() const
{
    return primary().session
        && primary().session->derivedFieldDefinitions()
            == m_derivedFields->definitions();
}

void MainWindow::reloadIfDefinitionsMoved()
{
    if (m_closing || !m_derivedFields->available()) {
        return;
    }
    // The primary first: its reload bumps the generation a companion load
    // checks, so the companion is asked once the primary has the list -- from
    // the reload's completion, which lands here again.
    if (openSessionHasCurrentDefinitions()) {
        reloadCompanionIfDefinitionsMoved();
        return;
    }
    // Once per list per session. This is asked on every frame a sequence
    // displays, so without a memo a list the load cannot install would reopen
    // the dataset on every event-loop turn until the session hit its dataset
    // limit. Pairing the list with the session epoch is what keeps the memo
    // from becoming a trap: installing anything at all makes it stale, so it
    // can only ever suppress a retry that would be asking the same question of
    // the same session. It also replaces the separate in-flight flag, which
    // guarded a single event-loop turn rather than the reload.
    if (m_reloadAskedFor
        && m_reloadAskedFor->first == m_derivedFields->definitions()
        && m_reloadAskedFor->second == primary().sessionEpoch) {
        return;
    }
    m_reloadAskedFor = {m_derivedFields->definitions(), primary().sessionEpoch};
    // Not now: the frame path is called from inside the sequence controller's
    // own load completion, which sets m_inFlight and emits frameDisplayed
    // after this returns -- starting a load from here would have it clobber
    // the load this reload starts.
    QTimer::singleShot(0, this, [this] {
        if (m_closing || !m_derivedFields->available()
            || openSessionHasCurrentDefinitions()) {
            return;
        }
        // Dropped again if the reload stood aside: mid-playback the frames
        // carry the list themselves and setPlaybackMode asks once more when
        // they stop, so recording a reload that never ran is what left the
        // definition uninstalled with the editor reporting it applied.
        if (!reloadCurrentDataset()) {
            m_reloadAskedFor.reset();
        }
    });
}

std::shared_ptr<remote::RemoteDatasetSession>
MainWindow::openRemoteSessionForLoad(
    const std::shared_ptr<remote::Connection>& connection,
    const std::string& remotePath,
    const std::vector<DerivedFieldDefinition>& derivedFields,
    StopToken cancellation)
{
    return remote::RemoteDatasetSession::open(connection, remotePath,
        initialCacheBudget(), cancellation,
        connection && connection->supportsDerivedFields()
            ? derivedFields
            : std::vector<DerivedFieldDefinition>{});
}

InitialSliceResult MainWindow::loadRemoteFrame(
    const std::shared_ptr<remote::Connection>& connection,
    const std::string& remotePath, std::uint64_t connectionGeneration,
    const FrameSliceSpec& spec, StopToken cancellation)
{
    // Foreground loads and prefetches share the session's connection, which
    // multiplexes their requests. There is no reconnect: the connection lives
    // as long as the ssh session, and a lost session is reported to the user
    // rather than silently re-established -- said here, because "not
    // connected" is the message a user can act on and a bare transact failure
    // is not.
    if (!connection || !connection->connected()) {
        throw std::runtime_error("remote session is not connected: "
            + (connection ? connection->disconnectReason()
                          : std::string("no connection")));
    }
    auto session = openRemoteSessionForLoad(
        connection, remotePath, spec.derivedFields, cancellation);
    auto result = executeSessionFrameLoad(
        std::move(session), spec, cancellation);
    result.connectionGeneration = connectionGeneration;
    return result;
}

bool MainWindow::derivedFieldsReachNextLoad() const
{
    // A remote sequence carries them only when the server it was opened on can
    // install them (protocol 1.4); a FAB drilled out of a MultiFab is what the
    // editor is unavailable over -- for the same reason the reload cannot
    // rebuild one.
    return (!m_remoteSequence || m_remoteSequenceDerivedFields)
        && !m_fabNavigator->fabMode();
}

std::array<std::string, 3> MainWindow::vectorFieldNames() const
{
    std::array<std::string, 3> names;
    if (!primary().session) {
        return names;
    }
    const auto& fields = primary().session->metadata().fields;
    const std::array<int, 3> selected{
        m_vectorUField, m_vectorVField, m_vectorWField};
    for (std::size_t axis = 0; axis < names.size(); ++axis) {
        const auto field = selected[axis];
        if (field >= 0 && static_cast<std::size_t>(field) < fields.size()) {
            names[axis] = fields[static_cast<std::size_t>(field)].name;
        }
    }
    return names;
}

void MainWindow::restoreVectorFields(const std::array<std::string, 3>& names)
{
    if (!primary().session) {
        return;
    }
    const auto& fields = primary().session->metadata().fields;
    std::array<int*, 3> selected{
        &m_vectorUField, &m_vectorVField, &m_vectorWField};
    for (std::size_t axis = 0; axis < names.size(); ++axis) {
        auto* field = selected[axis];
        if (*field < 0 || names[axis].empty()) {
            continue;
        }
        // By name, and out of range when the name is not here. Leaving the
        // old id would re-point the component at whatever field now holds it:
        // the derived fields are part of this list, so an id outlives the
        // definition it named and means a different one afterwards. Clamping
        // would be worse still -- all three on one field, and looking valid to
        // ensureVectorFieldDefaults, which then skips the re-detection that an
        // out-of-range id is precisely the signal for.
        const auto found = std::find_if(fields.begin(), fields.end(),
            [&names, axis](const FieldMetadata& candidate) {
                return candidate.name == names[axis];
            });
        *field = found != fields.end()
            ? static_cast<int>(std::distance(fields.begin(), found))
            : -1;
    }
}

void MainWindow::updateIsoGeometry()
{
    if (!primary().session) {
        return;
    }
    const auto& metadata = primary().session->metadata();
    if (metadata.dimension != 3) {
        return;
    }
    // Physical proportions stretched by the user's factors, as the slice
    // panels are: a 50 km by 300 m ocean is a line otherwise, and its boxes
    // with it. Volume rendering keeps its own unscaled wireframe.
    const auto domain = datasetSampleBounds(metadata);
    const auto factors = normalizedAxisScale(m_axisScale);
    m_isoWidget->setGeometry(metadata,
        [domain, factors](std::size_t, const Real3& point) {
            return axisScaledDisplayPoint(domain, factors, point);
        });
}

void MainWindow::publishSlicePositions()
{
    m_isoWidget->setSlicePositions(m_slicePosition3d[0], m_slicePosition3d[1],
        m_slicePosition3d[2]);
    m_volumeController->slicePositionsChanged();
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
    if (!primary().session) {
        setSlicePositionControlsVisible(false);
        return;
    }
    setSlicePositionControlsVisible(true);
    const auto& md = primary().session->metadata();

    if (md.dimension != 3) {
        // 2-D: dim rather than hide — there is no slice depth to control,
        // but the user can see Position is a 3-D-only concept.
        m_slicePositionControls->setEnabled(false);
        return;
    }

    if (m_pair) {
        // Two datasets: stacked finest rows along the perpendicular axis, the
        // union at the reference cell along the shared ones.
        m_slicePositionControls->setEnabled(true);
        for (int axis = 0; axis < 3; ++axis) {
            const auto a = static_cast<std::size_t>(axis);
            auto* spin = m_sliceSpinboxes[a];
            const QSignalBlocker blocker(spin);
            const bool perpendicular = axis == m_pair->perpendicularAxis;
            spin->setRange(0, (perpendicular ? m_pair->stackedCellCount()
                                             : m_pair->unionCellCount(axis)) - 1);
            spin->setSingleStep(1);
            spin->setValue(perpendicular
                ? m_pair->stackedIndexForPosition(m_slicePosition3d[a])
                : m_pair->unionIndexForPosition(axis, m_slicePosition3d[a]));
        }
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
    if (!primary().session || primary().session->metadata().dimension != 3) {
        return -1;
    }
    const auto levelData = primary().levelSelector->currentData().toInt();
    return decodeLevelData(levelData, primary().session->metadata().finestLevel).maximumLevel;
}

void MainWindow::setSlicePosition(int axis, double value)
{
    if (!primary().session || primary().session->metadata().dimension != 3) {
        return;
    }
    const auto ax = static_cast<std::size_t>(axis);
    // With a companion the position ranges over both domains together.
    const auto domain = m_pair ? m_pair->unionBounds
                               : datasetSampleBounds(primary().session->metadata());
    const auto position = std::clamp(value, domain.lower[ax],
        std::nextafter(domain.upper[ax], domain.lower[ax]));
    m_slicePosition3d[ax] = position;
    {
        const QSignalBlocker blocker(m_sliceSpinboxes[ax]);
        const auto level = sliceIndexLevel();
        if (m_pair) {
            m_sliceSpinboxes[ax]->setValue(axis == m_pair->perpendicularAxis
                ? m_pair->stackedIndexForPosition(position)
                : m_pair->unionIndexForPosition(axis, position));
        } else if (level >= 0 && static_cast<std::size_t>(level)
            < primary().session->metadata().levels.size()) {
            m_sliceSpinboxes[ax]->setValue(sliceIndexForPosition(
                primary().session->metadata(), level, axis, position));
        }
    }
    publishSlicePositions();
    // The cached full-domain Visible range is now stale — and so is any
    // pending deferred store, whose union was computed from pre-move planes.
    m_displayCoordinator.invalidateRangeCache();
    for (auto& layer : m_layers) {
        layer.pendingRangeStore.reset();
    }
    // The other two views only need their crosshair guides redrawn; the view
    // normal to the moved axis gets a fresh (debounced) slice.
    updateCrosshairs();
    // The layer the position just left keeps the slice it shows: hidden
    // now, a slice at its face would only replace the tile held on show
    // until the incoming layer's lands (updateShownLayers). It slices again
    // when the position comes back into it.
    for (auto* state : statesForPanel(axis)) {
        if (m_pair && !stateShown(*state)) {
            continue;
        }
        scheduleSliceRequest(*state);
    }
    updateShownLayers();
}

void MainWindow::scheduleSliceRequest(bool rasterDirty)
{
    if (m_controlsReady && primary().session) {
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
        // A zoom or pan lands here, and with the volume window limited to what
        // the views show it has to follow. Only a region that actually moved
        // renders; most calls through here have not moved it.
        m_volumeController->regionChanged();
    }
}

void MainWindow::scheduleSliceRequest(PlaneViewState& state, bool rasterDirty)
{
    if (m_controlsReady && primary().session) {
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
        m_volumeController->regionChanged();
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
    // A settle withheld for this queue (settleIfDrained) whose flush started
    // nothing -- the requests were refused, or nothing was queued after all.
    if (m_settleDeferred && m_diagnosticsModel->activeRequests() == 0
        && !sliceRequestQueued()) {
        m_settleDeferred = false;
        emit interactiveSlicesSettled();
    }
}

void MainWindow::requestSlice(PlaneViewState& state, bool rasterDirty)
{
    if (!m_controlsReady || !layerFor(state).session
        || layerFor(state).fieldSelector->currentIndex() < 0
        || layerFor(state).levelSelector->currentIndex() < 0) {
        return;
    }
    updateRangeModeAvailability(layerFor(state));

    const auto dataset = layerFor(state).session;
    const auto& metadata = dataset->metadata();
    SliceRequest request;
    request.dataset = dataset->id();
    request.field.value = layerFor(state).fieldSelector->currentData().toUInt();
    request.normalDirection = state.normal;
    if (metadata.dimension == 3) {
        request.physicalPosition
            = m_slicePosition3d[static_cast<std::size_t>(state.normal)];
        if (m_pair) {
            // The shared position ranges over both domains; a layer whose
            // domain it has left keeps a slice at its nearest face rather
            // than an empty one, ready for when the position comes back.
            const auto bounds = datasetSampleBounds(metadata);
            const auto axis = static_cast<std::size_t>(state.normal);
            request.physicalPosition = std::clamp(request.physicalPosition,
                bounds.lower[axis],
                std::nextafter(bounds.upper[axis], bounds.lower[axis]));
        }
    }
    request.visibleRegion = state.visibleRegion.value_or(
        datasetSampleBounds(metadata));
    request.outputSize = sliceOutputSize(state);
    const auto level = layerFor(state).levelSelector->currentData().toInt();
    const auto [composition, maximumLevel] = decodeLevelData(
        level, metadata.finestLevel);
    request.composition = composition;
    request.maximumLevel = maximumLevel;
    request.includeGridBoxes = m_boxesAction->isChecked();
    request.sphericalDisplay = m_sphericalDisplay;
    const auto wanted = requestedWarpFor(state);
    request.mappedGrid = wanted == DisplayWarp::MappedGrid;
    if (isWarped(wanted)) {
        // The warp is drawn for the window the viewport shows, at its own
        // device pixels (updateMappedDemand); until the view has asked for
        // one -- the first warped arrival -- over the whole node bounding
        // box, sized to the viewport, which Fit then frames. The canvas the
        // view lays that out on is asked for once, with the first arrival.
        const bool windowed = state.warp == wanted && hasMappedWindow(state);
        request.displayWindow = windowed ? state.mappedWindow : RealBox{};
        request.displayPixels = windowed ? state.mappedWindowPixels
                                         : viewportPixelSize(state);
        request.wantMappedDomainBounds = !state.mappedCanvasBounds.has_value();
    }

    const auto selection = layerFor(state).range->selection();
    auto rangeMode = effectiveRangeMode(dataset, request.field,
        maximumLevel, composition, selection.mode);
    std::optional<std::pair<double, double>> userRange;
    if (rangeMode == RangeMode::User) {
        userRange = selection.userRange;
    }
    if (m_pair && state.layer == 1 && m_companionFollowsPrimary) {
        // "Same as primary": the primary's displayed range on this panel, as
        // a fixed range; before the primary has rendered, its own File range.
        const auto& primaryState
            = primary().planeViews[static_cast<std::size_t>(state.normal)];
        if (primaryState.plane->width > 0
            && primaryState.displayMinimum < primaryState.displayMaximum) {
            rangeMode = RangeMode::User;
            userRange = std::pair{primaryState.displayMinimum,
                primaryState.displayMaximum};
        }
    }
    // A following companion also takes the mapping the primary rendered
    // with: the same bounds drawn linear under a logarithmic bar would lie.
    const auto logarithmic = m_pair && state.layer == 1 && m_companionFollowsPrimary
            && primary().planeViews[static_cast<std::size_t>(state.normal)].plane->width > 0
        ? primary().planeViews[static_cast<std::size_t>(state.normal)].displayLogarithmic
        : selection.logarithmic;
    const auto palette = m_paletteController->palette();
    // The primary's vector field ids mean nothing in a companion's field
    // list, so a companion renders its raster (and contours) without glyphs.
    const auto displayMode = state.layer == 1
            && m_displayMode == DisplayMode::VelocityVectors
        ? DisplayMode::Raster : m_displayMode;
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
        && (!isContourMode(displayMode) || state.contourPlane->width > 0)
        && (displayMode != DisplayMode::VelocityVectors
            || (!state.vectorSegments.empty()
                && contourCount == state.cachedContourCount
                // Cached glyphs are layout-specific for a spherical dataset:
                // R-Z segments carry display (R, Z) coordinates while the
                // logical layouts carry plane pixels, so a display-mode switch
                // must regenerate them.
                && (!displayIsSpherical()
                    || (state.cachedRequest.sphericalDisplay
                            == request.sphericalDisplay)
                    || (state.cachedRequest.sphericalDisplay
                            != SphericalDisplay::RZ
                        && request.sphericalDisplay != SphericalDisplay::RZ))));

    // Switching the warp on or off changes the pixmap, a warped view whose
    // warp never landed (a refresh overtook the request that carried it)
    // still shows the logical raster, and a warped refresh for a window or
    // pixel count other than the pixmap's (a contour change overtaking a
    // redraw) would keep a raster drawn for somewhere else: all need the
    // raster drawn, whatever cosmetic change asked for this refresh.
    rasterDirty = rasterDirty || wanted != state.warp
        || (isWarped(wanted) && !state.displaySourceIndex
            && state.view->hasImage())
        || (isWarped(wanted) && state.hasCachedRequest
            && (state.cachedRequest.displayWindow != request.displayWindow
                || state.cachedRequest.displayPixels != request.displayPixels));

    state.stopSource.request_stop();
    state.stopSource = StopSource{};
    const auto cancellation = state.stopSource.get_token();
    const auto generation = m_generation;
    // The session this request is about to be computed against. A reload leaves
    // the outgoing one installed while its replacement loads, so this is what
    // tells an arrival apart from the session that is current when it lands.
    const auto sessionEpoch = layerFor(state).sessionEpoch;
    const auto sliceGeneration = ++state.sliceGeneration;
    ++state.pendingRequests;
    m_diagnosticsModel->adjustActivity(1);
    const auto tag = m_viewDimension == 3
        ? tr(" (%1)").arg(state.label) : QString();
    statusBar()->showMessage(tr("Loading %1%2...").arg(
        layerFor(state).fieldSelector->currentText(), tag));
    updateDiagnostics();

    QFuture<SliceDisplayResult> future;
    if (fromCache) {
        // Cheap path: re-range, re-render, and re-contour the cached planes
        // on a worker; no SliceQuery runs at all. The captures are shared_ptr
        // snapshots — refcount bumps, not a plane deep copy. The immutable
        // display plane is passed straight through by shared_ptr and adopted by
        // showSlice, so the former ~110 MB copy per range/log/palette tweak is
        // gone. A newer arrival can safely replace the view's pointers
        // meanwhile; this worker keeps reading its own snapshots. (The
        // contour-resolution plane is still deref'd into a by-value copy at
        // the call below -- ~14 MB; see SliceDisplayResult::reusedPlane for
        // why it is not yet reused.)
        future = QtConcurrent::run([dataset, request,
            displayPlane = state.plane,
            contourPlane = state.contourPlane,
            vectors = state.vectorSegments,
            gridNodes = state.gridNodes,
            rangeMode, userRange, logarithmic, palette, displayMode,
            vectorUField, vectorVField, contourCount, rasterDirty,
            cancellation]() mutable {
#ifdef AMREXPLORER_QT_TEST_ACCESS
            slice_worker_test::waitAtGate();
#endif
            return refreshCachedSlice(dataset, request, std::move(displayPlane),
                *contourPlane, std::move(vectors), std::move(gridNodes),
                rangeMode, userRange,
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
        [this, watcher, dataset, generation, sliceGeneration, sessionEpoch,
         cancellation, &state, rangeMode] {
            --state.pendingRequests;
            m_diagnosticsModel->adjustActivity(-1);
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                // takeResult, not result(): result() returns a reference into
                // the future and copying out of it duplicates every plane in
                // the arrival before showSlice has even seen it.
                auto result = watcher->future().takeResult();
                // The session too, by the epoch captured at submission
                // rather than by comparing the pointer: `dataset == layerFor(state).session`
                // reads like a staleness test but is really a test of whether
                // the reload's completion has run yet, since layerFor(state).session is only
                // swapped there. Both forms accept in the common ordering,
                // where this slice finishes before the reload installs -- and
                // it is right to, because that session is still the installed
                // one. What makes the display consistent is the other half of
                // the invariant: the reload re-slices the views whose display
                // it skipped, so a raster stamped with a session that has since
                // been replaced does not stay on screen.
                if (generation == m_generation
                    && sliceGeneration == state.sliceGeneration
                    && sessionEpoch == layerFor(state).sessionEpoch) {
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
                        rangeCacheDataset(state.layer, result.request.dataset),
                        result.request.field, result.request.maximumLevel,
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
                            *cachedRange, m_paletteController->palette(), m_viewDimension != 3);
                    }
                    // showSlice takes the arrival by value; the fallback levels
                    // are still needed below, so copy them out first rather
                    // than reading them back off a moved-from result.
                    const auto fallbackToLevel = result.cacheFallbackToLevel;
                    const auto fallbackFromLevel = result.cacheFallbackFromLevel;
                    showSlice(state, std::move(result), sessionEpoch);
                    // Cache the full-domain range. In 3-D the store defers to
                    // the (async) shared-range sync's completion so the union
                    // across all panels is captured; 2-D has no later sync
                    // and stores the panel's own range immediately.
                    if (isFullDomain && rangeMode == RangeMode::Visible
                        && state.plane->width > 0) {
                        if (m_viewDimension == 3) {
                            layerFor(state).pendingRangeStore = rangeKey;
                        } else {
                            m_displayCoordinator.storeFullDomainRange(rangeKey,
                                {state.displayMinimum, state.displayMaximum});
                        }
                    }
                    const auto cache = dataset->cacheMetrics();
                    m_diagnosticsModel->setCacheMetrics(cache);
                    // A cache-pressure fallback lowered the composite level;
                    // reflect it in the level combo (no re-slice) and inform the
                    // user, matching the initial-load handling.
                    if (fallbackToLevel >= 0) {
                        if (selectCacheFallbackLevel(
                                layerFor(state).levelSelector, fallbackToLevel)) {
                            configureSlicePositionControls();
                            updateRangeModeAvailability();
                            syncMenuChecks();
                            // The combo moved behind a signal blocker, so the
                            // volume window is not told by the usual
                            // currentIndexChanged: without this its frame and
                            // its "level N" label keep the level the slices
                            // just fell back from.
                            m_volumeController->refresh();
                        }
                        statusBar()->showMessage(cacheFallbackMessage(
                            *dataset, fallbackFromLevel, fallbackToLevel));
                    }
                } else {
                    m_diagnosticsModel->noteStaleResult();
                }
            } catch (const std::exception& error) {
                // The same three tests as the success arm: a slice the window
                // has decided is not current must not raise an error either.
                // A reopen that pushes the connection past a server limit, or
                // a budget the outgoing session's cache cannot meet, would
                // otherwise report a failure for work already superseded.
                if (generation == m_generation
                    && sliceGeneration == state.sliceGeneration
                    && sessionEpoch == layerFor(state).sessionEpoch
                    && !cancellation.stop_requested()) {
                    reportBackgroundError(
                        tr("Cannot load slice: %1").arg(exceptionMessage(error)));
                } else {
                    m_diagnosticsModel->noteStaleResult();
                }
            }
            // Dispatch the 3-D shared-range sync after the try/catch, not inside
            // the success path: under single-flight only the settling arrival
            // dispatches, so a throwing arrival must not leave the batch without
            // its sync. syncVisibleRanges no-ops outside 3-D Visible and defers
            // while slices are still in flight, so calling it on every arrival
            // (stale or thrown included) is safe. It is now outside the try
            // above, so guard it too: a bad_alloc while allocating the sync
            // worker must not escape this slot (syncVisibleRanges itself resets
            // its own in-flight state before any such throw).
            try {
                syncVisibleRanges();
            } catch (const std::exception& error) {
                // The shared-range sync could not be scheduled; surface it (as
                // the arrival path above does) rather than fail silently. The
                // panels keep their per-view ranges until the next arrival
                // retries.
                reportVisibleSyncFailure(error);
            }
            updateDiagnostics();
            // A layer switch waiting on this slice (updateShownLayers) is
            // finished by showSlice; an arrival that never got there -- stale,
            // failed -- must not leave the outgoing tile on show. Visibility
            // only: a failed slice asked for again would fail again.
            if (m_pair) {
                updateShownLayers(/*sliceNewlyShown=*/false);
            }
            watcher->deleteLater();
            settleIfDrained();
        });
    watcher->setFuture(future);
}

void MainWindow::updateGridBoxes(PlaneViewState& state)
{
    std::vector<GridBoxOverlay> overlays;
    if (!m_boxesAction->isChecked() || !layerFor(state).session || !state.view->hasImage()
        || state.plane->width <= 0 || state.plane->height <= 0) {
        state.view->setGridBoxes(overlays, state.tile);
        return;
    }

    const auto& metadata = layerFor(state).session->metadata();
    const auto& plane = *state.plane;
    const auto axes = displayAxes(state.normal);
    const auto rawLevel = layerFor(state).levelSelector->currentData().toInt();
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
                m_paletteController->palette().levelColor(levelIndex, lastLevel)));
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
        if (state.warp == DisplayWarp::MappedGrid) {
            // The box's logical rectangle, in fractional plane pixels, drawn
            // along the stretched cell edges it maps to.
            if (!(xUpper > xLower) || !(yUpper > yLower)) {
                continue;
            }
            const auto col0 = std::clamp(
                (xLower - plane.physicalRegion.lower[xAxis]) / xExtent
                    * plane.width, 0.0, static_cast<double>(plane.width));
            const auto col1 = std::clamp(
                (xUpper - plane.physicalRegion.lower[xAxis]) / xExtent
                    * plane.width, 0.0, static_cast<double>(plane.width));
            const auto row0 = std::clamp(
                (yLower - plane.physicalRegion.lower[yAxis]) / yExtent
                    * plane.height, 0.0, static_cast<double>(plane.height));
            const auto row1 = std::clamp(
                (yUpper - plane.physicalRegion.lower[yAxis]) / yExtent
                    * plane.height, 0.0, static_cast<double>(plane.height));
            if (!(col1 > col0) || !(row1 > row0)) {
                continue;
            }
            GridBoxOverlay overlay;
            overlay.color = color;
            overlay.path = mappedCellPath(mapping, col0, col1, row0, row1);
            overlays.push_back(std::move(overlay));
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
    state.view->setGridBoxes(overlays, state.tile);
}

void MainWindow::updateGridBoxes()
{
    for (auto* state : currentViews()) {
        updateGridBoxes(*state);
    }
}

void MainWindow::updateScaleBar(PlaneViewState& state)
{
    double horizontalWidthCodeUnits = 0.0;
    if (m_scaleBarAction->isEnabled() && m_scaleBarAction->isChecked()
        && layerFor(state).session && state.view->hasImage()
        && layerFor(state).session->metadata().hasPhysicalGeometry
        && !(displayIsSpherical()
            && state.sphericalDisplay == SphericalDisplay::ThetaR)) {
        const auto horizontalAxis = displayIsSpherical()
            ? std::size_t{0}
            : static_cast<std::size_t>(displayAxes(state.normal)[0]);
        horizontalWidthCodeUnits = state.displayRegion.upper[horizontalAxis]
            - state.displayRegion.lower[horizontalAxis];
    }
    state.view->setScaleBarWidth(horizontalWidthCodeUnits,
        lengthUnitFromId(m_lengthUnitId.toStdString()));
}

void MainWindow::updateScaleBars()
{
    for (auto* state : currentViews()) {
        updateScaleBar(*state);
    }
}

void MainWindow::updateScaleBarAvailability()
{
    // A horizontal bar states one length per screen pixel; it is offered
    // only while that holds vertically too. The saved preference survives a
    // dataset or aspect setting on which the bar is withheld.
    // Withheld over two datasets: the bar would speak for one of them.
    bool available = primary().session && !m_pair
        && displayIsPhysicallyIsotropic(
            primary().session->metadata(), displayStretchPerAxis());
    {
        const QSignalBlocker blocker(m_scaleBarAction);
        m_scaleBarAction->setChecked(available && m_scaleBarVisible);
    }
    m_scaleBarAction->setEnabled(available);
}

void MainWindow::updateCrosshairs(PlaneViewState& state)
{
    std::optional<QLineF> vertical;
    std::optional<QLineF> horizontal;
    QColor verticalColor;
    QColor horizontalColor;
    if (layerFor(state).session && layerFor(state).session->metadata().dimension == 3
        && m_slicePlanesAction->isChecked()
        && state.plane->width > 0 && state.plane->height > 0) {
        const auto axes = displayAxes(state.normal);
        const auto xAxis = static_cast<std::size_t>(axes[0]);
        const auto yAxis = static_cast<std::size_t>(axes[1]);
        // A mapped pixmap is physical over displayRegion and sized by the
        // warp, not one pixel per plane cell; the guides live in pixmap
        // pixels. The stretched axis (the last one) is then a curve at a
        // constant logical coordinate, which a straight guide cannot
        // follow, so that guide is left out; the horizontal in-plane axes
        // keep theirs.
        const bool mapped = state.warp == DisplayWarp::MappedGrid && state.view->hasImage();
        const auto& region = mapped ? state.displayRegion
                                    : state.plane->physicalRegion;
        const auto width = mapped
            ? static_cast<double>(std::max(1, state.view->image(state.tile).width()))
            : static_cast<double>(state.plane->width);
        const auto height = mapped
            ? static_cast<double>(std::max(1, state.view->image(state.tile).height()))
            : static_cast<double>(state.plane->height);
        const auto lastAxis = static_cast<std::size_t>(
            layerFor(state).session->metadata().dimension - 1);
        // The vertical guide marks the slice position of the axis pointing
        // horizontally in this view, and vice versa; each guide takes that
        // axis' legacy palette color and hides outside the displayed region.
        const auto xPosition = m_slicePosition3d[xAxis];
        if (xPosition >= region.lower[xAxis] && xPosition <= region.upper[xAxis]
            && !(mapped && xAxis == lastAxis)) {
            const auto t = (xPosition - region.lower[xAxis])
                / (region.upper[xAxis] - region.lower[xAxis]);
            vertical = QLineF(t * width, 0.0, t * width, height);
            verticalColor = sliceAxisColor(axes[0]);
        }
        const auto yPosition = m_slicePosition3d[yAxis];
        if (yPosition >= region.lower[yAxis] && yPosition <= region.upper[yAxis]
            && !(mapped && yAxis == lastAxis)) {
            const auto t = (yPosition - region.lower[yAxis])
                / (region.upper[yAxis] - region.lower[yAxis]);
            const auto sceneY = height * (1.0 - t);
            horizontal = QLineF(0.0, sceneY, width, sceneY);
            horizontalColor = sliceAxisColor(axes[1]);
        }
    }
    state.view->setCrosshairs(vertical, horizontal, verticalColor,
        horizontalColor, state.tile);
}

void MainWindow::updateCrosshairs()
{
    for (auto* state : currentViews()) {
        updateCrosshairs(*state);
    }
}

void MainWindow::appendMetadataRows(QTreeWidgetItem* root,
    const PlotfileMetadataResult& result, const std::filesystem::path& path,
    const std::shared_ptr<DatasetSession>& session)
{
    const auto& metadata = *result.metadata;
    const auto newTopLevel = [this, root](const QStringList& columns) {
        return root != nullptr ? new QTreeWidgetItem(root, columns)
                               : new QTreeWidgetItem(m_metadataTree, columns);
    };
    const auto addValue = [&newTopLevel](const QString& name, const QString& value) {
        newTopLevel({name, value});
    };

    // Standalone FABs and MultiFabs carry neither a simulation time nor an
    // AMR hierarchy, so those rows (and the per-level listing below) would
    // show invented values; they are skipped for such data.
    const bool standalone = !metadata.hasPhysicalGeometry;
    const auto dimension = static_cast<std::size_t>(
        std::clamp(metadata.dimension, 1, 3));
    // Per-axis values as one space-separated row, so a domain corner or a
    // cell size reads as the vector it is.
    const auto realTriple = [dimension](const auto& values) {
        QStringList parts;
        for (std::size_t axis = 0; axis < dimension; ++axis) {
            parts << QString::number(values[axis], 'g', 17);
        }
        return parts.join(QLatin1Char(' '));
    };
    const auto intTriple = [dimension](const auto& values) {
        QStringList parts;
        for (std::size_t axis = 0; axis < dimension; ++axis) {
            parts << QString::number(values[axis]);
        }
        return parts.join(QLatin1Char(' '));
    };
    addValue(tr("Dataset"), QString::fromStdString(path.string()));
    addValue(tr("Format"), QString::fromStdString(result.fileVersion));
    addValue(tr("Dimension"), QString::number(metadata.dimension));
    if (!standalone) {
        addValue(tr("Time"), QString::number(metadata.time, 'g', 17));
        addValue(tr("Finest level"), QString::number(metadata.finestLevel));
        QString coordinates;
        switch (static_cast<CoordinateSystem>(metadata.coordinateSystem)) {
        case CoordinateSystem::Cartesian: coordinates = tr("Cartesian"); break;
        case CoordinateSystem::Cylindrical:
            coordinates = tr("Cylindrical (R-Z)");
            break;
        case CoordinateSystem::Spherical:
            coordinates = tr("Spherical (r-%1)").arg(QChar(0x03B8));
            break;
        }
        if (coordinates.isEmpty()) {
            coordinates = QString::number(metadata.coordinateSystem);
        }
        addValue(tr("Coordinate system"), coordinates);
        if (metadata.hasMappedGrid) {
            QStringList names;
            if (result.mappedGrid) {
                for (const auto& field : result.mappedGrid->fields) {
                    names << QString::fromStdString(field.name);
                }
            } else if (const auto remote = std::dynamic_pointer_cast<
                           remote::RemoteDatasetSession>(session)) {
                // A remote catalog names the components without the grid's
                // own metadata.
                for (const auto& name : remote->mappedGridComponentNames()) {
                    names << QString::fromStdString(name);
                }
            }
            addValue(tr("Mapped grid"), names.isEmpty()
                ? tr("Yes") : tr("Yes (%1)").arg(names.join(QLatin1Char(' '))));
        }
        Real3 extent;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            extent[axis] = metadata.physicalDomain.upper[axis]
                - metadata.physicalDomain.lower[axis];
        }
        auto* domain = newTopLevel({tr("Domain"), realTriple(extent)});
        new QTreeWidgetItem(domain,
            {tr("Lower"), realTriple(metadata.physicalDomain.lower)});
        new QTreeWidgetItem(domain,
            {tr("Upper"), realTriple(metadata.physicalDomain.upper)});
    }

    auto* fields = newTopLevel({tr("Fields"), QString::number(metadata.fields.size())});
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
        auto* levels = newTopLevel({tr("Levels"), QString::number(metadata.levels.size())});
        for (std::size_t index = 0; index < metadata.levels.size(); ++index) {
            const auto& level = metadata.levels[index];
            auto* item = new QTreeWidgetItem(levels, {
                tr("Level %1").arg(level.level),
                tr("%1 grid(s), %2").arg(level.boxes.size()).arg(
                    QString::fromStdString(level.dataPath))
            });
            Int3 cells;
            Int3 lower = level.domain.lower;
            Int3 upper = level.domain.upper;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                // The index domain is cell-centred in a plotfile Header; a
                // nodal axis holds one more index than it has cells.
                cells[axis] = upper[axis] - lower[axis] + 1
                    - (level.domain.centering[axis] != 0 ? 1 : 0);
            }
            new QTreeWidgetItem(item, {tr("Cells"), intTriple(cells)});
            new QTreeWidgetItem(item, {tr("Index domain"),
                tr("(%1) to (%2)").arg(intTriple(lower), intTriple(upper))});
            new QTreeWidgetItem(item,
                {tr("Cell size"), realTriple(level.cellSize)});
            if (index > 0) {
                // The Header's ratios are one integer per level; the ratio of
                // cell sizes gives the same thing per axis.
                const auto& coarser = metadata.levels[index - 1];
                Int3 ratio;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const auto fine = level.cellSize[axis];
                    ratio[axis] = fine > 0.0
                        ? static_cast<int>(std::lround(
                            coarser.cellSize[axis] / fine))
                        : 0;
                }
                new QTreeWidgetItem(item,
                    {tr("Refinement ratio"), intTriple(ratio)});
            }
            new QTreeWidgetItem(item,
                {tr("Step"), QString::number(level.step)});
        }
    }
}

void MainWindow::showMetadata(
    const PlotfileMetadataResult& result, const std::filesystem::path& path)
{
    m_metadataTree->clear();
    const auto& companion = m_layers[1];
    if (companion.active && companion.session) {
        // Two datasets: each under its own parent row, named as the toolbars
        // name them.
        auto* primaryRoot = new QTreeWidgetItem(m_metadataTree,
            {tr("Primary"), QString::fromStdString(path.filename().string())});
        appendMetadataRows(primaryRoot, result, path, primary().session);
        PlotfileMetadataResult companionResult;
        companionResult.metadata = std::make_shared<const DatasetMetadata>(
            companion.session->metadata());
        companionResult.metrics = companion.session->metadataReadMetrics();
        companionResult.fileVersion = companion.session->fileVersion();
        auto* companionRoot = new QTreeWidgetItem(m_metadataTree,
            {tr("Companion"), companion.name});
        appendMetadataRows(companionRoot, companionResult, companion.path,
            companion.session);
    } else {
        appendMetadataRows(nullptr, result, path, primary().session);
    }
    m_metadataTree->expandAll();

    primary().openMetadata = result.metadata;
    primary().fileVersion = result.fileVersion;
    updateWindowTitle();

    const auto& metadata = *result.metadata;
    const bool standalone = !metadata.hasPhysicalGeometry;
    m_diagnosticsModel->setMetadataMetrics(
        result.metrics.filesRead, result.metrics.bytesRead);
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
    // Clamped to the raster that actually arrived. The window is the *viewport*
    // mapped through the old plane, and after a rubber-band zoom the viewport
    // shows more than the selection: the feedback zoom fits the selection with
    // KeepAspectRatio, which pads the slack axis. Framing that padded window
    // over a raster that stops at the selection leaves the raster short of the
    // pane -- the gap that only the next pan corrected, by refitting.
    //
    // There is nothing outside the raster to show anyway, so clamping is the
    // whole fix, and it is a no-op for the case this function exists for: a
    // density change mid-view preserves a window that lies *inside* the new
    // raster. Local rubber-band zooms never reach here at all, since their
    // density is unchanged -- which is why they never showed the gap.
    const QRectF rasterBounds(0.0, 0.0,
        static_cast<double>(incoming.width),
        static_cast<double>(incoming.height));
    const auto clamped = window.intersected(rasterBounds);
    if (clamped.isEmpty()) {
        return std::nullopt;
    }
    return clamped;
}

void MainWindow::showSlice(PlaneViewState& state, SliceDisplayResult display,
    std::uint64_t sessionEpoch)
{
    // A refresh that kept the pixmap (rasterUnchanged) can say "mapped" of a
    // pixmap that is not the warp: the request carrying the warp was
    // overtaken before it landed. Such an arrival is shown as the logical
    // raster it sits on, and the warp is asked for again.
    const bool flatPixmap = isWarped(display.warp) && display.rasterUnchanged
        && !display.displaySourceIndex
        && !(isWarped(state.warp) && state.displaySourceIndex);
    if (flatPixmap) {
        display.warp = DisplayWarp::None;
        display.gridNodes.reset();
        // The pixmap on screen is the flat raster: the logical region, or
        // for a spherical plane the layout that raster was drawn in.
        display.sphericalDisplay = state.sphericalDisplay;
        display.displayRegion = displayIsSpherical()
            ? state.displayRegion : display.displayPlane().physicalRegion;
    }
    // The view changes made while the arrival is installed (a Fit, a
    // stretch) must not ask the warp for a window against the request this
    // arrival replaces; it is asked once below, with the new request cached.
    struct ApplyingArrival {
        bool& flag;
        bool previous;
        explicit ApplyingArrival(bool& value)
            : flag(value), previous(std::exchange(value, true)) {}
        ~ApplyingArrival() { flag = previous; }
    } applying{m_applyingArrival};
    // A warped arrival lays out on its canvas: the node bounding box of the
    // whole domain, brought by the first arrival and grown -- never shrunk --
    // by every later one, so the scene's anchor holds still.
    if (isWarped(display.warp)) {
        auto bounds = display.mappedDomainBounds.value_or(display.mappedBounds);
        if (state.mappedCanvasBounds) {
            for (const auto axis : displayAxes(state.normal)) {
                const auto a = static_cast<std::size_t>(axis);
                bounds.lower[a] = std::min(bounds.lower[a], state.mappedCanvasBounds->lower[a]);
                bounds.upper[a] = std::max(bounds.upper[a], state.mappedCanvasBounds->upper[a]);
            }
        }
        state.mappedCanvasBounds = bounds;
    }
    // Over a pair the node box is the layer's display bounds: a wider one
    // re-lays out the panel and re-places the other tile, once.
    if (m_pair && m_viewDimension == 3 && updatePairLayouts()) {
        applyPairLayouts();
        // Every other warped tile moved with the layout (a node box grown on
        // one panel's axis moves the others' bands too) while arrivals are
        // held back: asked again for what they now show, once this one is in.
        for (auto* other : currentViews()) {
            if (other != &state && isWarped(other->warp)) {
                QTimer::singleShot(0, this, [this, other] { updateMappedDemand(*other); });
            }
        }
    }
    // Before the raster is installed, so a Fit is computed once, with the
    // stretch the raster was sized for: none on a mapped grid, whose tile
    // carries its own placement.
    state.warp = display.warp;
    state.mappedNodeBounds = isWarped(display.warp) ? display.mappedBounds : RealBox{};
    if (!isWarped(display.warp)) {
        // The next warped arrival starts from the whole node box again, not
        // from a window this view showed before it left the warp.
        state.mappedWindow = {};
        state.mappedWindowPixels = {0, 0};
    }
    applyDisplayStretch(state);
    state.view->setSmoothPixmapTransformation(isWarped(display.warp));
    if (flatPixmap) {
        scheduleSliceRequest(state, true);
    }
    if (!display.rasterUnchanged) {
        if (!display.image.valid()) {
            throw std::runtime_error("renderer produced an invalid image");
        }
        // Preserve/Refit/GeometryAware from the cached-vs-incoming request
        // pair; the rationale lives with the decision in the coordinator.
        const auto& metadata = layerFor(state).session->metadata();
        const DisplayCoordinator::RasterGeometry incomingGeometry{
            metadata.physicalDomain, metadata.dimension,
            metadata.coordinateSystem, display.request.normalDirection,
            display.sphericalDisplay, display.warp};
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
            *state.plane, display.displayPlane(), axes);
        // A warped arrival lands on its canvas below and keeps the view
        // as it is; the plane-pixel re-frame does not describe it.
        if (transformPolicy == ImageTransformPolicy::Preserve
            && !isWarped(display.warp)
            && state.view->transformMode()
                == ImageView::TransformMode::Custom
            && (ownerChanged || densityChanged)) {
            dataWindowInNewScene = preservedDataWindow(
                state, display.displayPlane());
        }
        const auto image = displayImageFor(display.image);
        // A view on a virtual canvas keeps it: the raster lands at its
        // cell offset while the scroll position stays put.
        std::optional<ImageView::VirtualPlacement> placement;
        if (state.view->virtualCanvasActive() && !displayIsSpherical()
            && !isWarped(display.warp)) {
            placement = virtualPlacementFor(
                state, display.displayPlane().physicalRegion);
        }
        if (const auto placed = tilePlacement(state)) {
            // A warp of the window it was asked for lands at that window's
            // place on the canvas, a flat raster at its region's; the view's
            // transform and scroll position are untouched (Preserve), a
            // first warped arrival or a switch from the logical grid refits
            // to the canvas. Over a pair the other tile stays where it is.
            // Never setImage: that would drop the canvas.
            const auto region = isWarped(display.warp)
                ? display.displayRegion : display.displayPlane().physicalRegion;
            state.view->setTileImage(state.tile, image,
                toQRectF(placed->sceneRectForRegion(region)),
                toQRectF(placed->canvas), transformPolicy);
            applyPairTileVisibility(state);
        } else {
            state.view->setImage(image, transformPolicy,
                logicalImageSize(state, display.displayPlane(), image),
                placement);
            if (dataWindowInNewScene) {
                state.view->zoomToRect(*dataWindowInNewScene);
            }
        }
        state.rasterGeometry = incomingGeometry;
    }
    // Stamped once the view is showing this session's pixels, not on the way
    // in: ahead of the validity check above, a view that threw before adopting
    // anything still counted as the installed session's, and
    // resliceReplacedViews -- which the failure arm calls for exactly this --
    // would never ask for it again. Anything below that throws has already put
    // the raster on screen, so the stamp belongs here rather than at the end.
    state.planeSessionEpoch = sessionEpoch;
    // Fresh immutable snapshots: replace the pointers, never mutate the
    // pointees a cached-planes refresh worker may still be reading. The cache
    // fast path already holds the plane by shared_ptr, so adopt it directly;
    // the executeSlice path produces a fresh plane to wrap.
    // Copy (not move) the reused pointer: moving it would leave `display` in a
    // state where displayPlane() returns an empty plane, violating its "never
    // empty" contract for anything that reads `display` afterward. The copy is
    // a single shared_ptr refcount bump.
    state.plane = display.reusedPlane
        ? display.reusedPlane
        : std::make_shared<const ScalarPlane>(std::move(display.slice.plane));
    // Stamp the rewrite immediately -- before the contour make_shared calls
    // below, any of which can throw. The stamp is the entire staleness key for
    // the 3-D shared-range sync; it must never lag the plane it stamps, or a
    // sync that rendered the previous plane could be mistaken for current.
    ++state.renderGeneration;
    // Spherical warps the raster into physical (R, Z); overlays and the probe
    // map through displayRegion, which for every other system is just the
    // plane's logical bounds (see PlaneMapping).
    state.coordinateSystem = display.coordinateSystem;
    state.sphericalDisplay = display.sphericalDisplay;
    state.displayRegion = display.displayRegion;
    if (!display.rasterUnchanged) {
        state.pixmapRegion = display.displayRegion;
    }
    // Likewise a warp: the pixmap is physical over displayRegion, and these
    // place plane pixels on it and back. A refresh that kept the pixmap
    // (rasterUnchanged) drew nothing to index, so the index of the pixmap
    // still on screen stays as well.
    const bool keepIndex = isWarped(display.warp) && display.rasterUnchanged
        && !display.displaySourceIndex;
    state.warp = display.warp;
    state.gridNodes = display.warp == DisplayWarp::MappedGrid
        ? (display.gridNodes ? display.gridNodes : state.gridNodes)
        : nullptr;
    if (!keepIndex) {
        state.displaySourceIndex
            = isWarped(display.warp) ? display.displaySourceIndex : nullptr;
    }
    // The plotfile does not declare a length unit. The selected interpretation
    // is applied only while formatting the annotation; this width stays in
    // native coordinates. Theta-r puts an angle on the horizontal axis, so a
    // horizontal length bar would be a lie in that spherical layout.
    updateScaleBar(state);
    state.contourPlane
        = std::make_shared<const ScalarPlane>(std::move(display.contourPlane));
    state.contourPolylines = std::move(display.contourPolylines);
    const auto fieldName = QString::fromStdString(display.fieldName);
    state.fieldName = fieldName;
    const bool rangeMoved = state.displayMinimum != display.minimum
        || state.displayMaximum != display.maximum
        || state.displayLogarithmic != display.logarithmic;
    state.displayMinimum = display.minimum;
    state.displayMaximum = display.maximum;
    state.displayLogarithmic = display.logarithmic;
    state.vectorSegments = std::move(display.vectors);
    // A companion following the primary's range takes the new one.
    if (m_pair && state.layer == 0 && m_companionFollowsPrimary && rangeMoved
        && m_viewDimension == 3 && m_layers[1].active) {
        refreshFollowingCompanion(static_cast<std::size_t>(state.normal),
            state.displayMinimum, state.displayMaximum, state.displayLogarithmic);
    }
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
    // With the new request cached, what the viewport shows is compared with
    // what this warp was drawn for (a first arrival's Fit, a view moved while
    // it was on its way): the ask the guard above held back. Queued: inside a
    // sequence frame's display, a slice request would restart that frame.
    if (isWarped(state.warp)) {
        QTimer::singleShot(0, this, [this, &state] { updateMappedDemand(state); });
    }
    if (m_activeView == &state
        || (m_pair && m_activeView != nullptr
            && m_activeView->normal == state.normal)) {
        // Tracks the active panel; if log was requested but fell back to
        // linear, the checkbox reflects that log did not apply. With a
        // companion the other layer's raster on that panel arrives on its
        // own, so its colour bar is refreshed from here too.
        syncActiveViewColorControls(*m_activeView);
    }
    // The straight-line profile tool works on the logical r-theta / theta-r
    // grid but not on the warped R-Z view, nor on a mapped grid, where a
    // straight screen line is not a constant logical coordinate.
    updateLineToolAvailability(state);
    // The 2-D Spherical menu is available only for spherical datasets;
    // Aspect Ratio for the others.
    updateSphericalControls();
    updateMappedGridControls();
    updateAspectControls();
    updateScaleBarAvailability();
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
    // The Dataset window's marked cell, on the same footing as the overlays
    // above: setImage cleared it, and no selection signal is coming to ask for
    // it again (see applyDatasetCellHighlight).
    applyDatasetCellHighlight(state);

    // setImage demotes Custom to Fit when the coordinator returns Refit -- a
    // spherical r-theta to R-Z switch does it -- so the raster funnel has to
    // restate the scale too, not only the sequence path that calls this in a
    // loop.
    refreshScaleReport();

    m_diagnosticsModel->setSliceMetrics(display.slice.metrics.blocksRead,
        display.slice.metrics.cacheHits, display.slice.metrics.payloadBytesRead);
    if (!display.mappedGridFallback.empty()) {
        const auto message = tr("Mapped grid display is off: %1")
            .arg(QString::fromStdString(display.mappedGridFallback));
        statusBar()->showMessage(state.layer == 1
                ? tr("%1: %2").arg(layerFor(state).name, message) : message);
    } else {
        statusBar()->clearMessage();
    }

    // The region a limited volume render covers is read off this raster, so it
    // is only now that a pan or a rubber-band zoom can be measured: the
    // scheduling call that fetched this plane ran while the previous one was
    // still displayed, and would have measured that.
    m_volumeController->regionChanged();
}

void MainWindow::refreshFollowingCompanion(std::size_t normal, double minimum,
    double maximum, bool logarithmic)
{
    auto& state = m_layers[1].planeViews[normal];
    const bool current = state.plane->width > 0
        && state.displayMinimum == minimum && state.displayMaximum == maximum
        && state.displayLogarithmic == logarithmic;
    if (!current) {
        scheduleSliceRequest(state);
    }
}

void MainWindow::resliceReplacedViews()
{
    if (!m_controlsReady || !primary().session) {
        return;
    }
    for (auto* state : currentViews()) {
        // A companion's views too: a primary reload stopped and dropped
        // their requests along with the primary's (reloadCurrentDataset),
        // though its session stayed, so a position moved just before the
        // Apply would otherwise never reach them.
        if (state->planeSessionEpoch != layerFor(*state).sessionEpoch
            || (state->layer == 1 && m_layers[1].active)) {
            scheduleSliceRequest(*state);
        }
    }
}

int MainWindow::slicesInFlight() const
{
    int total = 0;
    for (const auto& layer : m_layers) {
        if (layer.active) {
            total += slicesInFlight(layer);
        }
    }
    return total;
}

bool MainWindow::sliceRequestQueued() const
{
    // The debounce timer is normally active while a request is queued, but some
    // paths stop it without clearing the queue (see openDataset), so check the
    // pending views too -- "queued behind the debounce" means either.
    return (m_sliceDebounce != nullptr && m_sliceDebounce->isActive())
        || m_pendingAllViews || !m_pendingViews.empty();
}

void MainWindow::settleIfDrained()
{
    // The interactive batch has drained once nothing is in flight AND nothing
    // is queued; the smoke tests wait on this. An arrival can schedule the
    // next request on its way through showSlice (a following companion, a
    // warp asked for the screen): that request sits behind the 100 ms
    // debounce, not in the activity count, so a settle here would come
    // before it ran and a second settle after -- which the companion smoke
    // once saw as "range following kept re-slicing at rest", on slow CI
    // runners only. A withheld settle is sent by flushSliceRequests when the
    // flush issues nothing, and by the completion of what it issues. A frame
    // prefetch can likewise still be running when the last slice lands; its
    // end sends the signal then (loadActivityChanged).
    m_settleDeferred = m_diagnosticsModel->activeRequests() != 0 || sliceRequestQueued();
    if (!m_settleDeferred) {
        emit interactiveSlicesSettled();
    }
}

int MainWindow::slicesInFlight(const DatasetLayer& layer) const
{
    if (m_viewDimension == 2) {
        return &layer == &primary() ? m_view2d.pendingRequests : 0;
    }
    int total = 0;
    for (const auto& state : layer.planeViews) {
        total += state.pendingRequests;
    }
    return total;
}

void MainWindow::syncVisibleRanges()
{
    for (auto& layer : m_layers) {
        if (layer.active) {
            syncVisibleRanges(layer);
        }
    }
}

void MainWindow::syncVisibleRanges(DatasetLayer& layer)
{
    if (m_viewDimension != 3 || !layer.session) {
        return;
    }
    if (layer.range->mode() != RangeMode::Visible) {
        return;
    }
    if (&layer == &m_layers[1] && m_companionFollowsPrimary) {
        // Following the primary: its range is the primary's, not a union of
        // its own panels.
        return;
    }
    // Single-flight: dispatch one sync only once the panel slice batch has
    // settled (no view has a slice on a worker) and no sync is already running.
    // While slices are still arriving -- or a sync is in flight -- defer; the
    // settling arrival, or the running sync's completion, dispatches then,
    // against fully-current panels. Running against a settled batch is what lets
    // the completion apply all-or-nothing (a coherent shared range/log/color bar
    // across all three panels) instead of a stale subset. Gate on panel slice
    // work only (slicesInFlight), never the DiagnosticsModel's global active
    // count -- particle loads, line plots, and sequence prefetch bump that,
    // and would wedge the sync shut for the whole operation.
    if (slicesInFlight(layer) != 0 || layer.visibleSyncInFlight) {
        layer.visibleSyncRerun = true;
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
    const FieldId currentField{layer.fieldSelector->currentData().toUInt()};
    const auto rawLevel = layer.levelSelector->currentData().toInt();
    const auto [composition, maximumLevel] = decodeLevelData(
        rawLevel, layer.session->metadata().finestLevel);
    const auto cachedRange = m_displayCoordinator.cachedFullDomainRange(
        {rangeCacheDataset(&layer == &m_layers[1] ? 1 : 0, layer.session->id()),
            currentField, maximumLevel, composition});

    struct PanelSnapshot {
        std::shared_ptr<const ScalarPlane> plane;
        std::shared_ptr<const ScalarPlane> contourPlane;
        std::array<int, 2> outputSize{0, 0};
        // A panel drawn on the mapped grid is re-coloured and then re-warped
        // through the same nodes and factor, so the re-coloured pixmap keeps
        // the shape (and the source index) of the one it replaces.
        std::shared_ptr<const MappedGridPlane> gridNodes;
        std::array<int, 2> axes{0, 1};
        RealBox displayWindow{};
        std::array<int, 2> displayPixels{0, 0};
    };
    std::array<PlaneViewState*, 3> views{
        &layer.planeViews[0], &layer.planeViews[1], &layer.planeViews[2]};
    std::array<PanelSnapshot, 3> snapshots;
    // Render-generation stamps captured at dispatch, kept separate from the
    // plane snapshots so the completion (which only compares these integers)
    // need not capture the heavy plane shared_ptrs the worker uses.
    std::array<std::uint64_t, 3> snapshotGenerations{};
    for (std::size_t index = 0; index < views.size(); ++index) {
        const auto* state = views[index];
        snapshots[index] = {state->plane, state->contourPlane,
            state->cachedRequest.outputSize,
            state->warp == DisplayWarp::MappedGrid ? state->gridNodes : nullptr,
            displayAxes(state->normal),
            state->cachedRequest.displayWindow,
            state->cachedRequest.displayPixels};
        snapshotGenerations[index] = state->renderGeneration;
    }

    struct SyncOutcome {
        std::optional<DisplayCoordinator::SharedRangeSync> sync;
        std::array<QImage, 3> images;   // display-ready (flipped) rasters
        // A mapped panel's re-warp: the window drawn and its source index,
        // adopted with the image so probe and overlays keep matching it.
        std::array<std::optional<RealBox>, 3> mappedWindows;
        std::array<std::shared_ptr<const std::vector<std::int32_t>>, 3>
            mappedSourceIndices;
    };

    // This dispatch consumes any deferred request; a request that lands while
    // the worker runs re-arms the flag and reruns from the completion below.
    layer.visibleSyncRerun = false;
    const auto generation = m_generation;
    // Allocate the watcher and register the completion before committing to
    // "in flight": if either throws (bad_alloc), the flags stay clean and the
    // (call-site-guarded) exception unwinds without latching the sync shut.
    auto* watcher = new QFutureWatcher<SyncOutcome>(this);
    connect(watcher, &QFutureWatcher<SyncOutcome>::finished, this,
        [this, watcher, generation, snapshotGenerations, views, &layer] {
            layer.visibleSyncInFlight = false;
            m_diagnosticsModel->adjustActivity(-1);
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            // Whether the panels the worker rendered are still the ones on
            // screen: every render-generation stamp captured at dispatch must
            // still match. Shared by the apply below and the failure path.
            const auto stampsCurrent = [&views, &snapshotGenerations] {
                for (std::size_t index = 0; index < views.size(); ++index) {
                    if (views[index]->renderGeneration
                        != snapshotGenerations[index]) {
                        return false;
                    }
                }
                return true;
            };
            const bool current = generation == m_generation
                && m_viewDimension == 3 && layer.session
                && layer.range->mode() == RangeMode::Visible;
            // Everything after the outcome is in hand, on both paths.
            const auto finish = [this, watcher, generation, &layer] {
                if (generation != m_generation) {
                    // Superseded by a new dataset (or frame): the deferred store
                    // key and any armed rerun belong to the old generation.
                    layer.pendingRangeStore.reset();
                    layer.visibleSyncRerun = false;
                }
                updateDiagnostics();
                watcher->deleteLater();
                if (layer.visibleSyncRerun) {
                    layer.visibleSyncRerun = false;
                    // Re-dispatch inside a slot: guard as at the arrival site
                    // so a bad_alloc allocating the next worker cannot escape,
                    // and surface it rather than fail silently.
                    try {
                        syncVisibleRanges(layer);
                    } catch (const std::exception& error) {
                        reportVisibleSyncFailure(error);
                    }
                }
                settleIfDrained();
            };
            // Only the extraction is guarded: takeResult rethrows a worker
            // exception (a failed extrema scan or render), which must not
            // escape the slot. The apply below is deliberately outside it --
            // it is the one region that must not land partially, so a throw
            // there is a crash rather than three panels on mixed state.
            SyncOutcome outcome;
            try {
                outcome = watcher->future().takeResult();
            } catch (const std::exception& error) {
                // The panels keep their per-view state until the next arrival
                // re-syncs (as after a failed dispatch). Surface the failure
                // only if the sync was still current by the same test the
                // apply uses -- mode, dimension, dataset and every panel's
                // stamp; a superseded worker's throw is counted stale, as the
                // arrival path does, not reported as the user's problem.
                if (current && stampsCurrent()) {
                    reportVisibleSyncFailure(error);
                    // This union will never be stored: drop the deferred key
                    // so a later sync -- possibly over a zoomed subregion --
                    // cannot store its own under it as the full-domain range.
                    layer.pendingRangeStore.reset();
                } else {
                    m_diagnosticsModel->noteStaleResult();
                }
                finish();
                return;
            }
            // All-or-nothing, keyed on the render-generation stamp captured per
            // panel at dispatch. The shared range and log flag are joint across
            // all three panels, so they must not land on a subset. If any panel
            // was re-sliced while this sync ran (stamp bumped), the sync rendered
            // it from the previous plane -- and cached-plane reuse means pointer
            // identity can't tell (the pointer is reused), while the rerun flag
            // is not always armed (syncVisibleRanges early-returns on a
            // mode/dimension flip without setting it). Drop the whole outcome and
            // let the rerun, or the superseding batch's settle, produce a fresh
            // coherent one. Single-flight makes the common case all-current, so
            // this drops only on a genuine mid-sync re-slice, not every tweak.
            //
            // Chosen tradeoff (vs. the earlier per-panel partial apply): during
            // sweep playback or animation export, if a sync outlasts the frame
            // delay every sync is superseded and nothing lands for the sweep's
            // duration (it self-heals when playback stops), where partial apply
            // kept the untouched panels coherent. Coherence-by-construction is
            // worth that: partial apply could leave the three panels on different
            // ranges/log flags. If sweep coherence ever matters, gate playbackTick
            // on the sync too (it currently waits only on pendingRequests).
            const bool allCurrent = outcome.sync.has_value() && stampsCurrent();
            if (current && outcome.sync && allCurrent) {
                const auto [globalMin, globalMax] = outcome.sync->range;
                bool activeApplied = false;
                for (std::size_t index = 0; index < views.size(); ++index) {
                    auto* state = views[index];
                    auto& update = outcome.sync->panels[index];
                    if (!update.applies) {
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
                            && !displayIsSpherical() && state->warp != DisplayWarp::MappedGrid) {
                            placement = virtualPlacementFor(
                                *state, state->plane->physicalRegion);
                        }
                        const bool warpedWindow
                            = state->warp == DisplayWarp::MappedGrid
                            && outcome.mappedWindows[index].has_value();
                        if (state->warp == DisplayWarp::MappedGrid && !warpedWindow) {
                            // The re-warp fell back: the tile is flat now and
                            // the state says so, or the next relayout would
                            // place it at the warp's window and the probe read
                            // through the warp's index.
                            state->warp = DisplayWarp::None;
                            state->displaySourceIndex.reset();
                            state->mappedNodeBounds = {};
                            state->mappedWindow = {};
                            state->mappedWindowPixels = {0, 0};
                            state->displayRegion = state->plane->physicalRegion;
                            updateLineToolAvailability(*state);
                        }
                        const auto placed = tilePlacement(*state);
                        if (placed && (warpedWindow || m_pair)) {
                            // The re-coloured warp of the same window lands
                            // where it was, a flat tile at its region; the
                            // canvas and the view stay, and over a pair only
                            // this layer's tile changes.
                            if (warpedWindow) {
                                state->displayRegion = *outcome.mappedWindows[index];
                                state->displaySourceIndex
                                    = outcome.mappedSourceIndices[index];
                            }
                            const auto region = warpedWindow
                                ? state->displayRegion : state->plane->physicalRegion;
                            state->pixmapRegion = region;
                            state->view->setTileImage(state->tile,
                                outcome.images[index],
                                toQRectF(placed->sceneRectForRegion(region)),
                                toQRectF(placed->canvas),
                                ImageTransformPolicy::Preserve);
                            applyPairTileVisibility(*state);
                        } else {
                            state->view->setImage(outcome.images[index],
                                ImageTransformPolicy::GeometryAware,
                                logicalImageSize(*state, *state->plane,
                                    outcome.images[index]),
                                placement);
                        }
                        // Replacing the raster drops its overlays; restore them.
                        updateGridBoxes(*state);
                        updateOverlay(*state);
                        updateParticleOverlay(*state);
                    }
                    if (&layer == &primary() && m_pair && m_companionFollowsPrimary
                        && m_layers[1].active) {
                        // Only when the companion's raster shows something
                        // else: its own arrival re-dispatches this sync, so an
                        // unconditional request here would never settle.
                        refreshFollowingCompanion(index, globalMin, globalMax,
                            outcome.sync->logarithmic);
                    }
                    // This layer's raster on the active panel: the active
                    // view itself, or its counterpart when the active view
                    // belongs to the other layer.
                    activeApplied = activeApplied
                        || (m_activeView != nullptr
                            && state->normal == m_activeView->normal);
                }
                const auto* shown = m_activeView == nullptr ? nullptr
                    : &layer.planeViews[static_cast<std::size_t>(m_activeView->normal)];
                if (activeApplied && shown != nullptr && shown->plane->width > 0) {
                    const auto fieldName = layer.fieldSelector->currentText();
                    const auto label = shown->displayLogarithmic
                        ? fieldName + tr(" (log)") : fieldName;
                    applyDisplayPrecision(globalMin, globalMax);
                    layer.colorBar->setLogarithmic(shown->displayLogarithmic);
                    layer.colorBar->setFieldRange(label, globalMin, globalMax);
                    layer.range->showDisplayRange(globalMin, globalMax);
                    // The panel loop above wrote this range into every applied
                    // panel's state, the active one included, so the Dataset
                    // window reads the same numbers the bar just took.
                    syncDatasetWindowColors();
                }
                // The deferred full-domain range store (see the slice-arrival
                // completion): the union is only known here. This block runs
                // only for an all-current outcome (every panel's stamp matched),
                // so the stored union is over the current planes -- a stale
                // outcome is dropped in the else branch and never stored.
                if (layer.pendingRangeStore) {
                    // Only store if the pending key still describes the current
                    // (dataset, field, level, composition): a full-domain
                    // arrival's key can outlive its own sync (e.g. its
                    // syncVisibleRanges early-returned because the mode was
                    // File), and this sync's union is for the *current* field,
                    // so storing it under the stale key would poison that
                    // field's cached range (range-cache-staleness-races).
                    const FieldId liveField{
                        layer.fieldSelector->currentData().toUInt()};
                    const auto [liveComposition, liveMaximumLevel] =
                        decodeLevelData(layer.levelSelector->currentData().toInt(),
                            layer.session->metadata().finestLevel);
                    const DisplayCoordinator::RangeKey liveKey{
                        rangeCacheDataset(&layer == &m_layers[1] ? 1 : 0,
                            layer.session->id()),
                        liveField, liveMaximumLevel, liveComposition};
                    if (*layer.pendingRangeStore == liveKey) {
                        m_displayCoordinator.storeFullDomainRange(
                            *layer.pendingRangeStore, outcome.sync->range);
                    }
                    layer.pendingRangeStore.reset();
                }
            } else if (current && outcome.sync) {
                // Dropped as stale (a panel was re-sliced mid-sync): the union
                // is over at least one now-superseded plane, so it is neither
                // applied nor stored -- the pending key stays for the rerun,
                // which recomputes the union over the current planes.
#ifdef AMREXPLORER_QT_TEST_ACCESS
                // Sole writer of this test-only tally; the staleness smoke test
                // asserts its exact delta. The DiagnosticsModel's stale count
                // is not touched.
                ++m_visibleSyncStaleSkips;
#endif
            }
            finish();
        });
    // Commit to "in flight" only now that the throwing allocations above
    // succeeded; the sync joins the interactive batch (adjustActivity(1) on
    // the DiagnosticsModel) so interactiveSlicesSettled waits for it. Guard the launch: if
    // QtConcurrent::run throws (bad_alloc), un-latch so a later arrival can
    // retry, drop the watcher, and swallow -- the failure must not escape this
    // slot or wedge the sync shut for the session.
    layer.visibleSyncInFlight = true;
    m_diagnosticsModel->adjustActivity(1);
    try {
        watcher->setFuture(QtConcurrent::run([cachedRange, snapshots,
            logarithmic = layer.range->logarithmic(),
            contourMode = isContourMode(m_displayMode),
            contourCount = m_contourCount, palette = m_paletteController->palette()] {
#ifdef AMREXPLORER_QT_TEST_ACCESS
            // Held only when the staleness test has armed the gate; the throw
            // only when it has asked for one (the failure-path checks).
            if (visible_sync_test::waitAtGate()) {
                throw std::runtime_error("injected visible-sync failure");
            }
#endif
            std::array<DisplayCoordinator::PanelSyncInput, 3> inputs;
            for (std::size_t index = 0; index < snapshots.size(); ++index) {
                const auto& snapshot = snapshots[index];
                inputs[index] = {snapshot.plane.get(),
                    snapshot.contourPlane.get(), snapshot.outputSize};
            }
            SyncOutcome outcome;
            outcome.sync = DisplayCoordinator::renderPanelsToSharedRange(
                cachedRange, inputs, logarithmic, contourMode, contourCount,
                palette);
            if (outcome.sync) {
                for (std::size_t index = 0; index < inputs.size(); ++index) {
                    auto& image = outcome.sync->panels[index].image;
                    const auto& snapshot = snapshots[index];
                    if (image.valid() && image.width > 0 && snapshot.gridNodes) {
                        // The panel shows the warp of its plane, so the
                        // re-coloured plane is warped the same way, for the
                        // same window at the same pixels.
                        auto warped = warpMappedGrid(image, *snapshot.gridNodes,
                            snapshot.axes, snapshot.displayWindow,
                            snapshot.displayPixels);
                        image = std::move(warped.image);
                        if (warped.sourceIndex) {
                            outcome.mappedWindows[index] = warped.displayRegion;
                            outcome.mappedSourceIndices[index]
                                = std::move(warped.sourceIndex);
                        }
                    }
                    if (image.valid() && image.width > 0) {
                        outcome.images[index] = displayImageFor(image);
                    }
                }
            }
            return outcome;
        }));
    } catch (const std::exception& error) {
        layer.visibleSyncInFlight = false;
        m_diagnosticsModel->adjustActivity(-1);
        watcher->deleteLater();
        reportVisibleSyncFailure(error);
    }
}

void MainWindow::reportVisibleSyncFailure(const std::exception& error)
{
    reportBackgroundError(
        tr("Cannot synchronize views: %1").arg(exceptionMessage(error)));
}

void MainWindow::choosePlotfileSequence()
{
    // Select the plotfile directories directly with click / Ctrl-click /
    // Shift-click. QFileDialog::Directory only permits selecting more than one
    // directory on the non-native dialog, so disable the native one and force
    // extended selection on every file-list view (both the icon/list view and
    // the detail/tree view). The selected directories are validated as AMReX
    // plotfiles (Header + Level_N) by openSequence.
    QFileDialog dialog(this,
        tr("Open Plotfile Sequence — select two or more plotfile directories"),
        rememberedDialogDirectory());
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
    resetLengthUnit();
    resetAxisScale();
    closeCompanion();
    m_fabNavigator->reset();
    m_particleController->cancel();
    m_particleController->clearSamples();
    // A sequence is a different dataset, so it starts from defaults exactly as
    // a plain open does.
    m_particleController->resetSettings();
    m_remoteSequenceConnectionGeneration = 0;
    // A mapped view's canvas and window belong to the outgoing dataset.
    for (auto* state : allViewStates()) {
        state->mappedCanvasBounds.reset();
        state->mappedWindow = {};
        state->mappedWindowPixels = {0, 0};
        state->mappedNodeBounds = {};
    }
    // Frame 0 is not installed yet, so primary().session still describes the outgoing
    // one. Take the overlay dialogs' menu items down with the dialogs above,
    // the way a plain open's teardown does; configureSequenceControls and
    // the particle controller bring them back when a frame arrives.
    m_contoursAction->setEnabled(false);
    m_particleController->suspendAction();

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
    // lists its species, whose settings were just reset above, and the
    // contours one lists its fields. openDatasetImpl closes them for a plain
    // open; this path never runs that, so a sequence open would otherwise leave
    // them on screen writing the old dataset's names back on Apply. Frame steps
    // do not come through here, so both survive stepping, as intended.
    m_particleController->closeDialog();
    auto* contoursDialog = m_contoursDialog;
    m_contoursDialog = nullptr;
    if (contoursDialog != nullptr) {
        contoursDialog->close();
    }
}

void MainWindow::openRemoteSequence(
    const std::vector<std::string>& remotePaths)
{
    // Read together, before anything else runs: the generation belongs to
    // this connection and must be the one the loader captures.
    auto connection = m_remoteSession->connection();
    const auto connectionGeneration = m_remoteSession->connectionGeneration();
    if (!connection) {
        emit sequenceFrameFailed();
        reportBackgroundError(tr("Open a remote session first "
                                 "(File > Open Remote Plotfile...)."));
        return;
    }
    if (remotePaths.size() < 2
        || std::any_of(remotePaths.begin(), remotePaths.end(),
            [](const auto& path) { return path.empty(); })) {
        emit sequenceFrameFailed();
        QMessageBox::warning(this, tr("Cannot open remote sequence"),
            tr("Enter two or more plotfile paths as they appear on the "
               "remote machine. Remote frames are named by their path on "
               "the server, not chosen from a local file dialog."));
        return;
    }

    prepareSequence(remotePaths.size());
    m_remoteSequence = true;
    m_remoteSequenceDerivedFields
        = connection && connection->supportsDerivedFields();

    std::vector<std::filesystem::path> frames;
    frames.reserve(remotePaths.size());
    for (const auto& path : remotePaths) {
        frames.emplace_back(path);
    }
    // Foreground loads and prefetches share the session's connection, which
    // multiplexes their requests. There is no reconnect: the connection lives
    // as long as the ssh session, and a lost session is reported to the user
    // rather than silently re-established.
    auto loader = [connection = std::move(connection),
                      generation = connectionGeneration](
                      const std::filesystem::path& path, DatasetId,
                      const FrameSliceSpec& spec, StopToken cancellation) {
        return loadRemoteFrame(
            connection, path.string(), generation, spec, cancellation);
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
    const auto threeD = primary().session != nullptr
        && primary().session->metadata().dimension == 3;
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
        // DatasetId is allocated by the server and restarts at one on every
        // connection. Drop every dataset-scoped display range before
        // publishing a frame from a newly installed connection so an old ID
        // cannot alias.
        m_displayCoordinator.invalidateRangeCache();
        for (auto& layer : m_layers) {
            layer.pendingRangeStore.reset();
        }
        m_remoteSequenceConnectionGeneration = result.connectionGeneration;
    }
    const auto previousVectorFields = vectorFieldNames();
    primary().session = result.dataset;
    ++primary().sessionEpoch;
    restoreVectorFields(previousVectorFields);
    m_particleController->setSamples(std::move(result.particles));
    m_particleController->configureForDataset(true);
    m_volumeController->configureForDataset();
    const auto& metadata = primary().session->metadata();
    m_viewDimension = metadata.dimension;

    // Refresh the metadata dock and the window title (frame name + time).
    PlotfileMetadataResult frameMetadata;
    frameMetadata.metadata = std::make_shared<DatasetMetadata>(metadata);
    frameMetadata.metrics = result.dataset->metadataReadMetrics();
    frameMetadata.fileVersion = !result.fileVersion.empty()
        ? result.fileVersion : primary().fileVersion;
    showMetadata(frameMetadata, m_datasetPath);

    configureSequenceControls(defaultPositions,
        result.displays.empty()
            ? std::nullopt
            : std::optional<std::uint32_t>{
                  result.displays.front().request.field.value});
    if (selectCacheFallbackLevel(primary().levelSelector, result.cacheFallbackToLevel)) {
        configureSlicePositionControls();
        updateRangeModeAvailability();
        syncMenuChecks();
    }
    const auto views = primaryViews();
    if (result.displays.size() != views.size()) {
        throw std::runtime_error("frame slice count does not match the view set");
    }
    for (std::size_t index = 0; index < views.size(); ++index) {
        showSlice(*views[index], std::move(result.displays[index]),
            primary().sessionEpoch);
    }
    const auto cache = primary().session->cacheMetrics();
    m_diagnosticsModel->setCacheMetrics(cache);
    // This window has no dataset while it opens a sequence, so a definition
    // committed in another window meanwhile reached every window but this one,
    // and this frame carries the older list. Mid-playback the reload stands
    // aside and setPlaybackMode picks it up.
    reloadIfDefinitionsMoved();
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

void MainWindow::configureSequenceControls(
    bool defaultPositions, std::optional<std::uint32_t> displayedField)
{
    if (!primary().session) {
        return;
    }
    const auto& metadata = primary().session->metadata();
    // Preserve the user's selections across frames: the field index if it
    // still exists, the level by its combo data (falling back to finest
    // available when this frame has fewer levels).
    const auto previousField = m_controlsReady && primary().fieldSelector->count() > 0
        ? primary().fieldSelector->currentIndex() : 0;
    const auto previousLevel = m_controlsReady
        && primary().levelSelector->currentIndex() >= 0
            ? primary().levelSelector->currentData().toInt() : -1;
    const auto derivedRows = derivedFieldRows();
    {
        const QSignalBlocker fieldBlocker(primary().fieldSelector);
        const QSignalBlocker levelBlocker(primary().levelSelector);
        populateFieldSelector(derivedRows);
        // The field this frame was rendered with, not the position the last
        // frame's combo happened to be at: the two agree only while every
        // frame lists the same fields in the same order, and a definition one
        // frame cannot resolve compacts the ids after it. Selecting by
        // position there would label the plot with a different field's name
        // and point the colour range at it.
        const auto displayedIndex = displayedField
            ? primary().fieldSelector->findData(*displayedField)
            : -1;
        // Not clamped: std::clamp is undefined when the list is empty
        // (count() - 1 < 0), and selectFieldItem takes any index and comes to
        // rest on a field or on nothing.
        selectFieldItem(
            displayedIndex >= 0 ? displayedIndex : previousField);
        // The range memory is keyed by field id, and the ids moved with the
        // field list, so the widgets have to be told which field they now
        // represent or the next switch commits this frame's range onto
        // whatever field used to hold that id.
        primary().range->setTrackedField(primary().fieldSelector->currentText());
        primary().levelSelector->clear();
        populateLevelCombo(primary().levelSelector, metadata.finestLevel);
        const auto levelIndex = primary().levelSelector->findData(previousLevel);
        primary().levelSelector->setCurrentIndex(levelIndex >= 0 ? levelIndex : 0);
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
        updateIsoGeometry();
        publishSlicePositions();
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
        setActiveView(isThreeDimensional ? primary().planeViews[2] : m_view2d);
        focusActiveViewForPanning();
    }

    enableDatasetControls(metadata);
    m_exportAnimationAction->setEnabled(true);
    rebuildVariableMenu(derivedRows);
    ensureVectorFieldDefaults();
    updateRangeModeAvailability();
}

void MainWindow::resetRangeState()
{
    primary().range->reset();
    m_displayCoordinator.invalidateRangeCache();
    for (auto& layer : m_layers) {
        layer.pendingRangeStore.reset();
    }
}

void MainWindow::updateRangeModeAvailability()
{
    for (auto& layer : m_layers) {
        if (layer.active) {
            updateRangeModeAvailability(layer);
        }
    }
}

void MainWindow::updateRangeModeAvailability(DatasetLayer& layer)
{
    if (!layer.session || layer.fieldSelector == nullptr
        || layer.fieldSelector->currentIndex() < 0
        || layer.levelSelector->currentIndex() < 0) {
        return;
    }
    const auto& metadata = layer.session->metadata();
    const FieldId field{layer.fieldSelector->currentData().toUInt()};
    const auto [composition, maximumLevel] = decodeLevelData(
        layer.levelSelector->currentData().toInt(), metadata.finestLevel);
    layer.range->updateAvailability(
        RangeController::Availability{
            .file = layer.session->rangeAvailable(RangeRequest{
                field, maximumLevel, composition, RangeScope::File}),
            .level = layer.session->rangeAvailable(RangeRequest{
                field, maximumLevel, composition, RangeScope::Level}),
        },
        layer.fieldSelector->currentText());
}

FrameSliceSpec MainWindow::buildFrameSpec()
{
    FrameSliceSpec spec;
    // A viewer-wide setting, so it travels with every spec except where the
    // load it is built for cannot install it (derivedFieldsReachNextLoad).
    spec.derivedFields = derivedFieldsReachNextLoad()
        ? m_derivedFields->definitions()
        : std::vector<DerivedFieldDefinition>{};
    spec.displayMode = m_displayMode;
    spec.palette = m_paletteController->palette();
    spec.contourCount = m_contourCount;
    spec.sphericalDisplay = m_sphericalDisplay;
    spec.mappedGrid = m_mappedGrid;
    {
        const auto selection = primary().range->selection();
        spec.logarithmic = selection.logarithmic;
        spec.rangeMode = selection.mode;
        spec.userRange = selection.userRange;
    }
    spec.field = m_controlsReady && primary().fieldSelector->currentIndex() >= 0
        ? primary().fieldSelector->currentData().toUInt() : 0U;
    // The names alongside the indices: an index means something only in the
    // field list it came from, and the next frame's list can differ -- by its
    // stored fields, or by a derived definition that frame could not resolve
    // and left out, which compacts every id after it. Without a name the
    // reload lands on whatever now occupies that slot (resolveSpecField).
    const auto nameOf = [this](int field) {
        if (!primary().session) {
            return std::string{};
        }
        const auto& fields = primary().session->metadata().fields;
        return field >= 0 && static_cast<std::size_t>(field) < fields.size()
            ? fields[static_cast<std::size_t>(field)].name
            : std::string{};
    };
    spec.fieldName = nameOf(static_cast<int>(spec.field));
    spec.vectorUFieldName = nameOf(m_vectorUField);
    spec.vectorVFieldName = nameOf(m_vectorVField);
    spec.vectorWFieldName = nameOf(m_vectorWField);
    spec.levelSelection = m_controlsReady && primary().levelSelector->currentIndex() >= 0
        ? primary().levelSelector->currentData().toInt() : -1;
    spec.vectorUField = static_cast<std::uint32_t>(std::max(m_vectorUField, 0));
    spec.vectorVField = static_cast<std::uint32_t>(std::max(m_vectorVField, 0));
    spec.vectorWField = static_cast<std::uint32_t>(std::max(m_vectorWField, 0));
    // Slice positions only carry over between 3-D frames; anything else
    // starts the new dataset at its domain midpoints.
    spec.defaultPositions = m_viewDimension != 3;
    spec.slicePositions = m_slicePosition3d;
    const auto& particles = m_particleController->settings();
    spec.particleSelectionInitialized = particles.selectionInitialized;
    if (particles.selectionInitialized) {
        spec.particleSpecies = particles.species;
    }
    spec.particleFraction = particles.fraction;
    spec.particleSeed = particles.seed;
    spec.includeGridBoxes = m_boxesAction->isChecked();
    const auto views = primaryViews();
    spec.visibleRegions.reserve(views.size());
    if (m_remoteSequence) {
        spec.outputSizesAreViewportBounds = true;
        spec.outputSizes.reserve(views.size());
    }
    for (const auto* state : views) {
        spec.visibleRegions.push_back(state->visibleRegion);
        if (m_remoteSequence) {
            spec.outputSizes.push_back(stretchedViewportPixelSize(*state));
        }
    }
    fillMappedDisplays(spec, views);
    return spec;
}

void MainWindow::fillMappedDisplays(FrameSliceSpec& spec,
    const std::vector<PlaneViewState*>& views) const
{
    spec.displayWindows.clear();
    spec.displayPixels.clear();
    for (const auto* state : views) {
        // The window a warped view shows at its pixels; without one, the
        // whole node box at the viewport's.
        const bool windowed = isWarped(state->warp) && hasMappedWindow(*state);
        spec.displayWindows.push_back(windowed ? state->mappedWindow : RealBox{});
        spec.displayPixels.push_back(
            windowed ? state->mappedWindowPixels : viewportPixelSize(*state));
    }
}

void MainWindow::stepSweep(int direction)
{
    if (!primary().session || primary().session->metadata().dimension != 3) {
        return;
    }
    const auto axis = m_animationPanel->sweepAxis();
    const auto index = static_cast<std::size_t>(axis);
    const auto& metadata = primary().session->metadata();
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
    if (!primary().session || primary().session->metadata().dimension != 3) {
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
    const bool wasSequence = m_playbackMode == PlaybackMode::Sequence;
    m_playbackMode = mode;
    m_animationPanel->setSweepPlaying(mode == PlaybackMode::Sweep);
    m_animationPanel->setSequencePlaying(mode == PlaybackMode::Sequence);
    if (mode == PlaybackMode::None) {
        m_playbackTimer->stop();
    } else {
        m_playbackTimer->start(m_animationPanel->frameDelayMs());
    }
    // Playback is why the reload stood aside: the next frame reads the list
    // for itself, and stopping means there is no next frame. What is on screen
    // may then have been rendered against an older list, and only the session
    // itself can say -- a prefetch of the next frame is built from the current
    // one, so "when was a spec last built" answers a different question.
    if (wasSequence && mode != PlaybackMode::Sequence) {
        reloadIfDefinitionsMoved();
    }
    // Every frame of a sequence renders the volume as a draft, so what is
    // standing in the window when playback stops is a half-size one. Ask for
    // it again now that frames have stopped arriving; with no volume window
    // open this does nothing.
    if (wasSequence && mode != PlaybackMode::Sequence) {
        m_volumeController->refresh();
    }
}

void MainWindow::playbackTick()
{
    if (m_playbackMode == PlaybackMode::Sweep) {
        if (!primary().session || primary().session->metadata().dimension != 3) {
            setPlaybackMode(PlaybackMode::None);
            return;
        }
        // Skip the tick while the previous slice is still on a worker, so a
        // fast Speed setting cannot pile up requests.
        const auto axis = m_animationPanel->sweepAxis();
        if (primary().planeViews[static_cast<std::size_t>(axis)].pendingRequests > 0) {
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
    statusBar()->showMessage(message.section(QLatin1Char('\n'), 0, 0));
    m_diagnosticsModel->reportBackgroundError(message);
}

void MainWindow::updateDiagnostics()
{
    // Follows the session because it is wired to sessionChanged(): every
    // install decides afresh whether the values arrive at full precision.
    if (m_remotePrecisionLabel && m_remoteSession) {
        const auto notice = m_remoteSession->valuePrecisionNotice();
        m_remotePrecisionLabel->setText(
            notice.isEmpty() ? QString() : tr("Remote values: float"));
        m_remotePrecisionLabel->setToolTip(notice);
        m_remotePrecisionLabel->setVisible(!notice.isEmpty());
    }
    m_diagnosticsModel->refresh();
}

} // namespace amrvis::qt
