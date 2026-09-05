#include "MainWindowInternal.hpp"
#include "WidgetImageExport.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <stdexcept>

namespace amrvis::qt {

namespace {

struct ExportChoices {
    bool colorBar;
    bool axes;
    bool transparent;
};

std::optional<ExportChoices> chooseExportOptions(QWidget* parent) {
    auto settings = makeSettings();
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Export options"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* colorBar = new QCheckBox(QObject::tr("Include color bar"), &dialog);
    auto* axes = new QCheckBox(QObject::tr("Include axes, labels, and ticks"), &dialog);
    colorBar->setObjectName(QStringLiteral("exportColorBar"));
    axes->setObjectName(QStringLiteral("exportAxes"));
    colorBar->setChecked(settings.value(QStringLiteral("export/colorBar"), true).toBool());
    axes->setChecked(settings.value(QStringLiteral("export/axes"), false).toBool());
    layout->addWidget(colorBar);
    layout->addWidget(axes);
    auto* background = new QComboBox(&dialog);
    background->setObjectName(QStringLiteral("exportBackground"));
    background->addItem(QObject::tr("White background"), false);
    background->addItem(QObject::tr("Transparent background (PNG)"), true);
    background->setCurrentIndex(
        settings.value(QStringLiteral("export/transparent"), false).toBool() ? 1 : 0);
    layout->addWidget(background);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    settings.setValue(QStringLiteral("export/colorBar"), colorBar->isChecked());
    settings.setValue(QStringLiteral("export/axes"), axes->isChecked());
    settings.setValue(QStringLiteral("export/transparent"), background->currentData().toBool());
    return ExportChoices{colorBar->isChecked(), axes->isChecked(),
                         background->currentData().toBool()};
}

// The result of a dataset open worker: the metadata plus, when the caller did
// not ask to preserve the existing selector, the FAB selector contents built
// alongside it (so the GUI-thread completion only blits, never reads files).
struct OpenedDataset {
    PlotfileMetadataResult metadata;
    std::optional<FabSelectorBuild> fabSelector;
    std::shared_ptr<DatasetSession> session;
};

// Confirms what the save dialog could not: `targets` are the files the export
// is about to write, `vetted` the one name the dialog returned. Anything else
// it never saw -- the name after it gained the format's suffix, and the three
// per-panel names a 3-D export derives from it. Returns false to abandon the
// export; silent, returning true, when there is nothing the dialog has not
// already asked.
//
// The default button is Cancel only when a file would be replaced. For the
// ordinary "shot" -> "shot.png" notice there is nothing to lose, and
// defaulting to Cancel there would throw the export away on the same reflexive
// Return that dismissed the dialog a moment earlier.
//
// Titled through MainWindow::tr so it shares a translation context with the
// window's other export strings rather than QObject's.
[[nodiscard]] bool confirmExportTargets(QWidget* owner, const QString& vetted,
    const QStringList& targets, const QString& format)
{
    const auto clobbered = existingExportTargets(vetted, targets);
    // Only the single-file case reports a rename: a 3-D export always writes
    // names the dialog did not see, and saying so on every one of them would
    // be a prompt per export rather than a warning about losing something.
    const bool renamed = targets.size() == 1 && targets.front() != vetted;
    if (clobbered.isEmpty() && !renamed) {
        return true;
    }
    QStringList paragraphs;
    if (renamed) {
        paragraphs << MainWindow::tr(
            "Saving as %1 instead: the image is written as %2.")
                          .arg(targets.front(), format);
    }
    if (clobbered.size() == 1) {
        paragraphs << MainWindow::tr("%1 already exists and will be replaced.")
                          .arg(clobbered.front());
    } else if (!clobbered.isEmpty()) {
        paragraphs << MainWindow::tr(
            "These files already exist and will be replaced:")
                + QLatin1Char('\n') + clobbered.join(QLatin1Char('\n'));
    }
    QMessageBox box(QMessageBox::Question, MainWindow::tr("Export Image"),
        paragraphs.join(QStringLiteral("\n\n")),
        QMessageBox::Save | QMessageBox::Cancel, owner);
    box.setDefaultButton(
        clobbered.isEmpty() ? QMessageBox::Save : QMessageBox::Cancel);
    return box.exec() == QMessageBox::Save;
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
    m_particleController->cancel();
    m_volumeController->cancel();
    m_view2d.stopSource.request_stop();
    for (auto& state : m_planeViews) {
        state.stopSource.request_stop();
    }
}

void MainWindow::restoreSettings()
{
    const auto settings = makeSettings();

    if (const auto error = m_paletteController->restore(settings)) {
        // The stored file palette did not load; the controller fell back to
        // a builtin and keeps the file as the wanted selection, so say so
        // rather than silently showing a different palette.
        reportBackgroundError(tr("Cannot load palette file %1: %2")
            .arg(settings.value(QStringLiteral("palette/filePath")).toString(),
                *error));
    }
    m_colorBar->setPalette(&m_paletteController->palette());

    m_themeController->restore(settings);

    m_range->showLogarithmic(
        settings.value(QStringLiteral("range/logarithmic"), false).toBool());
    {
        // A stored format that no longer validates falls back to the default.
        const auto format = settings.value(QStringLiteral("numberFormat"),
            defaultNumberFormat()).toString();
        m_numberFormat = isValidNumberFormat(format) ? format
            : defaultNumberFormat();
        m_range->setNumberFormat(m_numberFormat);
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
    if (m_scaleBarAction != nullptr) {
        const QSignalBlocker scaleBarBlocker(m_scaleBarAction);
        m_scaleBarVisible = settings.value(
            QStringLiteral("overlay/scaleBar"), false).toBool();
        m_scaleBarAction->setChecked(m_scaleBarVisible);
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
    settings.setValue(QStringLiteral("range/logarithmic"), m_range->logarithmic());
    m_paletteController->save(settings);
    m_themeController->save(settings);
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
    settings.setValue(QStringLiteral("overlay/scaleBar"),
        m_scaleBarVisible);
    settings.remove(QStringLiteral("scaleBar/lengthUnit"));
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
    if (m_fabNavigator->fabMode()) {
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
            m_fabNavigator->openStandaloneFab(
                std::filesystem::path(filename.toStdString()));
        } else {
            openDataset(filename.toStdString());
        }
    }
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
    const auto chosen = QFileDialog::getSaveFileName(
        this, tr("Export scalar image"), QString(),
        tr("PNG image (*.png);;FITS float64 image (*.fits *.fit)"),
        &selectedFilter);
    if (chosen.isEmpty()) {
        return;
    }

    // Which format, and the suffixes that already say it. An explicitly typed
    // recognized extension wins over the selected filter.
    const QStringList fitsSuffixes{
        QStringLiteral(".fits"), QStringLiteral(".fit")};
    const QStringList pngSuffixes{QStringLiteral(".png")};
    const bool hasFitsExtension
        = !carriedExportSuffix(chosen, fitsSuffixes).isEmpty();
    const bool hasPngExtension
        = !carriedExportSuffix(chosen, pngSuffixes).isEmpty();
    const bool fits = hasFitsExtension
        || (!hasPngExtension
            && selectedFilter.contains(QStringLiteral("*.fits")));

    // The suffix to write under, and the base name the per-panel suffixes of a
    // 3-D export hang off. A name that already says the format keeps the
    // spelling it was typed in: chopping the suffix and re-appending a
    // lowercase one -- what this did -- turned a typed "shot.PNG" into
    // "shot.png", a different file on a case-sensitive filesystem, replaced
    // without asking while the file the dialog had vetted was left alone.
    const auto carried
        = carriedExportSuffix(chosen, fits ? fitsSuffixes : pngSuffixes);
    const QString extension = !carried.isEmpty()
        ? carried
        : fits ? QStringLiteral(".fits") : QStringLiteral(".png");
    QString base = chosen;
    base.chop(carried.size());
    const QString filename = base + extension;
    // One expression builds a panel's name, so the confirmation below cannot
    // name a different set of files than the writes then produce.
    const auto panelPath = [&base, &extension](std::size_t normal) {
        constexpr std::array<const char*, 3> suffixes{"_yz", "_xz", "_xy"};
        return base + QString::fromLatin1(suffixes[normal]) + extension;
    };
    const auto formatName
        = fits ? QStringLiteral("FITS") : QStringLiteral("PNG");

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
            // Which panels, their names, and the data itself, all decided
            // before anything is written: the confirmation below runs a nested
            // event loop, so it has to cover every file at risk rather than
            // the first of three, and what lands in them has to be what was on
            // screen when the question was asked. The planes are immutable
            // shared snapshots, so holding one costs a refcount and pins it
            // against an arrival that installs a fresh pointer mid-prompt.
            std::vector<std::pair<std::shared_ptr<const ScalarPlane>, QString>>
                outputs;
            for (std::size_t normal = 0; normal < m_planeViews.size(); ++normal) {
                const auto& state = m_planeViews[normal];
                if (state.plane->width <= 0 || state.plane->height <= 0) {
                    continue;
                }
                outputs.emplace_back(state.plane, panelPath(normal));
            }
            QStringList targets;
            for (const auto& [plane, outPath] : outputs) {
                targets << outPath;
            }
            if (!confirmExportTargets(this, chosen, targets, formatName)) {
                return;
            }
            for (const auto& [plane, outPath] : outputs) {
                writePlane(outPath, *plane);
            }
        } else if (m_activeView != nullptr) {
            const auto plane = m_activeView->plane;
            if (!confirmExportTargets(this, chosen, {filename}, formatName)) {
                return;
            }
            writePlane(filename, *plane);
        }
        return;
    }

