#include "MainWindowInternal.hpp"

namespace amrvis::qt {

namespace {

// Everything the FAB selector dock needs for a source, computed off the GUI
// thread (see buildFabSelector) so its header scans / per-block preads never
// block the event loop. `matched` distinguishes a recognized FAB or
// single-level-VisMF source (whose m_fabMode/source state should be applied)
// from anything else (leave that state untouched, just hide the dock).
struct FabSelectorBuild {
    bool matched = false;
    bool fabMode = false;
    bool hasSourceMetadata = false;
    std::vector<FabSelectorEntry> entries;
    std::filesystem::path root;
};

// The result of a dataset open worker: the metadata plus, when the caller did
// not ask to preserve the existing selector, the FAB selector contents built
// alongside it (so the GUI-thread completion only blits, never reads files).
struct OpenedDataset {
    PlotfileMetadataResult metadata;
    std::optional<FabSelectorBuild> fabSelector;
    std::shared_ptr<DatasetSession> session;
};

// Reads FAB/MultiFab record headers and builds the selector entries. Runs on a
// worker thread; QCoreApplication::translate is thread-safe, and it touches no
// widgets or member state.
FabSelectorBuild buildFabSelector(
    const PlotfileMetadataResult& result, const std::filesystem::path& path)
{
    const auto precisionLabel = [](FabRealPrecision precision) {
        return precision == FabRealPrecision::Single
            ? QCoreApplication::translate("MainWindow", "IEEE-32")
            : QCoreApplication::translate("MainWindow", "IEEE-64");
    };

    FabSelectorBuild build;
    build.root = path.parent_path();
    if (build.root.empty()) {
        build.root = ".";
    }

    if (result.fileVersion == "FAB") {
        const auto records = scanFabFile(path);
        build.entries.reserve(records.size());
        for (const auto& record : records) {
            build.entries.push_back({
                .ordinal = record.ordinal,
                .level = 0,
                .blockIndex = record.ordinal,
                .filePath = path,
                .fileOffset = record.headerOffset,
                .validBox = record.storedBox,
                .storedBox = record.storedBox,
                .dimension = record.dimension,
                .components = record.components,
                .precision = precisionLabel(record.precision),
                .rawRecord = true
            });
        }
        build.matched = true;
        build.fabMode = true;
        build.hasSourceMetadata = false;
    } else if (result.fileVersion.starts_with("VisMF-")
        && result.metadata->levels.size() == 1) {
        const auto& metadata = *result.metadata;
        const auto& level = metadata.levels.front();
        build.entries.reserve(level.blocks.size());
        for (std::size_t index = 0; index < level.blocks.size(); ++index) {
            const auto& block = level.blocks[index];
            // Overflow-guarded shared grow (this copy previously used
            // plain int).
            auto storedBox = amrvis::detail::grownBox<MetadataReadError>(
                block.box, level.ghostWidth, metadata.dimension);
            auto precision = FabRealPrecision::Double;
            if (level.visMfHeaderVersion == 1) {
                const auto record = inspectFabRecord(
                    build.root / block.filePath, block.fileOffset);
                storedBox = record.storedBox;
                precision = record.precision;
            } else {
                precision = fabPrecisionFromDescriptor(level.realDescriptor);
            }
            build.entries.push_back({
                .ordinal = index,
                .level = level.level,
                .blockIndex = index,
                .filePath = build.root / block.filePath,
                .fileOffset = block.fileOffset,
                .validBox = block.box,
                .storedBox = storedBox,
                .dimension = metadata.dimension,
                .components = level.storedComponents,
                .precision = precisionLabel(precision),
                .rawRecord = false
            });
        }
        build.matched = true;
        build.fabMode = false;
        build.hasSourceMetadata = true;
    }
    return build;
}

} // namespace

void MainWindow::cancelInFlight()
{
    // Stop the timers that resubmit work and request stop on every async task
    // this window can launch. The slice/prefetch/line-query/initial-load tasks
    // run on QThreadPool::globalInstance() via QtConcurrent::run; that pool's
    // destructor calls waitForDone() with no timeout, and a task caught mid-read
    // holds the global AMReX I/O mutex and only re-checks its cancellation token
    // at the next chunk boundary (PlotfileBlockReader checks every 1 MiB / 4096
    // values). request_stop is the cooperative signal those tasks poll, so once
    // set a running task bails promptly and teardown unblocks -- which is what
    // keeps closing a window (or quitting) from looking like a hang.
    //
    // This must NOT clear() the global pool: it is shared by every MainWindow
    // (File -> Open New Window), and clear() discards *other* windows' queued-
    // but-unstarted runnables, whose QFutureWatchers then never fire, wedging
    // those windows on "Loading..." forever (see
    // window-close-clears-shared-thread-pool). Cancellation is per-task via the
    // stop tokens above; a queued task starts, observes its token, and exits
    // cheaply. clear() belongs only on the aboutToQuit path (every window is
    // going away), where it is wired in the constructor.
    if (m_sliceDebounce != nullptr) {
        m_sliceDebounce->stop();
    }
    if (m_playbackTimer != nullptr) {
        m_playbackTimer->stop();
    }
    m_initialStopSource.request_stop();
    m_metadataStopSource.request_stop();
    m_sequenceController->cancelActiveWork();
    m_linePlotStopSource.request_stop();
    m_particleStopSource.request_stop();
    m_view2d.stopSource.request_stop();
    for (auto& state : m_planeViews) {
        state.stopSource.request_stop();
    }
}

void MainWindow::restoreSettings()
{
    const auto settings = makeSettings();

    m_paletteController->restore(settings);
    m_colorBar->setPalette(&m_paletteController->palette());

    {
        const QSignalBlocker logarithmicBlocker(m_logarithmic);
        m_logarithmic->setChecked(
            settings.value(QStringLiteral("range/logarithmic"), false).toBool());
    }
    {
        // A stored format that no longer validates falls back to the default.
        const auto format = settings.value(QStringLiteral("numberFormat"),
            defaultNumberFormat()).toString();
        m_numberFormat = isValidNumberFormat(format) ? format
            : defaultNumberFormat();
        m_rangeMinimum->setNumberFormat(m_numberFormat);
        m_rangeMaximum->setNumberFormat(m_numberFormat);
        m_colorBar->setNumberFormat(m_numberFormat);
    }
    m_animationPanel->setSpeedValue(
        settings.value(QStringLiteral("animation/speed"), 300).toInt());
    {
        const QSignalBlocker syncZoomBlocker(m_syncRubberBandZoomAction);
        m_syncRubberBandZoomAction->setChecked(
            settings.value(QStringLiteral("zoom/syncRubberBand"), true).toBool());
    }
    if (m_boxesAction != nullptr) {
        // Blocked: the toggle handler re-slices, and nothing is loaded yet.
        const QSignalBlocker boxesBlocker(m_boxesAction);
        m_boxesAction->setChecked(
            settings.value(QStringLiteral("overlay/boxes"), false).toBool());
    }
    if (m_sphericalSupersampleGroup != nullptr) {
        const auto stored = settings.value(
            QStringLiteral("spherical/supersample"), m_sphericalSupersample).toInt();
        // Accept only a factor the menu offers; otherwise keep the default.
        // setChecked emits toggled, not triggered, so the re-warp slot is not
        // fired here.
        for (auto* action : m_sphericalSupersampleGroup->actions()) {
            if (action->data().toInt() == stored) {
                m_sphericalSupersample = stored;
                action->setChecked(true);
                break;
            }
        }
    }
    if (m_sphericalDisplayGroup != nullptr) {
        const auto stored = settings.value(QStringLiteral("spherical/display"),
            static_cast<int>(m_sphericalDisplay)).toInt();
        for (auto* action : m_sphericalDisplayGroup->actions()) {
            if (action->data().toInt() == stored) {
                m_sphericalDisplay = static_cast<SphericalDisplay>(stored);
                action->setChecked(true);
                break;
            }
        }
    }
    applySpeed();

    const auto geometry = settings.value(QStringLiteral("geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
}

void MainWindow::saveSettings()
{
    auto settings = makeSettings();
    // Range mode is deliberately not persisted: the correct default (File)
    // depends on the dataset and restoring a different mode from a previous
    // session would produce unexpected color bars.
    settings.setValue(QStringLiteral("range/logarithmic"), m_logarithmic->isChecked());
    m_paletteController->save(settings);
    settings.setValue(QStringLiteral("numberFormat"), m_numberFormat);
    settings.setValue(QStringLiteral("animation/speed"),
        m_animationPanel->speedValue());
    settings.setValue(QStringLiteral("zoom/syncRubberBand"),
        m_syncRubberBandZoomAction->isChecked());
    // The grid-box overlay is a display preference like the palette, and the
    // toggle has always called saveSettings; it just had no key to write, so
    // it looked persisted and was not.
    settings.setValue(QStringLiteral("overlay/boxes"),
        m_boxesAction->isChecked());
    settings.setValue(QStringLiteral("spherical/supersample"),
        m_sphericalSupersample);
    settings.setValue(QStringLiteral("spherical/display"),
        static_cast<int>(m_sphericalDisplay));
}

void MainWindow::updateWindowTitle()
{
    if (!m_openMetadata) {
        setWindowTitle(tr("AMReXplorer"));
        return;
    }
    const auto& metadata = *m_openMetadata;
    auto name = QString::fromStdString(m_datasetPath.filename().string());
    if (name.isEmpty()) {
        name = QString::fromStdString(m_datasetPath.string());
    }
    // Standalone FABs and MultiFabs carry neither a simulation time nor an
    // AMR hierarchy, so their titles show just the format name.
    if (m_fabMode) {
        setWindowTitle(tr("AMReXplorer — %1 — FAB").arg(name));
    } else if (!metadata.hasPhysicalGeometry) {
        setWindowTitle(tr("AMReXplorer — %1 — MultiFab").arg(name));
    } else {
        setWindowTitle(
            tr("AMReXplorer — %1  T = %2  Levels: 0..%3  Finest Level: %3")
                .arg(name)
                .arg(metadata.time, 0, 'g', 12)
                .arg(metadata.finestLevel));
    }
}

MainWindow* MainWindow::createNewWindow()
{
    auto* window = new MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
    return window;
}

void MainWindow::chooseDataset()
{
    const auto settings = makeSettings();
    const auto directory = QFileDialog::getExistingDirectory(
        this, tr("Open AMReX plotfile"),
        settings.value(QStringLiteral("lastOpenDirectory")).toString());
    if (directory.isEmpty()) {
        return;
    }
    // Directory pickers descend into a plotfile on double-click instead of
    // selecting it, so the choice easily lands on an inner directory
    // (Level_1, a particle species, ...). Such a selection resolves up to
    // the enclosing plotfile rather than failing on the subdirectory.
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
    openDataset(path);
}

void MainWindow::chooseStandaloneDataset(const QString& caption, bool rawFab)
{
    const auto settings = makeSettings();
    const auto filename = QFileDialog::getOpenFileName(this,
        caption,
        settings.value(QStringLiteral("lastOpenDirectory")).toString(),
        tr("AMReX data (*)"));
    if (!filename.isEmpty()) {
        if (rawFab) {
            const auto path = std::filesystem::path(filename.toStdString());
            auto root = path.parent_path();
            if (root.empty()) {
                root = ".";
            }
            openStandaloneFabAsync(path, std::nullopt, std::move(root), false,
                std::nullopt, tr("Cannot open FAB"));
        } else {
            openDataset(filename.toStdString());
        }
    }
}

void MainWindow::applyFabSelectorRollback(const FabSelectorRollback& rollback)
{
    m_fabMode = rollback.fabMode;
    m_fabSelectorDock->setBackAvailable(rollback.backAvailable);
    if (rollback.ordinal) {
        m_fabSelectorDock->selectEntry(*rollback.ordinal);
    } else {
        m_fabSelectorDock->clearSelection();
    }
}

void MainWindow::openStandaloneFabAsync(std::filesystem::path path,
    std::optional<std::uint64_t> fileOffset, std::filesystem::path dataRoot,
    bool preserveFabSelector, std::optional<FrameSliceSpec> initialSpec,
    QString failureTitle, std::optional<FabSelectorRollback> rollback)
{
    ++m_activeRequests;
    const auto generation = m_generation;
    // Two of these can be in flight at once -- clicking a second raw record
    // while the first is still reading, which is reachable precisely because
    // the read no longer freezes the GUI. Without a per-request token both
    // completions match the generation they captured and the *first* to arrive
    // opens, while the selector already shows the second: oldest-wins, and the
    // window disagrees with the dock.
    //
    // Inheritance is decided here rather than at the call sites, because every
    // request supersedes whatever was live -- including the direct-open entry
    // point, which brings no rollback of its own. The read it supersedes will
    // retire without restoring anything, so if this one fails the state to
    // return to is still the one that was last displayed. Checked before the
    // token moves, since moving it is what retires the other request.
    const bool supersedesLive = m_pendingFabOpen
        && m_pendingFabOpen->generation == generation
        && m_pendingFabOpen->requestId == m_fabOpenGeneration;
    if (supersedesLive) {
        rollback = m_pendingFabOpen->rollback;
    }
    const auto requestId = ++m_fabOpenGeneration;
    if (rollback) {
        m_pendingFabOpen = PendingFabOpen{generation, requestId, *rollback};
    } else {
        // Nothing live and nothing to fall back to: this request owns the slot
        // and a failure of it has nowhere to return to.
        m_pendingFabOpen.reset();
    }
    auto* watcher = new QFutureWatcher<PlotfileMetadataResult>(this);
    connect(watcher, &QFutureWatcher<PlotfileMetadataResult>::finished, this,
        [this, watcher, generation, requestId, path,
            dataRoot = std::move(dataRoot), preserveFabSelector,
            initialSpec = std::move(initialSpec),
            failureTitle = std::move(failureTitle)]() mutable {
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            // A dataset opened while this read was in flight owns the window
            // now, and so does a newer FAB read or a selector teardown;
            // publishing over any of them would be a stale result.
            const bool current = generation == m_generation
                && requestId == m_fabOpenGeneration;
            // Only the completion that recorded the pending entry may consume
            // it. Anything that revoked it -- a dataset open, a teardown, a
            // newer click -- has already made `current` false.
            // Taken out of the member up front, not read back later: this
            // completion decides the entry's fate either way, and openDatasetImpl
            // below can throw *after* the success path has given the entry up.
            // Reading `m_pendingFabOpen->rollback` in the catch would then
            // dereference a disengaged optional.
            std::optional<FabSelectorRollback> owned;
            if (current && m_pendingFabOpen
                && m_pendingFabOpen->generation == generation
                && m_pendingFabOpen->requestId == requestId) {
                owned = m_pendingFabOpen->rollback;
                m_pendingFabOpen.reset();
            }
            try {
                auto metadata = watcher->future().takeResult();
                if (current) {
                    openDatasetImpl(path, false, std::move(metadata),
                        std::move(dataRoot), preserveFabSelector,
                        std::move(initialSpec));
                } else {
                    ++m_staleResults;
                }
            } catch (const std::exception& error) {
                if (current) {
                    // The caller may have moved the selector to this FAB
                    // before the read returned; the open did not happen, so
                    // put it back before saying so.
                    if (owned) {
                        applyFabSelectorRollback(*owned);
                    }
                    reportBackgroundError(
                        tr("%1: %2").arg(failureTitle, exceptionMessage(error)));
                } else {
                    ++m_staleResults;
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([path, fileOffset] {
        return fileOffset
            ? StandaloneMetadataReader{}.readFab(path, *fileOffset)
            : StandaloneMetadataReader{}.readFab(path);
    }));
    updateDiagnostics();
}

void MainWindow::viewFab(std::size_t entryIndex)
{
    const auto& entries = m_fabSelectorDock->entries();
    if (entryIndex >= entries.size()) {
        return;
    }
    const auto entry = entries[entryIndex];
    try {
        auto selectedSpec = m_dataset
            ? std::optional<FrameSliceSpec>{buildFrameSpec()}
            : std::nullopt;
        if (selectedSpec) {
            selectedSpec->levelSelection = -1;
            selectedSpec->rangeMode = RangeMode::File;
            selectedSpec->userRange.reset();
        }
        PlotfileMetadataResult selected;
        if (entry.rawRecord) {
            // The record's own header has to be read; do it off the GUI thread
            // and let the completion open it. The selector state is moved to
            // the pending record immediately so the dock reflects the choice
            // without waiting for the read -- but only a read that succeeds
            // actually changes what the window displays, so a failure has to
            // put the previous state back rather than leave the dock claiming
            // a FAB that is not on screen.
            //
            // Snapshot what is displayed now; openStandaloneFabAsync prefers a
            // still-live pending rollback over it, because from a displayed FAB
            // X, clicking A then B and having B fail must return to X, not to
            // the A the dock is merely showing -- A lost the request token and
            // will never open.
            const FabSelectorRollback rollback{m_fabMode,
                m_fabSelectorDock->backAvailable(),
                m_fabSelectorDock->selectedOrdinal()};
            m_fabMode = true;
            m_fabSelectorDock->setBackAvailable(m_multifabReturn.has_value());
            m_fabSelectorDock->selectEntry(entry.ordinal);
            openStandaloneFabAsync(m_fabSourcePath, entry.fileOffset,
                m_fabDataRoot, true, std::move(selectedSpec),
                tr("Cannot view FAB"), rollback);
            return;
        }
        {
            if (!m_fabSourceMetadata) {
                throw std::runtime_error(
                    "the source MultiFab is no longer available");
            }
            if (!m_multifabReturn) {
                m_multifabReturn = MultiFabReturnState{
                    m_fabSourcePath, m_fabDataRoot,
                    *m_fabSourceMetadata, buildFrameSpec()};
            }
            selected = makeSelectedFabMetadata(*m_fabSourceMetadata->metadata,
                entry.level, entry.blockIndex, m_fabDataRoot);
        }
        m_fabMode = true;
        m_fabSelectorDock->setBackAvailable(m_multifabReturn.has_value());
        m_fabSelectorDock->selectEntry(entry.ordinal);
        openDatasetImpl(m_fabSourcePath, false, std::move(selected),
            m_fabDataRoot, true, std::move(selectedSpec));
    } catch (const std::exception& error) {
        QMessageBox::critical(this, tr("Cannot view FAB"),
            exceptionMessage(error));
    }
}

void MainWindow::backToMultiFab()
{
    if (!m_multifabReturn) {
        return;
    }
    auto state = std::move(*m_multifabReturn);
    m_multifabReturn.reset();
    m_fabMode = false;
    m_fabSelectorDock->setBackAvailable(false);
    openDatasetImpl(state.path, false, std::move(state.metadata),
        std::move(state.dataRoot), true, std::move(state.spec));
}

void MainWindow::exportImage()
{
    auto* view = m_activeView != nullptr ? m_activeView->view : nullptr;
    if (view == nullptr || !view->hasImage()) {
        QMessageBox::information(this, tr("No image"),
            tr("Open a dataset before exporting an image."));
        return;
    }

    QString selectedFilter;
    auto filename = QFileDialog::getSaveFileName(
        this, tr("Export scalar image"), QString(),
        tr("PNG image (*.png);;FITS float64 image (*.fits *.fit)"),
        &selectedFilter);
    if (filename.isEmpty()) {
        return;
    }

    const bool hasFitsExtension = filename.endsWith(
            QStringLiteral(".fits"), Qt::CaseInsensitive)
        || filename.endsWith(QStringLiteral(".fit"), Qt::CaseInsensitive);
    const bool hasPngExtension = filename.endsWith(
        QStringLiteral(".png"), Qt::CaseInsensitive);
    // An explicitly typed recognized extension wins over the selected filter.
    const bool fits = hasFitsExtension
        || (!hasPngExtension
            && selectedFilter.contains(QStringLiteral("*.fits")));
    const QString extension = filename.endsWith(
            QStringLiteral(".fit"), Qt::CaseInsensitive)
        ? QStringLiteral(".fit")
        : fits ? QStringLiteral(".fits") : QStringLiteral(".png");

    // Normalize the chosen extension and get the base name used for the
    // per-panel suffixes of a 3-D export.
    QString base = filename;
    if (base.endsWith(QStringLiteral(".fits"), Qt::CaseInsensitive)) {
        base.chop(5);
    } else if (base.endsWith(QStringLiteral(".fit"), Qt::CaseInsensitive)
        || base.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        base.chop(4);
    }
    filename = base + extension;

    if (fits) {
        const auto writePlane = [this](const QString& outPath,
                                    const ScalarPlane& plane) {
            try {
                writeFloat64Fits(
                    std::filesystem::path(outPath.toStdString()), plane);
                return true;
            } catch (const std::exception& error) {
                QMessageBox::critical(this, tr("Cannot export image"),
                    tr("Could not write %1.\n\n%2")
                        .arg(outPath, QString::fromUtf8(error.what())));
                return false;
            }
        };
        if (m_viewDimension == 3) {
            constexpr std::array<const char*, 3> suffixes{"_yz", "_xz", "_xy"};
            for (std::size_t normal = 0; normal < m_planeViews.size(); ++normal) {
                const auto& state = m_planeViews[normal];
                if (state.plane->width <= 0 || state.plane->height <= 0) {
                    continue;
                }
                const auto outPath = base
                    + QString::fromLatin1(suffixes[normal]) + extension;
                writePlane(outPath, *state.plane);
            }
        } else if (m_activeView != nullptr) {
            writePlane(filename, *m_activeView->plane);
        }
        return;
    }

    QMessageBox choice(this);
    choice.setIcon(QMessageBox::Question);
    choice.setWindowTitle(tr("Export Image"));
    choice.setText(tr("Include the color bar in the exported image?"));
    auto* withBar = choice.addButton(tr("&With color bar"),
        QMessageBox::AcceptRole);
    auto* withoutBar = choice.addButton(tr("With&out color bar"),
        QMessageBox::AcceptRole);
    choice.addButton(QMessageBox::Cancel);
    choice.exec();
    if (choice.clickedButton() != withBar && choice.clickedButton() != withoutBar) {
        return;
    }
    const bool includeColorBar = choice.clickedButton() == withBar;

    if (m_viewDimension == 3) {
        // Export all three panels: foo_xy.png, foo_xz.png, foo_yz.png.
        constexpr std::array<const char*, 3> suffixes{"_yz", "_xz", "_xy"};
        for (int normal = 0; normal < 3; ++normal) {
            const auto idx = static_cast<std::size_t>(normal);
            auto* panelView = m_planeViews[idx].view;
            if (panelView == nullptr || !panelView->hasImage()) {
                continue;
            }
            const auto outPath = base
                + QString::fromLatin1(suffixes[idx]) + QStringLiteral(".png");
            const qreal scale = std::max(1.0,
                panelView->transform().m11());
            const QImage composite = composeExportFrame(
                panelView, includeColorBar, scale);
            if (composite.isNull() || !composite.save(outPath, "PNG")) {
                QMessageBox::critical(this, tr("Cannot export image"),
                    tr("Could not write %1.").arg(outPath));
            }
        }
    } else {
        const qreal exportScale = std::max(1.0, view->transform().m11());
        const QImage composite = composeExportFrame(
            view, includeColorBar, exportScale);
        if (composite.isNull()) {
            QMessageBox::critical(this, tr("Cannot export image"),
                tr("The image could not be composited."));
            return;
        }
        if (!composite.save(filename, "PNG")) {
            QMessageBox::critical(this, tr("Cannot export image"),
                tr("The image could not be written to %1.").arg(filename));
        }
    }
}

QImage MainWindow::composeExportFrame(const ImageView* view,
    bool includeColorBar, qreal scaleFactor) const
{
    if (view == nullptr) {
        return {};
    }
    const QImage scalar = view->composedImage(scaleFactor);
    if (scalar.isNull() || !includeColorBar) {
        return scalar;
    }
    constexpr int gap = 8;
    const int barWidth = m_colorBar->preferredWidth();
    QImage composite(QSize(scalar.width() + gap + barWidth, scalar.height()),
        QImage::Format_ARGB32_Premultiplied);
    {
        QPainter painter(&composite);
        painter.setFont(m_colorBar->font());
        painter.fillRect(composite.rect(), viewportBackground());
        painter.drawImage(0, 0, scalar);
        m_colorBar->paintBar(&painter,
            QRect(scalar.width() + gap, 0, barWidth, composite.height()));
    }
    return composite;
}

void MainWindow::exportAnimation()
{
    if (m_animationExporter->active()) {
        return;
    }
    if (!m_sequenceController->hasSequence()) {
        QMessageBox::information(this, tr("No animation"),
            tr("Open a plotfile sequence before exporting an animation."));
        return;
    }
    auto* view = m_activeView != nullptr ? m_activeView->view : nullptr;
    if (view == nullptr || !view->hasImage()) {
        QMessageBox::information(this, tr("No image"),
            tr("Open a dataset before exporting an animation."));
        return;
    }

    // Color-bar choice (same options as single-image export); applies to all.
    QMessageBox choice(this);
    choice.setIcon(QMessageBox::Question);
    choice.setWindowTitle(tr("Export Animation"));
    choice.setText(tr("Include the color bar in every frame?"));
    auto* withBar = choice.addButton(tr("&With color bar"), QMessageBox::AcceptRole);
    auto* withoutBar = choice.addButton(tr("With&out color bar"), QMessageBox::AcceptRole);
    choice.addButton(QMessageBox::Cancel);
    choice.exec();
    if (choice.clickedButton() != withBar && choice.clickedButton() != withoutBar) {
        return;
    }
    const bool includeColorBar = choice.clickedButton() == withBar;

    // The chosen file's directory and basename (minus extension) become the
    // output location and the PNG/MP4 stem, e.g. "runs/anim.png" ->
    // runs/anim_0000.png ... runs/anim.mp4.
    const auto settings = makeSettings();
    const auto path = QFileDialog::getSaveFileName(this,
        tr("Export animation"),
        settings.value(QStringLiteral("lastOpenDirectory")).toString(),
        tr("PNG image (*.png)"));
    if (path.isEmpty()) {
        return;
    }
    beginAnimationExport(path, includeColorBar);
}

void MainWindow::beginAnimationExport(const QString& path, bool includeColorBar)
{
    auto* view = m_activeView != nullptr ? m_activeView->view : nullptr;
    if (view == nullptr || !view->hasImage()
        || !m_sequenceController->hasSequence()) {
        return;
    }
    // Freeze the export zoom from the current view so every frame renders at the
    // same dimensions even if a frame's image size changes and refits the view.
    // In 3-D this single scale is shared by all three panels, so a panel whose
    // fitted zoom differs from the active view exports at the active view's
    // scale -- constant across frames, which is the goal.
    const auto scale = std::max(1.0, view->transform().m11());
    std::vector<QString> suffixes;
    if (m_viewDimension == 3) {
        suffixes = {QStringLiteral("_yz"), QStringLiteral("_xz"),
            QStringLiteral("_xy")};
    } else {
        suffixes = {QString()};
    }
    if (!m_animationExporter->begin(path, includeColorBar,
            m_sequenceController->frameCount(),
            m_sequenceController->currentIndex(), scale,
            std::move(suffixes), this)) {
        return;
    }

    // Freeze the action and stop playback while exporting.
    m_exportAnimationAction->setEnabled(false);
    setPlaybackMode(PlaybackMode::None);

    // forceRestart because the export drives itself off sequenceFrameDisplayed,
    // and an export started while frame 0 is already on screen would otherwise
    // be suppressed as a no-op and never receive the signal that advances it.
    goToSequenceFrame(0, true);
}

std::optional<DatasetRequest> MainWindow::buildDatasetRequest() const
{
    if (!m_dataset || m_activeView == nullptr
        || m_activeView->plane->width <= 0 || m_activeView->plane->height <= 0
        || m_fieldSelector->currentIndex() < 0) {
        return std::nullopt;
    }
    const auto& metadata = m_dataset->metadata();
    DatasetRequest request;
    request.dataset = m_dataset;
    request.field.value = m_fieldSelector->currentData().toUInt();
    request.fieldName = tr("%1 — %2").arg(m_activeView->label)
        .arg(QString::fromStdString(
            metadata.fields[request.field.value].name));
    // The "selected region" is the active view's visible region: the
    // rubber-band zoom, or the whole domain when fitted.
    request.region = m_activeView->plane->physicalRegion;
    request.normalAxis = m_activeView->normal;
    if (metadata.dimension == 3) {
        request.slicePosition
            = m_slicePosition3d[static_cast<std::size_t>(m_activeView->normal)];
    }
    return request;
}

void MainWindow::showDatasetWindow()
{
    auto request = buildDatasetRequest();
    if (!request.has_value()) {
        return;
    }
    // One instance at a time: a new window replaces the old one.
    closeDatasetWindow();
    auto* window = new DatasetWindow(*request);
    window->setNumberFormat(m_numberFormat);
    m_datasetWindow = window;
    connect(window, &QObject::destroyed, this, [this, window] {
        if (m_datasetWindow == window) {
            m_datasetWindow = nullptr;
        }
        for (auto* state : currentViews()) {
            state->view->setCellHighlight(std::nullopt);
        }
    });
    connect(window, &DatasetWindow::extractionFailed, this,
        &MainWindow::reportBackgroundError);
    connect(window, &DatasetWindow::cellActivated, this,
        [this](const RealBox& physicalCell) {
            datasetCellActivated(physicalCell);
        });
    connect(window, &DatasetWindow::refreshRequested, this,
        [this] { refreshDatasetWindow(); });
    window->show();
    window->raise();
    window->activateWindow();
}

void MainWindow::closeDatasetWindow()
{
    auto* window = m_datasetWindow;
    m_datasetWindow = nullptr;
    if (window != nullptr) {
        window->close();
    }
}

void MainWindow::refreshDatasetWindow()
{
    if (m_datasetWindow == nullptr) {
        return;
    }
    auto request = buildDatasetRequest();
    if (!request.has_value()) {
        closeDatasetWindow();
        return;
    }
    m_datasetWindow->reload(*request);
}

void MainWindow::datasetCellActivated(const RealBox& physicalCell)
{
    if (m_activeView == nullptr) {
        return;
    }
    const auto& plane = *m_activeView->plane;
    if (plane.width <= 0 || plane.height <= 0) {
        return;
    }
    const auto axes = displayAxes(m_activeView->normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    if (displayIsSpherical()) {
        // xAxis is r, yAxis is theta.
        const double r0 = physicalCell.lower[xAxis];
        const double r1 = physicalCell.upper[xAxis];
        const double t0 = physicalCell.lower[yAxis];
        const double t1 = physicalCell.upper[yAxis];
        const auto mapping = planeMapping(*m_activeView);
        const bool valid = r1 > r0 && t1 > t0;
        // Branch on the view state's mode, matching the mapping (see
        // updateGridBoxes).
        if (m_activeView->sphericalDisplay == SphericalDisplay::RZ) {
            std::optional<QPainterPath> highlight;
            if (valid) {
                highlight = sphericalSectorPath(mapping, r0, r1, t0, t1);
            }
            m_activeView->view->setCellHighlightPath(highlight);
        } else {
            std::optional<QRectF> highlight;
            if (valid) {
                QRectF rect(mapping.sceneFromLogical(r0, t0),
                    mapping.sceneFromLogical(r1, t1));
                rect = rect.normalized();
                if (!rect.isEmpty()) {
                    highlight = rect;
                }
            }
            m_activeView->view->setCellHighlight(highlight);
        }
        return;
    }
    const auto& region = plane.physicalRegion;
    const auto xExtent = region.upper[xAxis] - region.lower[xAxis];
    const auto yExtent = region.upper[yAxis] - region.lower[yAxis];
    // Same physical-to-scene mapping updateGridBoxes applies; plane row 0 is
    // the image bottom, so scene y runs opposite to physical y.
    const auto pixelX0 = (physicalCell.lower[xAxis] - region.lower[xAxis])
        / xExtent * plane.width;
    const auto pixelX1 = (physicalCell.upper[xAxis] - region.lower[xAxis])
        / xExtent * plane.width;
    const auto pixelY0 = plane.height
        - (physicalCell.upper[yAxis] - region.lower[yAxis])
            / yExtent * plane.height;
    const auto pixelY1 = plane.height
        - (physicalCell.lower[yAxis] - region.lower[yAxis])
            / yExtent * plane.height;
    QRectF rectangle(QPointF(pixelX0, pixelY0), QPointF(pixelX1, pixelY1));
    rectangle = rectangle.normalized().intersected(
        QRectF(0.0, 0.0, plane.width, plane.height));
    std::optional<QRectF> highlight;
    if (!rectangle.isEmpty()) {
        highlight = rectangle;
    }
    m_activeView->view->setCellHighlight(highlight);
}

void MainWindow::openDataset(
    const std::filesystem::path& path, bool metadataOnly)
{
    openDatasetImpl(
        path, metadataOnly, std::nullopt, {}, false, std::nullopt);
}

void MainWindow::resetFabState()
{
    // Belt-and-braces, not the guarantee. Tearing the selector down has to
    // revoke any read still in flight against it, but the dataset generation
    // already does that at both call sites: openDatasetImpl bumps it further
    // down the same straight-line body, and prepareSequence's callers reach
    // MainWindow's frameSwitchStarted handler -- a direct connection, so the
    // same event-loop slot -- which bumps it too. No completion can be
    // delivered in between, so `generation == m_generation` is already false
    // for every earlier read and this token bump has never been what retires
    // one. It is kept because it is cheap and because relying on a bump that
    // happens a frame up the stack, in a signal handler, is not a property
    // this function can see. Do not read it as load-bearing: it is not
    // covered by a test, and it cannot be, because the state it would guard
    // is unreachable.
    ++m_fabOpenGeneration;
    m_pendingFabOpen.reset();
    m_fabMode = false;
    m_multifabReturn.reset();
    m_fabSourceMetadata.reset();
    m_fabSourcePath.clear();
    m_fabDataRoot.clear();
    m_fabSelectorDock->setEntries({});
    m_fabSelectorDock->setBackAvailable(false);
    m_fabSelectorDock->setVisible(false);
}

void MainWindow::useRemoteConnection(
    std::shared_ptr<remote::Connection> connection, QString label)
{
    m_remoteConnection = std::move(connection);
    m_remoteLabel = std::move(label);
    ++m_remoteConnectionGeneration;
    updateDiagnostics();
}

bool MainWindow::hasRemoteConnection() const
{
    return m_remoteConnection && m_remoteConnection->connected();
}

QString MainWindow::remoteServerExecutableFor(const QString& destination)
{
    // An explicit executable path is a property of one machine, so nothing
    // stored for another destination is consulted: a destination without its
    // own entry gets the name every install puts on the remote PATH. (An
    // earlier revision fell back to the executable last used anywhere, which
    // let one machine's path break the destinations that worked by default.)
    auto settings = makeSettings();
    const auto forDestination
        = settings
              .value(QStringLiteral("remote/serverExecutables/%1")
                      .arg(destination))
              .toString()
              .trimmed();
    return forDestination.isEmpty() ? QStringLiteral("amrexplorer-server")
                                    : forDestination;
}

void MainWindow::startSshRemoteSession(std::string destination,
    std::string serverExecutable, std::vector<std::string> remotePaths,
    std::function<void()> onReady)
{
    if (destination.empty() || destination.front() == '-'
        || destination.find_first_of(" \t\r\n") != std::string::npos) {
        reportBackgroundError(tr("Invalid SSH destination."));
        return;
    }
    const auto destinationText = QString::fromStdString(destination);
    if (serverExecutable.empty()) {
        serverExecutable
            = remoteServerExecutableFor(destinationText).toStdString();
    }
    {
        // Remember this destination and its executable; the CLI teaches the
        // dialog this way too. Per destination only -- see
        // remoteServerExecutableFor.
        auto settings = makeSettings();
        settings.setValue(
            QStringLiteral("remote/sshDestination"), destinationText);
        settings.setValue(QStringLiteral("remote/serverExecutables/%1")
                              .arg(destinationText),
            QString::fromStdString(serverExecutable));
    }
    // A previous session's connection is closed with it; a dataset still open
    // on it fails on its next request, and the new server gets fresh opens.
    m_sshRemoteSession.reset();
    m_remoteConnection.reset();
    m_remoteLabel.clear();
    m_sshRemoteSession = std::make_unique<SshRemoteSession>(this);
    statusBar()->showMessage(tr("Starting remote session on %1...")
            .arg(QString::fromStdString(destination)));
    m_sshRemoteSession->start(destination, std::move(serverExecutable),
        remote::ConnectionOptions{.clientName = "AMReXplorer Qt",
            .softwareVersion = kVersion, .sessionToken = {}},
        [this, destination, paths = std::move(remotePaths),
            onReady = std::move(onReady)](
            std::shared_ptr<remote::Connection> connection) {
            if (m_closing) {
                return;
            }
            const auto& server = connection->serverInfo();
            useRemoteConnection(std::move(connection),
                tr("ssh %1").arg(QString::fromStdString(destination)));
            statusBar()->showMessage(
                tr("Remote session on %1 is ready (%2 %3, %4 worker threads)")
                    .arg(QString::fromStdString(destination),
                        QString::fromStdString(server.serverName),
                        QString::fromStdString(server.softwareVersion))
                    .arg(server.workerCount));
            if (paths.size() == 1) {
                openRemoteDataset(paths.front());
            } else if (paths.size() > 1) {
                openRemoteSequence(paths);
            }
            if (onReady) {
                onReady();
            }
        },
        [this, destination](const QString& message) {
            if (m_closing) {
                return;
            }
            statusBar()->showMessage(tr("Could not start the remote session"));
            reportBackgroundError(tr("Could not start the remote session on "
                                     "%1: %2")
                    .arg(QString::fromStdString(destination), message));
            updateDiagnostics();
        },
        [this, destination](const QString& message) {
            if (m_closing) {
                return;
            }
            statusBar()->showMessage(tr("Remote session ended"));
            reportBackgroundError(tr("The remote session on %1 ended: %2")
                    .arg(QString::fromStdString(destination), message));
            updateDiagnostics();
        });
    // After start(): the session reports its destination only from then on.
    updateDiagnostics();
}

void MainWindow::browseRemotePlotfiles(bool sequence)
{
    if (!hasRemoteConnection()) {
        reportBackgroundError(tr("Open a remote session first "
                                 "(File > Open Remote Plotfile...)."));
        return;
    }
    // The last directory browsed is remembered per destination: a path is a
    // property of one machine, like the server executable.
    const auto destination = m_sshRemoteSession
        ? QString::fromStdString(m_sshRemoteSession->destination())
        : m_remoteLabel;
    const auto settingsKey
        = QStringLiteral("remote/lastDirectories/%1").arg(destination);
    RemoteFileDialog dialog(m_remoteConnection,
        makeSettings().value(settingsKey).toString(),
        sequence ? RemoteFileDialog::SelectionMode::PlotfileSequence
                 : RemoteFileDialog::SelectionMode::SinglePlotfile,
        this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto paths = dialog.selectedPaths();
    if (paths.empty()) {
        return;
    }
    if (!dialog.currentDirectory().isEmpty()) {
        makeSettings().setValue(settingsKey, dialog.currentDirectory());
    }
    // One plotfile picked in the sequence browser is just that plotfile.
    if (paths.size() > 1) {
        openRemoteSequence(paths);
    } else {
        openRemoteDataset(paths.front());
    }
}

void MainWindow::openRemoteDataset(std::string remotePath)
{
    if (!m_remoteConnection) {
        reportBackgroundError(tr("Open a remote session first "
                                 "(File > Open Remote Plotfile...)."));
        return;
    }
    const auto displayPath = std::filesystem::path(remotePath);
    openDatasetImpl(displayPath, false, std::nullopt, {}, false,
        std::nullopt, RemoteOpen{m_remoteConnection, std::move(remotePath)});
}

void MainWindow::openDatasetImpl(const std::filesystem::path& path,
    bool metadataOnly,
    std::optional<PlotfileMetadataResult> preparedMetadata,
    std::filesystem::path dataRoot, bool preserveFabSelector,
    std::optional<FrameSliceSpec> initialSpec,
    std::optional<RemoteOpen> remoteOpen)
{
    if (!preserveFabSelector) {
        resetFabState();
    }
    // Opening a single dataset ends any plotfile sequence and stops playback
    // of either animation mode.
    setPlaybackMode(PlaybackMode::None);
    closeSequence();
    resetRangeState();
    // The new dataset arrives fitted -- setPlaceholder below puts every view
    // back to Fit -- so the scale report has to come back with it. Without
    // this the toolbar kept claiming the previous dataset's "4x" over a fitted
    // view of the new one.
    setScaleUiState(ScaleUiState::Fit);
    // Invalidate every in-flight per-view slice and reset the view states.
    for (auto* state : allViewStates()) {
        state->stopSource.request_stop();
        ++state->sliceGeneration;
        state->view->setPlaceholder(tr("Loading dataset..."));
        // Fresh empty snapshots — the pointers must stay non-null (see
        // PlaneViewState).
        state->plane = std::make_shared<const ScalarPlane>();
        state->contourPlane = std::make_shared<const ScalarPlane>();
        // Plane rewrite: drop any in-flight sync's outcome for this view
        // (see PlaneViewState::renderGeneration).
        ++state->renderGeneration;
        state->contourPolylines.clear();
        state->fieldName.clear();
        state->visibleRegion.reset();
        state->vectorSegments.clear();
        state->gridBoxes.clear();
        state->cachedRequest = {};
        state->hasCachedRequest = false;
        state->cachedMode = DisplayMode::Raster;
        state->cachedVectorUField = 0;
        state->cachedVectorVField = 0;
        state->cachedContourCount = 0;
    }
    m_initialStopSource.request_stop();
    m_linePlotStopSource.request_stop();
    m_particleStopSource.request_stop();
    m_pendingAllViews = false;
    m_pendingViews.clear();
    m_sliceDebounce->stop();
    m_controlsReady = false;
    m_viewDimension = 0;
    if (m_activeView != nullptr) {
        m_activeView->view->setActiveBorder(false);
    }
    m_activeView = nullptr;
    m_dataset.reset();
    // The dock's edge trigger is per dataset, not per session: an open is a new
    // context, so the next update re-asserts it. Without this the flags could
    // hold (false, true) straight across a 3-D -> 3-D open -- closeSequence
    // above runs while the outgoing dataset is still installed, so the !applies
    // branch that clears them never runs -- and a dock hidden under the old
    // dataset stayed hidden under the new one, while the same hide followed by
    // a 2-D dataset reopened it. Same user action, opposite result.
    m_animationDockSequence = false;
    m_animationDockThreeD = false;
    // Line plot curves are snapshots of this dataset; drop the window.
    auto* linePlotWindow = m_linePlotWindow;
    m_linePlotWindow = nullptr;
    if (linePlotWindow != nullptr) {
        linePlotWindow->close();
    }
    // The dataset window shows this dataset's raw values; drop it too.
    closeDatasetWindow();
    if (m_contoursDialog != nullptr) {
        auto* dialog = m_contoursDialog;
        m_contoursDialog = nullptr;
        dialog->close();
    }
    // The particles dialog lists this dataset's species; the next one has its
    // own. (A sequence frame switch keeps it open -- the species are the same.)
    if (m_particlesDialog != nullptr) {
        auto* dialog = m_particlesDialog;
        m_particlesDialog = nullptr;
        dialog->close();
    }
    // The Number Format dialog is deliberately *not* closed here. Unlike the
    // contours dialog above, its setting is dataset-independent and persisted,
    // so closing it on every open only discarded whatever the user had typed
    // but not yet applied.
    m_datasetPath = path;
    m_lastBlocksRead = 0;
    m_lastCacheHits = 0;
    m_lastPayloadBytesRead = 0;
    m_cacheBudgetBytes = 0;
    m_cacheResidentBytes = 0;
    m_cachePinnedBytes = 0;
    m_cacheEvictions = 0;
    m_fieldSelector->setEnabled(false);
    m_levelSelector->setEnabled(false);
    m_rangeMode->setEnabled(false);
    m_logarithmic->setEnabled(false);
    m_boxesAction->setEnabled(false);
    m_slicePlanesAction->setEnabled(false);
    m_rangeMinimum->setEnabled(false);
    m_rangeMaximum->setEnabled(false);
    setSlicePositionControlsVisible(false);
    m_animationPanel->setSweepVisible(false);
    m_levelMenu->setEnabled(false);
    m_contoursAction->setEnabled(false);
    m_particlesAction->setEnabled(false);
    m_datasetAction->setEnabled(false);
    m_exportAnimationAction->setEnabled(false);
    m_openMetadata.reset();
    m_fileVersion.clear();
    m_probeLines.clear();
    m_vectorUField = -1;
    m_vectorVField = -1;
    m_vectorWField = -1;
    m_particleSamples.clear();
    m_selectedParticleSpecies.clear();
    m_particleSelectionInitialized = false;
    m_particleLoading = false;
    m_particleProgress->setVisible(false);
    ++m_particleGeneration;
    setWindowTitle(tr("AMReXplorer"));
    {
        auto settings = makeSettings();
        settings.setValue(QStringLiteral("lastOpenDirectory"),
            QString::fromStdString(path.parent_path().string()));
    }
    m_probeLabel->clear();
    m_colorBar->clearRange();
    const auto generation = ++m_generation;
    m_metadataStopSource.request_stop();
    m_metadataStopSource = StopSource{};
    const auto metadataCancellation = m_metadataStopSource.get_token();
    ++m_activeRequests;
    statusBar()->showMessage(tr("Reading metadata for %1...").arg(
        QString::fromStdString(path.string())));
    updateDiagnostics();

    auto* watcher = new QFutureWatcher<OpenedDataset>(this);
    connect(watcher, &QFutureWatcher<OpenedDataset>::finished, this,
        [this, watcher, generation, path, metadataOnly,
            dataRoot = std::move(dataRoot),
            initialSpec = std::move(initialSpec)]() mutable {
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                auto result = watcher->result();
                if (generation == m_generation) {
                    showMetadata(result.metadata, path);
                    // The FAB selector contents were built off-thread with the
                    // metadata (when not preserving the existing selector);
                    // here we only apply them, no file I/O on the GUI thread.
                    if (result.fabSelector) {
                        auto& fab = *result.fabSelector;
                        if (fab.matched) {
                            m_fabMode = fab.fabMode;
                            if (fab.hasSourceMetadata) {
                                m_fabSourceMetadata = result.metadata;
                            } else {
                                m_fabSourceMetadata.reset();
                            }
                        }
                        if (fab.entries.empty()) {
                            m_fabSelectorDock->setVisible(false);
                        } else {
                            m_fabSourcePath = path;
                            m_fabDataRoot = fab.root;
                            m_fabSelectorDock->setEntries(std::move(fab.entries));
                            m_fabSelectorDock->setBackAvailable(false);
                            m_fabSelectorDock->setVisible(true);
                            m_fabSelectorDock->raise();
                            updateWindowTitle();
                        }
                    }
                    emit datasetOpenFinished(true);
                    if (!metadataOnly) {
                        auto root = std::move(dataRoot);
                        if (root.empty()) {
                            root = result.session
                                ? std::filesystem::path{"."}
                                : (std::filesystem::is_directory(path)
                                      ? path
                                      : path.parent_path());
                            if (root.empty()) {
                                root = ".";
                            }
                        }
                        requestInitialSlice(path, generation,
                            std::move(result.metadata), std::move(root),
                            std::move(initialSpec),
                            std::move(result.session));
                    }
                } else {
                    ++m_staleResults;
                }
            } catch (const std::exception& error) {
                if (generation == m_generation) {
                    reportBackgroundError(tr("Cannot open dataset: %1")
                            .arg(exceptionMessage(error)));
                    // The prior dataset was torn down before this attempt even
                    // began, so leaving "Loading dataset..." up would claim a
                    // load is still coming. Name what failed instead; the
                    // controls are already disabled from the teardown, so this
                    // is the whole of the settled failure state.
                    setAllViewPlaceholders(
                        tr("Could not open %1")
                            .arg(QString::fromStdString(path.string())));
                    // The teardown's own call ran while the outgoing dataset
                    // was still installed, so a 3-D one made the Animation
                    // panel still "apply" and the guard returned. By now the
                    // dataset is gone and the panel holds nothing, so this is
                    // the call that settles it -- without it an empty dock
                    // stayed up for the rest of the session.
                    updateAnimationDockVisibility();
                    emit datasetOpenFinished(false);
                } else {
                    ++m_staleResults;
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [path, preparedMetadata = std::move(preparedMetadata),
            cancellation = metadataCancellation, preserveFabSelector,
            remoteOpen = std::move(remoteOpen)]() mutable {
        OpenedDataset opened;
        if (remoteOpen) {
            opened.session = remote::RemoteDatasetSession::open(
                std::move(remoteOpen->connection), remoteOpen->remotePath,
                initialCacheBudget(), cancellation);
            opened.metadata.metadata
                = std::make_shared<const DatasetMetadata>(
                    opened.session->metadata());
            opened.metadata.metrics
                = opened.session->metadataReadMetrics();
            opened.metadata.fileVersion
                = opened.session->fileVersion();
        } else {
            opened.metadata = preparedMetadata
                ? std::move(*preparedMetadata)
                : readDatasetMetadata(path, cancellation);
        }
        // Build the FAB selector entries here, off the GUI thread, so the
        // header scans / per-block preads it needs never freeze the event
        // loop. Skipped when the caller preserves the existing selector.
        if (!preserveFabSelector && !remoteOpen) {
            opened.fabSelector = buildFabSelector(opened.metadata, path);
        }
        return opened;
    }));
}

void MainWindow::requestInitialSlice(
    const std::filesystem::path& path, std::uint64_t generation,
    std::optional<PlotfileMetadataResult> preparedMetadata,
    std::filesystem::path dataRoot,
    std::optional<FrameSliceSpec> initialSpec,
    std::shared_ptr<DatasetSession> preparedSession)
{
    validateVectorMode();
    const auto& metadata = *m_openMetadata;
    m_viewDimension = metadata.dimension;
    // Make the correct page visible and synchronously activate its layout
    // before deriving remote viewport budgets. The 3-D grid is otherwise still
    // the hidden stacked page and reports construction-time placeholder sizes.
    m_stack->setCurrentIndex(m_viewDimension == 3 ? 1 : 0);
    if (auto* page = m_stack->currentWidget(); page != nullptr
        && page->layout() != nullptr) {
        page->layout()->activate();
    }
    const auto views = currentViews();
    // The XY view starts out as the active one in 3-D.
    setActiveView(m_viewDimension == 3
        ? m_planeViews[2] : m_view2d);
    // ...and takes keyboard focus, so the arrow-key pan works on a freshly
    // opened dataset rather than only after the view has been clicked. The
    // other setActiveView callers run mid-session, where focus belongs to
    // whatever the user is doing; this one and the sequence path are opens.
    focusActiveViewForPanning();
    // Slice positions start at the domain midpoints unless a reversible FAB
    // transition is restoring the previous MultiFab view.
    const auto dataBounds = datasetSampleBounds(metadata);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto lower = dataBounds.lower[axis];
        const auto upper = dataBounds.upper[axis];
        m_slicePosition3d[axis] = initialSpec
            ? std::clamp(initialSpec->slicePositions[axis], lower,
                std::nextafter(upper, lower))
            : lower + 0.5 * (upper - lower);
    }
    m_initialStopSource.request_stop();
    m_linePlotStopSource.request_stop();
    m_particleStopSource.request_stop();
    m_initialStopSource = StopSource{};
    const auto cancellation = m_initialStopSource.get_token();
    // The initial open uses default slice state: field 0, finest available,
    // file range (falling back to Visible when metadata statistics are
    // unavailable), linear scale, whole domain, midpoint positions.
    FrameSliceSpec spec = initialSpec.value_or(FrameSliceSpec{});
    if (!initialSpec) {
        spec.palette = m_paletteController->palette();
        spec.displayMode = m_displayMode;
        spec.includeGridBoxes = m_boxesAction->isChecked();
        spec.vectorUField =
            static_cast<std::uint32_t>(std::max(m_vectorUField, 0));
        spec.vectorVField =
            static_cast<std::uint32_t>(std::max(m_vectorVField, 0));
        spec.vectorWField =
            static_cast<std::uint32_t>(std::max(m_vectorWField, 0));
        spec.contourCount = m_contourCount;
        spec.sphericalSupersample = m_sphericalSupersample;
        spec.sphericalDisplay = m_sphericalDisplay;
    }
    const auto isRemote = std::dynamic_pointer_cast<
        remote::RemoteDatasetSession>(preparedSession) != nullptr;
    if (spec.outputSizes.size() != views.size()) {
        spec.outputSizes.clear();
        spec.outputSizes.reserve(views.size());
        for (const auto* state : views) {
            auto outputSize = sliceOutputSize(*state, isRemote);
            if (preparedSession) {
                outputSize = frameBudgetBoundedOutputSize(outputSize,
                    preparedSession->maximumResponseBytes());
            }
            spec.outputSizes.push_back(outputSize);
        }
    }
    const auto restoredSpec = initialSpec;
    // Per-view generations captured now: a view that gets a newer request
    // before the initial slices land keeps its newer data.
    std::vector<std::uint64_t> viewGenerations;
    viewGenerations.reserve(views.size());
    for (const auto* state : views) {
        viewGenerations.push_back(state->sliceGeneration);
    }
    ++m_activeRequests;
    statusBar()->showMessage(tr("Loading initial slice..."));
    updateDiagnostics();

    auto* watcher = new QFutureWatcher<InitialSliceResult>(this);
    connect(watcher, &QFutureWatcher<InitialSliceResult>::finished, this,
        [this, watcher, generation, cancellation, views, viewGenerations,
            restoredSpec, isRemote] {
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                auto result = watcher->future().takeResult();
                if (generation == m_generation) {
                    m_dataset = result.dataset;
                    m_particleSamples = std::move(result.particles);
                    if (restoredSpec) {
                        m_selectedParticleSpecies
                            = restoredSpec->particleSpecies;
                        m_particleFraction = restoredSpec->particleFraction;
                        m_particleSeed = restoredSpec->particleSeed;
                        m_particleSelectionInitialized
                            = restoredSpec->particleSelectionInitialized;
                    }
                    configureParticleControls(restoredSpec.has_value());
                    configureSliceControls();
                    if (restoredSpec) {
                        const QSignalBlocker fieldBlocker(m_fieldSelector);
                        const QSignalBlocker levelBlocker(m_levelSelector);
                        const QSignalBlocker rangeBlocker(m_rangeMode);
                        const QSignalBlocker logBlocker(m_logarithmic);
                        const auto fieldIndex = m_fieldSelector->findData(
                            restoredSpec->field);
                        if (fieldIndex >= 0) {
                            m_fieldSelector->setCurrentIndex(fieldIndex);
                        }
                        const auto levelIndex = m_levelSelector->findData(
                            restoredSpec->levelSelection);
                        if (levelIndex >= 0) {
                            m_levelSelector->setCurrentIndex(levelIndex);
                        }
                        m_rangeMode->setCurrentIndex(
                            m_rangeMode->findData(
                                static_cast<int>(restoredSpec->rangeMode)));
                        m_logarithmic->setChecked(restoredSpec->logarithmic);
                        m_trackedField =
                            m_fieldSelector->currentData().toUInt();
                        m_fieldRanges[m_trackedField] = {
                            restoredSpec->rangeMode, restoredSpec->userRange};
                        if (restoredSpec->userRange) {
                            m_rangeMinimum->setValue(
                                restoredSpec->userRange->first);
                            m_rangeMaximum->setValue(
                                restoredSpec->userRange->second);
                        }
                        updateRangeModeAvailability();
                        const auto userRange =
                            static_cast<RangeMode>(
                                m_rangeMode->currentData().toInt())
                            == RangeMode::User;
                        m_rangeMinimum->setEnabled(userRange);
                        m_rangeMaximum->setEnabled(userRange);
                        configureSlicePositionControls();
                        syncMenuChecks();
                    }
                    if (selectCacheFallbackLevel(
                            m_levelSelector, result.cacheFallbackToLevel)) {
                        configureSlicePositionControls();
                        updateRangeModeAvailability();
                        syncMenuChecks();
                    }
                    if (result.displays.size() != views.size()) {
                        throw std::runtime_error(
                            "initial slice count does not match the view set");
                    }
                    // Copied out before the loop below moves each display into
                    // showSlice, which now takes it by value. The remote
                    // resize check afterwards needs the size each slice was
                    // *requested* at, and a moved-from display is not the
                    // place to read it from -- it happens to survive today
                    // only because SliceRequest holds nothing but scalars.
                    std::vector<std::array<int, 2>> requestedSizes;
                    requestedSizes.reserve(result.displays.size());
                    for (const auto& display : result.displays) {
                        requestedSizes.push_back(display.request.outputSize);
                    }
                    for (std::size_t index = 0; index < views.size(); ++index) {
                        if (views[index]->sliceGeneration
                            != viewGenerations[index]) {
                            continue;
                        }
                        // A FAB round-trip preserved the zoom in restoredSpec and
                        // executeFrameLoad rendered the slice against it, but
                        // openDatasetImpl reset the view states. Restore the zoom
                        // from the region that actually produced this plane (so it
                        // stays in step with the slice cache key), gated on
                        // whether the spec recorded a zoom for this view — a
                        // full-domain view stays nullopt, without float-comparing
                        // regions. See fab-round-trip-loses-visible-region.
                        if (restoredSpec) {
                            const bool wasZoomed =
                                index < restoredSpec->visibleRegions.size()
                                && restoredSpec->visibleRegions[index]
                                    .has_value();
                            views[index]->visibleRegion = wasZoomed
                                ? std::optional<RealBox>{
                                    result.displays[index].request.visibleRegion}
                                : std::optional<RealBox>{};
                        }
                        showSlice(*views[index], std::move(result.displays[index]));
                    }
                    if (isRemote) {
                        // A resize during the initial worker cannot submit a
                        // slice yet because m_dataset is not published. Once
                        // that result settles, coalesce each changed viewport
                        // into exactly one request using its newest size.
                        for (std::size_t index = 0; index < views.size(); ++index) {
                            if (requestedSizes[index]
                                != sliceOutputSize(*views[index])) {
                                scheduleSliceRequest(*views[index]);
                            }
                        }
                    }
                    const auto cache = m_dataset->cacheMetrics();
                    m_cacheBudgetBytes = cache.budgetBytes;
                    m_cacheResidentBytes = cache.residentBytes;
                    m_cachePinnedBytes = cache.pinnedBytes;
                    m_cacheEvictions = cache.evictions;
                    if (result.cacheFallbackToLevel >= 0) {
                        // Non-modal: an informational cache-fallback notice must
                        // not pop a modal dialog that would block the quit path.
                        statusBar()->showMessage(cacheFallbackMessage(
                            *result.dataset, result.cacheFallbackFromLevel,
                            result.cacheFallbackToLevel));
                    }
                    emit initialSliceFinished(true);
                } else {
                    ++m_staleResults;
                }
            } catch (const std::exception& error) {
                if (generation == m_generation && !cancellation.stop_requested()) {
                    reportBackgroundError(
                        tr("Cannot load slice: %1").arg(exceptionMessage(error)));
                    // The metadata opened but its first slice did not, so the
                    // panels are still on the open's placeholder with nothing
                    // left in flight to replace it. The dataset name is known
                    // here, so say which one has no displayable slice.
                    setAllViewPlaceholders(
                        tr("Could not display %1")
                            .arg(QString::fromStdString(m_datasetPath.string())));
                    emit initialSliceFinished(false);
                } else {
                    ++m_staleResults;
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [path, generation, spec = std::move(spec), cancellation,
            preparedMetadata = std::move(preparedMetadata),
            dataRoot = std::move(dataRoot),
            preparedSession = std::move(preparedSession)]() mutable {
        if (preparedSession) {
            return executeSessionFrameLoad(
                std::move(preparedSession), spec, cancellation);
        }
        return executeFrameLoad(path, DatasetId{generation}, spec,
            initialCacheBudget(), cancellation,
            std::move(preparedMetadata), std::move(dataRoot));
    }));
}

} // namespace amrvis::qt