    const auto choices = chooseExportOptions(this);
    if (!choices) {
        return;
    }
    const auto options = exportOptions(choices->colorBar, choices->axes, choices->transparent);

    if (m_viewDimension == 3) {
        // Export all three panels: foo_xy.png, foo_xz.png, foo_yz.png. Which
        // ones is settled before any of them is written, so the confirmation
        // names exactly the files the writes will touch. The pictures are
        // still composed afterwards, from the views: unlike the FITS path
        // above there is no cheap snapshot to hold, and the color-bar prompt
        // has always run an event loop here, so this is no wider a window
        // than it was. The views themselves live as long as the window.
        std::vector<std::pair<ImageView*, QString>> outputs;
        for (std::size_t normal = 0; normal < m_planeViews.size(); ++normal) {
            auto* panelView = m_planeViews[normal].view;
            if (panelView == nullptr || !panelView->hasImage()) {
                continue;
            }
            outputs.emplace_back(panelView, panelPath(normal));
        }
        QStringList targets;
        for (const auto& [panelView, outPath] : outputs) {
            targets << outPath;
        }
        if (!confirmExportTargets(this, chosen, targets, formatName)) {
            return;
        }
        for (const auto& [panelView, outPath] : outputs) {
            const qreal scale = std::max(1.0,
                panelView->transform().m11());
            const QImage composite = composeExportFrame(panelView, options, scale);
            if (composite.isNull() || !composite.save(outPath, "PNG")) {
                QMessageBox::critical(this, tr("Cannot export image"),
                    tr("Could not write %1.").arg(outPath));
            }
        }
    } else {
        if (!confirmExportTargets(this, chosen, {filename}, formatName)) {
            return;
        }
        const qreal exportScale = std::max(1.0, view->transform().m11());
        const QImage composite = composeExportFrame(view, options, exportScale);
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

ExportOptions MainWindow::exportOptions(bool includeColorBar, bool includeAxes,
                                        bool transparentBackground) const {
    ExportOptions options;
    options.includeColorBar = includeColorBar;
    options.includeAxes = includeAxes;
    options.transparentBackground = transparentBackground;
    options.font = QFont(QStringLiteral("Sans Serif"));
    options.font.setStyleHint(QFont::SansSerif);
    options.numberFormat = m_numberFormat;
    options.lengthUnit = m_lengthUnitId;
    return options;
}

QImage MainWindow::composeExportFrame(const ImageView* view, const ExportOptions& options,
                                      qreal scaleFactor, ExportLayout* frozenLayout) const {
    if (view == nullptr || !view->hasImage()) {
        return {};
    }
    const PlaneViewState* state = &m_view2d;
    for (const auto& candidate : m_planeViews) {
        if (candidate.view == view) {
            state = &candidate;
            break;
        }
    }
    ExportLayout localLayout;
    auto& layout = frozenLayout != nullptr ? *frozenLayout : localLayout;
    if (layout.dataRect.isEmpty()) {
        layout = makeExportLayout(view->composedImageSize(scaleFactor), options);
    } else if (!exportAspectMatches(view->image().size(), layout)) {
        throw std::runtime_error(
            tr("The aspect ratio of panel %1 changed. "
               "Export stopped to preserve the fixed image rectangle without stretching.")
                .arg(state->label)
                .toStdString());
    }
    const auto axes =
        exportAxes(state->displayRegion, m_viewDimension, state->normal, state->coordinateSystem,
                   state->sphericalDisplay, m_dataset && m_dataset->metadata().hasPhysicalGeometry,
                   options.lengthUnit);
    ColorBarWidget colorBar;
    colorBar.setFont(layout.font);
    colorBar.setPalette(&m_paletteController->palette());
    colorBar.setNumberFormat(options.numberFormat);
    colorBar.setLogarithmic(state->displayLogarithmic);
    colorBar.setFieldRange(state->fieldName +
                               (state->displayLogarithmic ? tr(" (log)") : QString()),
                           state->displayMinimum, state->displayMaximum);
    return composeExportImage(view->composedImage(layout.dataRect.size(), &layout.font), axes,
                              options, layout, &colorBar);
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

    const auto choices = chooseExportOptions(this);
    if (!choices) {
        return;
    }

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
    beginAnimationExport(path,
                         exportOptions(choices->colorBar, choices->axes, choices->transparent));
}

void MainWindow::beginAnimationExport(const QString& path, const ExportOptions& options) {
    auto* view = m_activeView != nullptr ? m_activeView->view : nullptr;
    if (view == nullptr || !view->hasImage()
        || !m_sequenceController->hasSequence()) {
        return;
    }
    // Freeze zoom and styling now; each panel's pixel layout is established
    // by frame 0 and retained for the complete animation.
    const auto scale = std::max(1.0, view->transform().m11());
    std::vector<QString> suffixes;
    if (m_viewDimension == 3) {
        suffixes = {QStringLiteral("_yz"), QStringLiteral("_xz"),
            QStringLiteral("_xy")};
    } else {
        suffixes = {QString()};
    }
    if (!m_animationExporter->begin(path, options, m_sequenceController->frameCount(),
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
    syncDatasetWindowColors();
    connect(window, &QObject::destroyed, this, [this, window] {
        if (m_datasetWindow == window) {
            m_datasetWindow = nullptr;
        }
        clearDatasetCellHighlights();
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
    // Now, not when the window's deferred destroyed arrives: a dataset
    // replacement gets back to the event loop -- where that deletion runs --
    // with m_viewDimension already zeroed, and the marked cell has to be gone
    // before the incoming dataset's first showSlice can draw it.
    clearDatasetCellHighlights();
    auto* window = m_datasetWindow;
    m_datasetWindow = nullptr;
    if (window != nullptr) {
        window->close();
    }
}

void MainWindow::clearDatasetCellHighlights()
{
    for (auto* state : allViewStates()) {
        state->datasetCell.reset();
        state->view->setCellHighlight(std::nullopt);
    }
}

void MainWindow::syncDatasetWindowColors()
{
    if (m_datasetWindow == nullptr || m_activeView == nullptr) {
        return;
    }
    // The active view's display range, which is what the color bar is set from
    // (syncActiveViewColorControls) -- read from the same place rather than
    // passed in, so the two cannot be given different numbers.
    m_datasetWindow->setColoring(
        makeDatasetColoring(m_paletteController->palette(),
            m_activeView->displayMinimum, m_activeView->displayMaximum,
            m_activeView->displayLogarithmic));
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
    m_activeView->datasetCell = physicalCell;
    applyDatasetCellHighlight(*m_activeView);
}

void MainWindow::applyDatasetCellHighlight(PlaneViewState& state)
{
    if (!state.datasetCell) {
        return;
    }
    const auto& physicalCell = *state.datasetCell;
    const auto& plane = *state.plane;
    if (plane.width <= 0 || plane.height <= 0) {
        return;
    }
    // In 3-D the marked cell sits on one slice, and the projection below reads
    // only the two displayed axes: without this the outline would come back
    // unchanged after the plane moved off the cell it marks. Measured against
    // the displayed raster's own position, not m_slicePosition3d, which has
    // already moved ahead whenever a slice is in flight (see updateOverlay).
    if (m_dataset && m_dataset->metadata().dimension == 3
        && state.hasCachedRequest
        && !datasetCellOnDisplayedSlice(physicalCell,
            state.cachedRequest.normalDirection,
            state.cachedRequest.physicalPosition)) {
        state.view->setCellHighlight(std::nullopt);
        return;
    }
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    if (displayIsSpherical()) {
        // xAxis is r, yAxis is theta.
        const double r0 = physicalCell.lower[xAxis];
        const double r1 = physicalCell.upper[xAxis];
        const double t0 = physicalCell.lower[yAxis];
        const double t1 = physicalCell.upper[yAxis];
        const auto mapping = planeMapping(state);
        const bool valid = r1 > r0 && t1 > t0;
        // Branch on the view state's mode, matching the mapping (see
        // updateGridBoxes).
        if (state.sphericalDisplay == SphericalDisplay::RZ) {
            std::optional<QPainterPath> highlight;
            if (valid) {
                highlight = sphericalSectorPath(mapping, r0, r1, t0, t1);
            }
            state.view->setCellHighlightPath(highlight);
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
            state.view->setCellHighlight(highlight);
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
    state.view->setCellHighlight(highlight);
}

void MainWindow::openDataset(
    const std::filesystem::path& path, bool metadataOnly)
{
    openDatasetImpl(
        path, metadataOnly, std::nullopt, {}, false, std::nullopt);
}

void MainWindow::useRemoteConnection(
    std::shared_ptr<remote::Connection> connection, QString label)
{
    m_remoteSession->install(std::move(connection), std::move(label));
}

void MainWindow::startSshRemoteSession(std::string destination,
    std::string serverExecutable, std::vector<std::string> remotePaths)
{
    m_remoteSession->start(std::move(destination), std::move(serverExecutable),
        std::move(remotePaths));
}

void MainWindow::openRemoteDataset(std::string remotePath)
{
    auto connection = m_remoteSession->connection();
    if (!connection) {
        reportBackgroundError(tr("Open a remote session first "
                                 "(File > Open Remote Plotfile...)."));
        return;
    }
    const auto displayPath = std::filesystem::path(remotePath);
    openDatasetImpl(displayPath, false, std::nullopt, {}, false,
        std::nullopt, RemoteOpen{std::move(connection), std::move(remotePath)});
}

void MainWindow::openDatasetImpl(const std::filesystem::path& path,
    bool metadataOnly,
    std::optional<PlotfileMetadataResult> preparedMetadata,
    std::filesystem::path dataRoot, bool preserveFabSelector,
    std::optional<FrameSliceSpec> initialSpec,
    std::optional<RemoteOpen> remoteOpen)
{
    if (!preserveFabSelector) {
        m_fabNavigator->reset();
    }
    // Opening a single dataset ends any plotfile sequence and stops playback
    // of either animation mode.
    setPlaybackMode(PlaybackMode::None);
    closeSequence();
    resetRangeState();
    resetLengthUnit();
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
        // Cleared, not stale: there is no raster to converge, and the open
        // about to run will stamp whatever it displays. Set after the bump
        // below would be too late -- resliceReplacedViews could run first.
        state->planeSessionEpoch = m_sessionEpoch + 1;
    }
    m_initialStopSource.request_stop();
    m_linePlotStopSource.request_stop();
    m_particleController->cancel();
    m_volumeController->cancel();
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
    // Nothing is installed now, which is a change of session like any other:
    // a slice still on a worker for the outgoing one must not be displayed.
    ++m_sessionEpoch;
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
    // And the volume window, which rendered this dataset's field.
    m_volumeController->reset();
    if (m_contoursDialog != nullptr) {
        auto* dialog = m_contoursDialog;
        m_contoursDialog = nullptr;
        dialog->close();
    }
    // The particles dialog lists this dataset's species; the next one has its
    // own. (A sequence frame switch keeps it open -- the species are the same.)
    m_particleController->closeDialog();
    // Number Format is dataset-independent and persisted, so its dialog stays
    // open without discarding a selection the user had not yet applied.
    m_datasetPath = path;
    m_diagnosticsModel->resetDatasetMetrics();
    m_fieldSelector->setEnabled(false);
    m_levelSelector->setEnabled(false);
    m_range->setControlsReady(false);
    m_boxesAction->setEnabled(false);
    m_scaleBarAction->setEnabled(false);
    m_slicePlanesAction->setEnabled(false);
    setSlicePositionControlsVisible(false);
    m_animationPanel->setSweepVisible(false);
    m_levelMenu->setEnabled(false);
    m_contoursAction->setEnabled(false);
    m_particleController->suspendAction();
    // The dataset is gone as of the reset above, so the Variable menu's field
    // entries name a session that no longer exists: triggering one would drive
    // a combo that is merely disabled, not emptied. This is the caller the
    // menu's no-dataset branch was written for.
    rebuildVariableMenu({});
    m_derivedFields->refreshAvailability();
    m_datasetAction->setEnabled(false);
    m_exportAnimationAction->setEnabled(false);
    m_openMetadata.reset();
    m_fileVersion.clear();
    m_vectorUField = -1;
    m_vectorVField = -1;
    m_vectorWField = -1;
    // Runtime particle state goes (the load was cancelled above); the
    // settings stay, because a restore reinstalls only what its spec carries
    // (the species selection is part of that, so it is cleared here and comes
    // back from the spec, or is reset with everything else by
    // configureForDataset when there is none).
    m_particleController->clearSamples();
    m_particleController->clearSelection();
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
    m_diagnosticsModel->adjustActivity(1);
    statusBar()->showMessage(tr("Reading metadata for %1...").arg(
        QString::fromStdString(path.string())));
    updateDiagnostics();

    auto* watcher = new QFutureWatcher<OpenedDataset>(this);
    connect(watcher, &QFutureWatcher<OpenedDataset>::finished, this,
        [this, watcher, generation, path, metadataOnly,
            dataRoot = std::move(dataRoot),
            initialSpec = std::move(initialSpec)]() mutable {
            m_diagnosticsModel->adjustActivity(-1);
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
                        m_fabNavigator->applySelectorBuild(
                            std::move(*result.fabSelector), path,
                            result.metadata);
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
                            SliceLoad{std::move(result.session), std::nullopt});
                    }
                } else {
                    m_diagnosticsModel->noteStaleResult();
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
                    m_diagnosticsModel->noteStaleResult();
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [path, preparedMetadata = std::move(preparedMetadata),
            cancellation = metadataCancellation, preserveFabSelector,
            remoteOpen = std::move(remoteOpen),
            // Copied here, on the GUI thread: the store has no locking and any
            // window's Apply mutates it, so reading it inside the worker would
            // race.
            derivedFields = m_derivedFields->definitions()]() mutable {
        OpenedDataset opened;
        if (remoteOpen) {
            opened.session = openRemoteSessionForLoad(remoteOpen->connection,
                remoteOpen->remotePath, derivedFields, cancellation);
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
            opened.fabSelector
                = FabNavigator::buildSelector(opened.metadata, path);
        }
        return opened;
    }));
}

bool MainWindow::SliceLoad::isRemote() const
{
    return reopen.has_value()
        || std::dynamic_pointer_cast<remote::RemoteDatasetSession>(session)
            != nullptr;
}

std::optional<std::uint32_t> MainWindow::SliceLoad::responseBytes() const
{
    if (session) {
        return session->maximumResponseBytes();
    }
    if (reopen && reopen->connection) {
        // What the session would report once it exists: the frame size the
        // connection negotiated.
        return reopen->connection->serverInfo().maximumFrameBytes;
    }
    return std::nullopt;
}

void MainWindow::requestInitialSlice(
    const std::filesystem::path& path, std::uint64_t generation,
    std::optional<PlotfileMetadataResult> preparedMetadata,
    std::filesystem::path dataRoot,
    std::optional<FrameSliceSpec> initialSpec,
    SliceLoad load)
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
    m_particleController->cancel();
    m_volumeController->cancel();
    m_initialStopSource = StopSource{};
    const auto cancellation = m_initialStopSource.get_token();
    // The initial open uses default slice state: field 0, finest available,
    // file range (falling back to Visible when metadata statistics are
    // unavailable), linear scale, whole domain, midpoint positions.
    FrameSliceSpec spec = initialSpec.value_or(FrameSliceSpec{});
    if (!initialSpec) {
        // An open with no spec has nothing else to carry the list; one built
        // by buildFrameSpec has decided already and must not be
        // second-guessed here, since two rules for one field is how they
        // drift. A prepared session's field list is fixed before this window
        // sees it, which is a property of the load rather than of the window,
        // so it is asked here and the rest through the shared predicate.
        spec.derivedFields
            = load.session || !derivedFieldsReachNextLoad()
            ? std::vector<DerivedFieldDefinition>{}
            : m_derivedFields->definitions();
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
    const auto isRemote = load.isRemote();
    if (spec.outputSizes.size() != views.size()) {
        spec.outputSizes.clear();
        spec.outputSizes.reserve(views.size());
        for (const auto* state : views) {
            auto outputSize = sliceOutputSize(*state, isRemote);
            if (const auto responseBytes = load.responseBytes()) {
                outputSize
                    = frameBudgetBoundedOutputSize(outputSize, responseBytes);
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
    m_diagnosticsModel->adjustActivity(1);
    statusBar()->showMessage(tr("Loading initial slice..."));
    updateDiagnostics();

    auto* watcher = new QFutureWatcher<InitialSliceResult>(this);
    connect(watcher, &QFutureWatcher<InitialSliceResult>::finished, this,
        [this, watcher, generation, cancellation, views, viewGenerations,
            restoredSpec, isRemote] {
            m_diagnosticsModel->adjustActivity(-1);
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                auto result = watcher->future().takeResult();
#ifdef AMREXPLORER_QT_TEST_ACCESS
                // Before anything is installed, which is where a load that
                // failed gives up: the session this one opened dies with
                // `result` and the arm below finds the previous one still on
                // screen. See failNextInitialSliceForTest.
                if (std::exchange(m_failNextInitialSliceForTest, false)) {
                    throw std::runtime_error("initial slice failed for test");
                }
#endif
                if (generation == m_generation) {
                    const auto previousVectorFields = vectorFieldNames();
                    m_dataset = result.dataset;
                    ++m_sessionEpoch;
                    // Which views a newer request has already claimed. Worked
                    // out before anything below restores control state,
                    // because that restoration replays the field, level and
                    // range this load was launched with -- and if the user
                    // moved any of them while it ran, replaying the old values
                    // silently reverts what they did, and the re-slice this
                    // load owes those views then renders the reverted
                    // selection. The controls are one per window, so once any
                    // view has been superseded the user's current state has to
                    // win for all of them.
                    //
                    // A request queued behind the debounce counts as much as
                    // one already dispatched: an edit only starts that timer,
                    // and sliceGeneration does not move until it flushes, so a
                    // load completing inside the window between the two would
                    // find nothing superseded and replay the spec over what
                    // the user had just done -- and the flush that followed
                    // would then render the replayed values. Every path that
                    // launches a load empties the queue first, so anything in
                    // it now was queued while this load ran.
                    bool superseded
                        = m_pendingAllViews || !m_pendingViews.empty();
                    for (std::size_t index = 0;
                        index < views.size() && !superseded; ++index) {
                        superseded = views[index]->sliceGeneration
                            != viewGenerations[index];
                    }
                    // By name, and before configureSliceControls below, which
                    // repopulates the selector and calls selectFieldItem(0):
                    // after that the user's choice is gone from the widget, and
                    // the restore that follows would put back the field the
                    // load was *launched* with. Ids renumber across a reload
                    // when the derived tail changes, so the name is the only
                    // stable handle.
                    const auto supersededField = superseded
                        ? m_fieldSelector->currentText()
                        : QString{};
                    // The level and the range are read here for the same
                    // reason: configureSliceControls puts the level combo back
                    // to its first row, and the spec restore below writes both
                    // outright. The level travels as the combo's data rather
                    // than its position, which is what the spec carries too --
                    // the reloaded session need not offer the same rows.
                    std::optional<int> supersededLevel;
                    std::optional<RangeController::Selection> supersededRange;
                    if (superseded) {
                        if (m_levelSelector->currentIndex() >= 0) {
                            supersededLevel
                                = m_levelSelector->currentData().toInt();
                        }
                        supersededRange = m_range->selection();
                    }
                    restoreVectorFields(previousVectorFields);
                    m_particleController->setSamples(
                        std::move(result.particles));
                    if (restoredSpec) {
                        m_particleController->restoreSelection(
                            restoredSpec->particleSpecies,
                            restoredSpec->particleFraction,
                            restoredSpec->particleSeed,
                            restoredSpec->particleSelectionInitialized);
                    }
                    m_particleController->configureForDataset(
                        restoredSpec.has_value());
                    m_volumeController->configureForDataset();
                    // Before configureSliceControls, which reads the open
                    // metadata this replaces: ensureVectorFieldDefaults checks
                    // the just-restored vector fields against it, and against
                    // the outgoing list an index that has gone out of range
                    // still looks valid. The sequence path shows the metadata
                    // first for the same reason.
                    //
                    // Only when the session's fields are not the ones already
                    // listed: the open path filled the dock from the file's
                    // own metadata a moment ago, and rebuilding it costs a
                    // copy of every level's box list and a tree item per box.
                    // What it can miss is the derived fields the session
                    // installed, and, after a reload, the ones that have gone.
                    const auto& sessionFields = m_dataset->metadata().fields;
                    const auto listed = m_openMetadata
                        && std::equal(m_openMetadata->fields.begin(),
                            m_openMetadata->fields.end(),
                            sessionFields.begin(), sessionFields.end(),
                            [](const FieldMetadata& shown,
                                const FieldMetadata& session) {
                                return shown.name == session.name;
                            });
                    if (!listed) {
                        refreshMetadataDisplay();
                    }
                    configureSliceControls();
                    if (restoredSpec) {
                        const QSignalBlocker fieldBlocker(m_fieldSelector);
                        const QSignalBlocker levelBlocker(m_levelSelector);
                        // The field the load was actually rendered with,
                        // which the pipeline resolves by name and so need not
                        // be the id the spec carried (resolveSpecField).
                        // Selecting the spec's id would name one field in the
                        // combo while the view showed another.
                        const auto rendered = result.displays.empty()
                            ? restoredSpec->field
                            : result.displays.front().request.field.value;
                        const auto fieldIndex =
                            m_fieldSelector->findData(rendered);
                        if (fieldIndex >= 0) {
                            m_fieldSelector->setCurrentIndex(fieldIndex);
                        }
                        const auto levelIndex = m_levelSelector->findData(
                            restoredSpec->levelSelection);
                        if (levelIndex >= 0) {
                            m_levelSelector->setCurrentIndex(levelIndex);
                        }
                        m_range->setSelection({restoredSpec->rangeMode,
                            restoredSpec->userRange, restoredSpec->logarithmic});
                        m_range->setTrackedField(
                            m_fieldSelector->currentText());
                        m_range->commitFieldRange(m_range->trackedField());
                        // The menu was rebuilt by configureSliceControls
                        // above, while the combo still sat on field 0; it is
                        // the same selection shown twice, so it has to follow
                        // the combo here. (The sequence path rebuilds its menu
                        // after selecting, so it needs none of this.)
                        syncVariableMenu();
                        // Before the extraction the User bounds were written
                        // unblocked here, which scheduled a (redundant)
                        // re-slice; kept so this path renders as it did.
                        if (restoredSpec->userRange) {
                            scheduleSliceRequest();
                        }
                        updateRangeModeAvailability();
                        configureSlicePositionControls();
                        syncMenuChecks();
                    }
                    // The user moved the field while this load ran, so theirs
                    // is the selection that stands -- otherwise the re-slice
                    // this load owes their view renders the field they left.
                    bool fieldRestored = false;
                    if (!supersededField.isEmpty()) {
                        const auto index
                            = m_fieldSelector->findText(supersededField);
                        // Only a row that is a field. A definition this
                        // session skipped is listed under the same name,
                        // greyed and carrying no id, and setCurrentIndex takes
                        // one as readily as any other -- leaving the combo,
                        // the Variable menu and the range on that name while
                        // every reader of currentData() renders field 0. Where
                        // the user's field did not survive the reload there is
                        // nothing of theirs to restore, and the field the load
                        // rendered stands.
                        if (index >= 0
                            && m_fieldSelector->itemData(index).isValid()) {
                            const QSignalBlocker blocker(m_fieldSelector);
                            m_fieldSelector->setCurrentIndex(index);
                            m_range->setTrackedField(
                                m_fieldSelector->currentText());
                            syncVariableMenu();
                            fieldRestored = true;
                        }
                    }
                    if (!fieldRestored) {
                        // A range belongs to the field it was set on, and the
                        // memory is keyed by that field's name. Restoring it
                        // over a field the user was not looking at would
                        // render that field with their bounds and remember
                        // them there.
                        supersededRange.reset();
                    }
                    // The level and the range that same interaction moved, put
                    // back after the field: the range widgets represent the
                    // selected field, and its name is the key commitFieldRange
                    // files them under. Blocked, as the spec restore above is
                    // -- the re-slice these views are owed comes from
                    // resliceReplacedViews below, and a signal from here would
                    // queue a second one for every view.
                    if (supersededLevel) {
                        const auto index
                            = m_levelSelector->findData(*supersededLevel);
                        if (index >= 0) {
                            const QSignalBlocker blocker(m_levelSelector);
                            m_levelSelector->setCurrentIndex(index);
                            configureSlicePositionControls();
                            syncMenuChecks();
                        }
                    }
                    if (supersededRange) {
                        m_range->setSelection(*supersededRange);
                        m_range->commitFieldRange(m_range->trackedField());
                    }
                    if (supersededLevel || supersededRange) {
                        // A mode the restored field and level cannot offer
                        // falls back here rather than staying selected and
                        // unavailable, which is what the spec restore's own
                        // call does for the values it put back.
                        updateRangeModeAvailability();
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
                    // Views this load will not display, because a newer
                    // request for them was submitted while it ran. Collected
                    // rather than merely skipped: each is still showing a
                    // raster produced by the session installed *before* this
                    // one, while m_dataset, the field selector, the range
                    // widgets and the colour bar have all just become the new
                    // session's. Leaving them is the catalog-vs-pixels
                    // mismatch -- after an edit that renumbers the derived
                    // tail, values from the old expression under the new field
                    // list -- and it is why an arrival-side check cannot close
                    // this on its own: in the common ordering that newer
                    // request finished before this completion ran and was
                    // rightly accepted against the session then installed.
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
                        showSlice(*views[index],
                            std::move(result.displays[index]), m_sessionEpoch);
                    }
                    // Re-sliced against the session just installed. Driven
                    // off each view's stamp rather than the list above, so a
                    // debt survives the next reload clearing the debounce: if
                    // that reload fails it re-checks the same stamps and the
                    // view is asked again.
                    resliceReplacedViews();
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
                    m_diagnosticsModel->setCacheMetrics(cache);
                    if (result.cacheFallbackToLevel >= 0) {
                        // Non-modal: an informational cache-fallback notice must
                        // not pop a modal dialog that would block the quit path.
                        statusBar()->showMessage(cacheFallbackMessage(
                            *result.dataset, result.cacheFallbackFromLevel,
                            result.cacheFallbackToLevel));
                    }
                    emit initialSliceFinished(true);
                    // Emitted first: this load did finish, and what follows
                    // is a fresh one. A window counts as unable to take
                    // derived fields for the whole of an open, so a list
                    // committed meanwhile reached every window but this one,
                    // and a FAB return reopens from a spec captured before any
                    // of it. Either way the session says so itself.
                    reloadIfDefinitionsMoved();
                } else {
                    m_diagnosticsModel->noteStaleResult();
                }
            } catch (const std::exception& error) {
                if (generation == m_generation && !cancellation.stop_requested()) {
                    reportBackgroundError(
                        tr("Cannot load slice: %1").arg(exceptionMessage(error)));
                    // The metadata opened but its first slice did not, so the
                    // panels are still on the open's placeholder with nothing
                    // left in flight to replace it. The dataset name is known
                    // here, so say which one has no displayable slice.
                    //
                    // Only when nothing is installed, which is what tells an
                    // open from a reload: a reload never resets m_dataset, so
                    // the session on screen is still live and its rasters are
                    // still what it produced. Wiping them because a reopen
                    // failed -- a definition the server would not take, a
                    // connection that went away -- would throw away a working
                    // display over a change that simply did not happen.
                    if (!m_dataset) {
                        setAllViewPlaceholders(
                            tr("Could not display %1")
                                .arg(QString::fromStdString(
                                    m_datasetPath.string())));
                    }
                    // A reload that started and then failed must not count as
                    // one that happened: the list is still uninstalled, and
                    // pressing Apply again cannot ask for it a second time
                    // because DerivedFieldStore::set returns without emitting
                    // for a list that has not moved. Left armed, the window
                    // stayed on the older list for good while the editor
                    // reported it applied. This cannot loop the way the memo
                    // guards against: it runs when a load completes, not once
                    // per event-loop turn.
                    m_reloadAskedFor.reset();
                    // The previous session is still installed and still fine,
                    // so this does not wipe the display -- it asks any view
                    // still showing an even older session's raster for one of
                    // the installed session's. A reload dropped between the
                    // debounce and this failure would otherwise be lost.
                    resliceReplacedViews();
                    emit initialSliceFinished(false);
                } else {
                    m_diagnosticsModel->noteStaleResult();
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [path, generation, spec = std::move(spec), cancellation,
            preparedMetadata = std::move(preparedMetadata),
            dataRoot = std::move(dataRoot),
            load = std::move(load)]() mutable {
        if (load.session) {
            return executeSessionFrameLoad(
                std::move(load.session), spec, cancellation);
        }
        if (load.reopen) {
            // The quiet reopen of a remote dataset: same loader the sequence
            // frames use, so the connected() pre-check and the derived-field
            // capability gate are the ones written once.
            return loadRemoteFrame(load.reopen->connection,
                load.reopen->remotePath, 0, spec, cancellation);
        }
        return executeFrameLoad(path, DatasetId{generation}, spec,
            initialCacheBudget(), cancellation,
            std::move(preparedMetadata), std::move(dataRoot));
    }));
#ifdef AMREXPLORER_QT_TEST_ACCESS
    // The one moment a test can act *during* a load: the spec above is built
    // and the work is running, while the completion is a queued signal that
    // cannot be delivered until this returns. Changing the derived-field list
    // here is what another window's Apply does to a window that is opening.
    if (m_initialSliceLaunchedForTest) {
        m_initialSliceLaunchedForTest();
    }
#endif
}

} // namespace amrvis::qt
