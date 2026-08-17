#include "MainWindowInternal.hpp"
#include "SshRemoteSession.hpp"

#include "CurrentRowBulletDelegate.hpp"

#include <QKeySequence>
#include <QStyle>

namespace amrvis::qt {

extern const char* const kVersion =
#ifdef AMREXPLORER_VERSION
    AMREXPLORER_VERSION
#else
    "0.2.0-dev"
#endif
    ;

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("AMReXplorer"));
    resize(960, 720);

    // The palette selection lives in its controller. Consumers keep a pointer
    // to its palette(), the toolbar shows its selector, the View menu its
    // menu, and paletteChanged (wired at the end of construction, once the
    // widgets its handler touches exist) drives the color bar and a re-render.
    m_paletteController = new PaletteController(this);
    connect(m_paletteController, &PaletteController::loadFileRequested, this,
        [this] { loadPaletteFile(); });

    // The plot area is a stacked widget: page 0 holds the single 2-D view,
    // page 1 the 3-D grid (XY top-left, XZ top-right, YZ bottom-left, iso
    // wireframe bottom-right).
    m_stack = new QStackedWidget(this);

    m_view2d.normal = 1;
    m_view2d.label = QStringLiteral("2-D");
    m_view2d.view = new ImageView(m_stack);
    m_view2d.view->setMinimumSize(320, 240);
    m_view2d.view->setPlaceholder(tr("Open an AMReX dataset to display a slice"));
    m_stack->addWidget(m_view2d.view);

    auto* gridPage = new QWidget(m_stack);
    auto* gridLayout = new QGridLayout(gridPage);
    gridLayout->setSpacing(2);
    gridLayout->setContentsMargins(2, 2, 2, 2);
    constexpr std::array<const char*, 3> viewLabels{"YZ", "XZ", "XY"};
    // Per-panel L-shaped axis indicator in the lower-left corner.
    constexpr std::array<const char*, 3> hAxis{"Y", "X", "X"};
    constexpr std::array<const char*, 3> vAxis{"Z", "Z", "Y"};
    for (int normal = 0; normal < 3; ++normal) {
        const auto idx = static_cast<std::size_t>(normal);
        auto& state = m_planeViews[idx];
        state.normal = normal;
        state.label = QString::fromLatin1(viewLabels[idx]);
        state.view = new ImageView(gridPage);
        state.view->setMinimumSize(200, 150);
        state.view->setSliceMoveEnabled(true);
        state.view->setPlaceholder(tr("%1 view").arg(state.label));
        state.view->setAxisIndicator(
            QString::fromLatin1(hAxis[idx]),
            QString::fromLatin1(vAxis[idx]));
    }
    m_isoWidget = new IsoWidget(gridPage);
    m_isoWidget->setColorPalette(&m_paletteController->palette());
    gridLayout->addWidget(m_planeViews[2].view, 0, 0);  // XY: plane normal to Z
    gridLayout->addWidget(m_planeViews[1].view, 0, 1);  // XZ: plane normal to Y
    gridLayout->addWidget(m_planeViews[0].view, 1, 0);  // YZ: plane normal to X
    gridLayout->addWidget(m_isoWidget, 1, 1);
    gridLayout->setColumnStretch(0, 1);
    gridLayout->setColumnStretch(1, 1);
    gridLayout->setRowStretch(0, 1);
    gridLayout->setRowStretch(1, 1);
    m_stack->addWidget(gridPage);
    m_stack->setCurrentIndex(0);
    setCentralWidget(m_stack);

    m_sliceToolbar = addToolBar(tr("Slice Controls"));
    auto* sliceToolbar = m_sliceToolbar;
    sliceToolbar->setMovable(false);
    sliceToolbar->addWidget(new QLabel(tr("Field:"), sliceToolbar));
    m_fieldSelector = new QComboBox(sliceToolbar);
    m_fieldSelector->setObjectName(QStringLiteral("fieldSelector"));
    m_fieldSelector->setMinimumContentsLength(10);
    m_fieldSelector->view()->setItemDelegate(new CurrentRowBulletDelegate(
        m_fieldSelector, m_fieldSelector->view()));
    sliceToolbar->addWidget(m_fieldSelector);
    sliceToolbar->addSeparator();
    sliceToolbar->addWidget(new QLabel(tr("Level:"), sliceToolbar));
    m_levelSelector = new QComboBox(sliceToolbar);
    m_levelSelector->setObjectName(QStringLiteral("levelSelector"));
    m_levelSelector->setMinimumContentsLength(8);
    m_levelSelector->view()->setItemDelegate(new CurrentRowBulletDelegate(
        m_levelSelector, m_levelSelector->view()));
    sliceToolbar->addWidget(m_levelSelector);
    sliceToolbar->addSeparator();
    // 3-D shared slice positions: one compact spinbox per axis. The whole
    // group stays hidden for 2-D datasets.
    m_slicePositionControls = new QWidget(sliceToolbar);
    auto* positionLayout = new QHBoxLayout(m_slicePositionControls);
    positionLayout->setContentsMargins(0, 0, 0, 0);
    positionLayout->setSpacing(4);
    positionLayout->addWidget(new QLabel(tr("Position:"), m_slicePositionControls));
    constexpr std::array<const char*, 3> axisLabels{"X:", "Y:", "Z:"};
    for (int axis = 0; axis < 3; ++axis) {
        positionLayout->addWidget(new QLabel(
            QString::fromLatin1(axisLabels[static_cast<std::size_t>(axis)]),
            m_slicePositionControls));
        auto* spin = new QSpinBox(m_slicePositionControls);
        spin->setMinimumWidth(110);
        positionLayout->addWidget(spin);
        m_sliceSpinboxes[static_cast<std::size_t>(axis)] = spin;
        connect(spin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this, axis](int index) {
                if (!m_controlsReady || !m_dataset
                    || m_dataset->metadata().dimension != 3) {
                    return;
                }
                const auto level = sliceIndexLevel();
                if (level < 0 || static_cast<std::size_t>(level)
                    >= m_dataset->metadata().levels.size()) {
                    return;
                }
                setSlicePosition(axis, positionForSliceIndex(
                    m_dataset->metadata(), level, axis, index));
            });
    }
    sliceToolbar->addWidget(m_slicePositionControls);
    // Separator between the Position group and Scale. It tracks the Position
    // group's visibility (see setSlicePositionControlsVisible) so it does not
    // dangle beside the Level separator when no dataset is loaded.
    m_positionSeparator = sliceToolbar->addSeparator();
    setSlicePositionControlsVisible(false);

    // A static "Scale:" label plus a state button, matching the Field:/Level:/
    // Range: label-and-widget pairs elsewhere on this toolbar (and the
    // View -> Scale menu name).
    sliceToolbar->addWidget(new QLabel(tr("Scale:"), sliceToolbar));
    m_scaleButton = new QPushButton(tr("Fit"), sliceToolbar);
    m_scaleButton->setToolTip(defaultScaleToolTip());
    m_scaleButton->setFocusPolicy(Qt::NoFocus);
    auto* scaleMenu = new QMenu(m_scaleButton);
    // The clicked item stays "Reset Zoom" (the action verb): it restores the
    // whole domain and refits (issue #45 renamed it from "Fit", which read as
    // fit-the-current-region). The button shows just the *scale state* ("Fit"
    // for auto-fit, which also holds for a panned crop in applyPanStep where
    // the region is not the whole domain); the adjacent "Scale:" label names
    // the control.
    auto* resetZoomAction = scaleMenu->addAction(tr("Reset Zoom"));
    // Straight to resetZoomAllViews, which is where the single Fit report
    // lives: the View menu's Reset Zoom connects to the same slot, and the two
    // are meant to be one action reached two ways.
    connect(resetZoomAction, &QAction::triggered, this,
        &MainWindow::resetZoomAllViews);
    constexpr std::array<int, 6> scaleFactors{1, 2, 4, 8, 16, 32};
    for (const auto factor : scaleFactors) {
        auto* action = scaleMenu->addAction(plainScaleLabel(factor));
        connect(action, &QAction::triggered, this, [this, factor] {
            // Apply first, report second: the report asks the view whether it
            // is on a virtual canvas and what region its raster covers, and
            // neither is settled until applyFixedScale has run. Derived, not
            // asserted -- applyFixedScale only touches currentViews(), and
            // setFixedScale early-returns on a view with no image, so before a
            // dataset (or after a failed open) the factor reaches nothing and
            // claiming it would put a number on the button no view backed.
            applyFixedScale(factor);
            refreshScaleReport();
        });
    }
    m_syncRubberBandZoomAction =
        new QAction(tr("Sync Rubber-band Zoom"), this);
    m_syncRubberBandZoomAction->setObjectName(
        QStringLiteral("syncRubberBandZoomAction"));
    m_syncRubberBandZoomAction->setCheckable(true);
    m_syncRubberBandZoomAction->setChecked(true);
    m_syncRubberBandZoomAction->setVisible(false);
    m_syncRubberBandZoomAction->setStatusTip(
        tr("Apply rubber-band selections to every 3-D panel; "
           "mouse-wheel zoom remains panel-specific"));
    connect(m_syncRubberBandZoomAction, &QAction::toggled,
        this, [this](bool) { saveSettings(); });
    scaleMenu->addSeparator();
    scaleMenu->addAction(m_syncRubberBandZoomAction);
    m_scaleButton->setMenu(scaleMenu);
    sliceToolbar->addWidget(m_scaleButton);

    addToolBarBreak(Qt::TopToolBarArea);
    m_rangeToolbar = addToolBar(tr("Color and Overlay Controls"));
    auto* rangeToolbar = m_rangeToolbar;
    rangeToolbar->setMovable(false);
    rangeToolbar->addWidget(new QLabel(tr("Range:"), rangeToolbar));
    m_rangeMode = new QComboBox(rangeToolbar);
    m_rangeMode->setObjectName(QStringLiteral("rangeModeSelector"));
    m_rangeMode->addItem(tr("File"), static_cast<int>(RangeMode::File));
    m_rangeMode->addItem(tr("Level"), static_cast<int>(RangeMode::Level));
    m_rangeMode->addItem(tr("Visible"), static_cast<int>(RangeMode::Visible));
    m_rangeMode->addItem(tr("User"), static_cast<int>(RangeMode::User));
    rangeToolbar->addWidget(m_rangeMode);
    m_rangeMinimum = new ScientificDoubleSpinBox(rangeToolbar);
    m_rangeMaximum = new ScientificDoubleSpinBox(rangeToolbar);
    for (auto* range : {m_rangeMinimum, m_rangeMaximum}) {
        range->setRange(-std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max());
        range->setMinimumWidth(110);
        range->setEnabled(false);
        rangeToolbar->addWidget(range);
    }
    m_rangeMinimum->setPrefix(tr("min "));
    m_rangeMaximum->setPrefix(tr("max "));
    m_rangeMaximum->setValue(1.0);
    // Separate the Range group (mode + min/max) from Log and Palette, matching
    // the per-group separators on the Slice Controls toolbar.
    rangeToolbar->addSeparator();
    m_logarithmic = new QCheckBox(tr("Log"), rangeToolbar);
    rangeToolbar->addWidget(m_logarithmic);
    rangeToolbar->addSeparator();
    rangeToolbar->addWidget(new QLabel(tr("Palette:"), rangeToolbar));
    rangeToolbar->addWidget(m_paletteController->createSelector(rangeToolbar));

    m_sliceDebounce = new QTimer(this);
    m_sliceDebounce->setSingleShot(true);
    m_sliceDebounce->setInterval(100);
    connect(m_sliceDebounce, &QTimer::timeout, this, [this] { flushSliceRequests(); });
    m_panDebounce = new QTimer(this);
    m_panDebounce->setSingleShot(true);
    m_panDebounce->setInterval(120);
    connect(m_panDebounce, &QTimer::timeout, this, [this] { flushPanDrag(false); });
    connect(m_fieldSelector, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int index) {
            // Swap the per-field range snapshot before re-slicing. This only
            // fires on a real user selection -- per-frame repopulation during
            // animation blocks signals and preserves the index, so the range
            // stays constant across frames.
            if (m_controlsReady && index >= 0) {
                const auto newField = m_fieldSelector->itemData(index).toUInt();
                if (newField != m_trackedField) {
                    commitFieldRange(m_trackedField);
                    m_trackedField = newField;
                    applyFieldRange(newField);
                }
            }
            // A deferred full-domain range store (m_pendingRangeStore) is keyed
            // to the field/level/mode in effect when it was queued. Changing any
            // of them means a completing 3-D Visible sync would store its union
            // under the wrong key, so drop the pending store here (the sync
            // completion also re-checks the key; see range-cache-staleness-races).
            m_pendingRangeStore.reset();
            updateRangeModeAvailability();
            scheduleSliceRequest();
        });
    connect(m_levelSelector, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) {
            configureSlicePositionControls();
            m_pendingRangeStore.reset();  // see the field selector above
            updateRangeModeAvailability();
            scheduleSliceRequest();
        });
    connect(m_rangeMode, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) {
            m_pendingRangeStore.reset();  // see the field selector above
            updateRangeModeAvailability();
            const auto userRange = static_cast<RangeMode>(
                m_rangeMode->currentData().toInt()) == RangeMode::User;
            m_rangeMinimum->setEnabled(userRange && m_controlsReady);
            m_rangeMaximum->setEnabled(userRange && m_controlsReady);
            scheduleSliceRequest();
        });
    connect(m_rangeMinimum, qOverload<double>(&QDoubleSpinBox::valueChanged),
        this, [this](double) {
            if (static_cast<RangeMode>(m_rangeMode->currentData().toInt())
                == RangeMode::User) {
                scheduleSliceRequest();
            }
        });
    connect(m_rangeMaximum, qOverload<double>(&QDoubleSpinBox::valueChanged),
        this, [this](double) {
            if (static_cast<RangeMode>(m_rangeMode->currentData().toInt())
                == RangeMode::User) {
                scheduleSliceRequest();
            }
        });
    connect(m_logarithmic, &QCheckBox::toggled,
        this, [this](bool) { scheduleSliceRequest(); });
    m_fieldSelector->setEnabled(false);
    m_levelSelector->setEnabled(false);
    m_rangeMode->setEnabled(false);
    m_logarithmic->setEnabled(false);

    m_metadataDock = new QDockWidget(tr("Dataset Metadata"), this);
    m_metadataTree = new QTreeWidget(m_metadataDock);
    m_metadataTree->setColumnCount(2);
    m_metadataTree->setHeaderLabels({tr("Property"), tr("Value")});
    m_metadataTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_metadataTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_metadataDock->setWidget(m_metadataTree);
    addDockWidget(Qt::LeftDockWidgetArea, m_metadataDock);
    m_metadataDock->setVisible(false);

    // The Diagnostics panel's counters, metrics and histories live in their
    // model; it renders the dock, and this window supplies the lines only it
    // knows.
    m_diagnosticsModel = new DiagnosticsModel(
        DiagnosticsModel::Hooks{
            [this] { return m_generation; },
            [this] { return m_sequenceController->lastFrameSwitchMs(); },
            [this] { return remoteDiagnosticsLines(); },
        },
        this);
    m_diagnosticsDock = m_diagnosticsModel->createDock(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_diagnosticsDock);

    m_colorBarDock = new QDockWidget(tr("Color Scale"), this);
    m_colorBar = new ColorBarWidget(m_colorBarDock);
    m_colorBarDock->setWidget(m_colorBar);
    addDockWidget(Qt::RightDockWidgetArea, m_colorBarDock);

    m_animationDock = new QDockWidget(tr("Animation"), this);
    m_animationPanel = new AnimationPanel(m_animationDock);
    m_animationDock->setWidget(m_animationPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_animationDock);
    // Shown only for 3-D datasets (slice sweep) or plotfile sequences; hidden
    // until updateAnimationDockVisibility() decides otherwise.
    m_animationDock->setVisible(false);

    // FAB/MultiFab navigation lives in its navigator; it opens what it
    // resolves through this window's openDatasetImpl and folds its header
    // reads into the diagnostics.
    m_fabNavigator = new FabNavigator(
        FabNavigator::Hooks{
            [this] { return m_generation; },
            [this] { return m_closing; },
            // Unguarded on purpose: buildFrameSpec reads the controls, not
            // the dataset, and the MultiFab-return record must capture them
            // even when a click lands mid-reload with no dataset installed;
            // gating here degraded that record to defaults.
            [this]() -> std::optional<FrameSliceSpec> {
                return buildFrameSpec();
            },
            [this](const std::filesystem::path& path,
                PlotfileMetadataResult metadata, std::filesystem::path dataRoot,
                bool preserveSelector, std::optional<FrameSliceSpec> spec) {
                openDatasetImpl(path, false, std::move(metadata),
                    std::move(dataRoot), preserveSelector, std::move(spec));
            },
        },
        this);
    connect(m_fabNavigator, &FabNavigator::loadActivityChanged, this,
        [this](int delta) {
            m_diagnosticsModel->adjustActivity(delta);
            updateDiagnostics();
        });
    connect(m_fabNavigator, &FabNavigator::staleResultDropped, this,
        [this] {
            m_diagnosticsModel->noteStaleResult();
            updateDiagnostics();
        });
    connect(m_fabNavigator, &FabNavigator::openFailed, this,
        [this](const QString& title, const QString& message) {
            reportBackgroundError(tr("%1: %2").arg(title, message));
        });
    connect(m_fabNavigator, &FabNavigator::windowTitleChanged, this,
        [this] { updateWindowTitle(); });
    m_fabSelectorDock = m_fabNavigator->createDock(this);
    addDockWidget(Qt::LeftDockWidgetArea, m_fabSelectorDock);

    // One playback timer drives either animation mode; starting one mode
    // stops the other (see setPlaybackMode).
    m_playbackTimer = new QTimer(this);
    connect(m_playbackTimer, &QTimer::timeout, this, [this] { playbackTick(); });
    // The sequence controller owns the frame/prefetch state machine; this
    // window supplies the GUI-coupled hooks (spec snapshot, frame display,
    // shutdown flag) and reacts to its signals below.
    m_sequenceController = new SequenceController(
        SequenceController::Hooks{
            [this] { return buildFrameSpec(); },
            [this](InitialSliceResult& result, bool defaultPositions) {
                displayFrameResult(result, defaultPositions);
            },
            [this] { return m_closing; },
        },
        this);
    connect(m_sequenceController, &SequenceController::frameSwitchStarted,
        this, [this](int index) {
            // Cancel the current dataset's in-flight work, exactly like
            // opening a fresh dataset does, but keep the view state (field,
            // level, range, log, palette, zoom, slice positions) for the
            // next frame.
            for (auto* state : allViewStates()) {
                state->stopSource.request_stop();
                ++state->sliceGeneration;
            }
            m_initialStopSource.request_stop();
            m_linePlotStopSource.request_stop();
            m_particleController->cancel();
            m_pendingAllViews = false;
            m_pendingViews.clear();
            m_sliceDebounce->stop();
            // The dataset window shows the previous frame's raw values;
            // drop it, and the line plot window whose curves are snapshots
            // of this dataset, so neither goes stale across the switch.
            closeDatasetWindow();
            auto* linePlotWindow = m_linePlotWindow;
            m_linePlotWindow = nullptr;
            if (linePlotWindow != nullptr) {
                linePlotWindow->close();
            }
            ++m_generation;
            m_datasetPath = m_sequenceController->framePath(index);
            m_animationPanel->setSequenceFrame(index);
        });
    connect(m_sequenceController, &SequenceController::loadActivityChanged,
        this, [this](int delta) {
            m_diagnosticsModel->adjustActivity(delta);
            updateDiagnostics();
        });
    connect(m_sequenceController, &SequenceController::staleResultDropped,
        this, [this] {
            m_diagnosticsModel->noteStaleResult();
            updateDiagnostics();
        });
    connect(m_sequenceController, &SequenceController::statusMessage,
        this, [this](const QString& message) {
            statusBar()->showMessage(message);
        });

    // The particle overlay's selection, samples and sample load live in their
    // controller; this window draws the samples, decides how a changed
    // selection is applied (reload, or restart an in-flight sequence frame so
    // it is baked into the frame spec), and folds the load's bookkeeping into
    // its diagnostics.
    m_particleController = new ParticleController(
        ParticleController::Hooks{
            [this] { return m_dataset; },
            [this] { return m_closing; },
        },
        this);
    connect(m_particleController, &ParticleController::overlaysChanged, this,
        [this] { updateParticleOverlays(); });
    connect(m_particleController, &ParticleController::sampleSelectionChanged,
        this, [this] {
            m_sequenceController->invalidatePrefetch();
            if (m_sequenceController->inFlight()
                && m_sequenceController->currentIndex() >= 0) {
                goToSequenceFrame(m_sequenceController->currentIndex(), true);
            } else {
                m_particleController->reload();
            }
        });
    connect(m_particleController, &ParticleController::loadActivityChanged,
        this, [this](int delta) {
            m_diagnosticsModel->adjustActivity(delta);
            updateDiagnostics();
        });
    connect(m_particleController, &ParticleController::statusMessage, this,
        [this](const QString& message, int timeoutMs) {
            statusBar()->showMessage(message, timeoutMs);
        });
    connect(m_particleController, &ParticleController::loadFailed, this,
        [this](const QString& message) { reportBackgroundError(message); });
    connect(m_particleController, &ParticleController::staleResultDropped,
        this, [this] { m_diagnosticsModel->noteStaleResult(); });
    connect(m_particleController, &ParticleController::loadFinished, this,
        [this] { updateDiagnostics(); });
    connect(m_sequenceController, &SequenceController::frameDisplayed,
        this, [this](int index) {
            m_animationPanel->setSequenceFrame(index);
            m_animationPanel->setSequenceInfo(
                QString::fromStdString(m_datasetPath.filename().string()),
                m_openMetadata->time);
            updateDiagnostics();
            emit sequenceFrameDisplayed(index);
        });
    connect(m_sequenceController, &SequenceController::frameLoadFailed,
        this, [this](const QString& message) {
            statusBar()->showMessage(tr("Frame load failed"));
            // Stop playing. Playback wraps, so without this it comes back to
            // the same unreadable frame -- or the same disconnected server --
            // every cycle, raising a diagnostic each time. The user is left on
            // the failed frame and can step or play again once they have dealt
            // with it. A sweep is left alone; only sequence playback can hit a
            // frame load.
            if (m_playbackMode == PlaybackMode::Sequence) {
                setPlaybackMode(PlaybackMode::None);
            }
            // During animation export the failure is reported by the export
            // handler; avoid a second dialog.
            const bool wasExporting = m_animationExporter->active();
            emit sequenceFrameFailed();
            if (!wasExporting) {
                reportBackgroundError(
                    tr("Cannot load frame: %1").arg(message));
            }
            updateDiagnostics();
        });

    // Animation export advances one frame at a time as each renders. The
    // exporter owns the whole export state machine; this window supplies
    // frame rendering and navigation, and restores its UI on finished().
    m_animationExporter = new AnimationExporter(
        [this](bool includeColorBar, qreal scale) {
            std::vector<std::pair<QString, QImage>> frames;
            if (m_viewDimension == 3) {
                constexpr std::array<const char*, 3> suffixes{
                    "_yz", "_xz", "_xy"};
                for (int normal = 0; normal < 3; ++normal) {
                    const auto idx = static_cast<std::size_t>(normal);
                    auto* panelView = m_planeViews[idx].view;
                    if (panelView == nullptr || !panelView->hasImage()) {
                        continue;
                    }
                    frames.emplace_back(QString::fromLatin1(suffixes[idx]),
                        composeExportFrame(panelView, includeColorBar, scale));
                }
            } else {
                frames.emplace_back(QString(), composeExportFrame(
                    m_activeView != nullptr ? m_activeView->view : nullptr,
                    includeColorBar, scale));
            }
            return frames;
        },
        [this](int index) { goToSequenceFrame(index); },
        this);
    connect(m_animationExporter, &AnimationExporter::encodingStarted,
        this, &MainWindow::exportEncodingStarted);
    connect(m_animationExporter, &AnimationExporter::finished, this,
        [this](bool success, const QString& message, int restoreIndex) {
            // Return the user to the frame they were viewing (unless we are
            // closing, which would launch a new frame load mid-shutdown).
            if (!m_closing && m_sequenceController->hasSequence()) {
                goToSequenceFrame(restoreIndex < 0 ? 0 : restoreIndex);
            }
            m_exportAnimationAction->setEnabled(
                m_sequenceController->hasSequence());
            if (!m_closing) {
                if (success) {
                    QMessageBox::information(
                        this, tr("Export Animation"), message);
                } else {
                    reportBackgroundError(message);
                }
            }
        });
    connect(this, &MainWindow::sequenceFrameDisplayed,
        m_animationExporter, &AnimationExporter::onFrameDisplayed);
    connect(this, &MainWindow::sequenceFrameFailed,
        m_animationExporter, &AnimationExporter::onFrameFailed);
    applySpeed();
    connect(m_animationPanel, &AnimationPanel::sweepStepRequested, this,
        [this](int direction) { stepSweep(direction); });
    connect(m_animationPanel, &AnimationPanel::sweepPlayToggled, this,
        [this] { toggleSweepPlayback(); });
    connect(m_animationPanel, &AnimationPanel::sequenceStepRequested, this,
        [this](int direction) { stepSequence(direction); });
    connect(m_animationPanel, &AnimationPanel::sequencePlayToggled, this,
        [this] { toggleSequencePlayback(); });
    connect(m_animationPanel, &AnimationPanel::sequenceFrameRequested, this,
        [this](int index) { goToSequenceFrame(index); });
    connect(m_animationPanel, &AnimationPanel::speedChanged, this,
        [this](int) {
            applySpeed();
            saveSettings();
        });

    createMenus();

    connect(m_fieldSelector, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) {
            syncMenuChecks();
            syncVariableMenu();
        });
    connect(m_levelSelector, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) { syncMenuChecks(); });
    // No saveSettings here: range mode is deliberately not persisted (see
    // saveSettings), so the call only ever rewrote unrelated keys.
    connect(m_logarithmic, &QCheckBox::toggled,
        this, [this](bool) { saveSettings(); });

    wireView(m_view2d);
    for (auto& state : m_planeViews) {
        wireView(state);
    }

    m_probeLabel = new QLabel(statusBar());
    statusBar()->addPermanentWidget(
        m_particleController->createProgress(statusBar()));
    statusBar()->addPermanentWidget(m_probeLabel);
    statusBar()->showMessage(tr("No dataset open"));
    updateDiagnostics();
    restoreSettings();
    // Only now: the handler persists settings and redraws through the color
    // bar, views and iso widget, all of which exist by here, and restoreSettings
    // above read the restored palette into the color bar itself.
    connect(m_paletteController, &PaletteController::paletteChanged, this,
        [this] {
            saveSettings();
            refreshPaletteDisplay();
        });
    // Cancel in-flight async work on any quit path (last-window close, Cmd-Q,
    // menu Quit) so QThreadPool teardown does not block on an outstanding read
    // and the process can exit promptly. Only here -- where every window is
    // going away -- is it safe to also drop the shared global pool's queued
    // jobs, so teardown skips starting work that would only observe its stop
    // token and exit; a per-window close must not (see cancelInFlight and
    // window-close-clears-shared-thread-pool).
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        cancelInFlight();
        if (auto* pool = QThreadPool::globalInstance()) {
            pool->clear();
        }
    });
}

MainWindow::~MainWindow() = default;

void MainWindow::promptRemoteOpen(bool sequence)
{
    QDialog dialog(this);
    dialog.setWindowTitle(sequence ? tr("Open Remote Plotfile Sequence")
                                   : tr("Open Remote Plotfile"));
    // Wide enough that a typical scratch-filesystem path is visible whole.
    dialog.setMinimumWidth(560);
    auto* layout = new QFormLayout(&dialog);
    auto* explanation = new QLabel(
        tr("AMReXplorer runs amrexplorer-server on the destination through "
           "ssh and talks to it over that connection. Any destination that "
           "works for the ssh command works here, including aliases from "
           "~/.ssh/config. An unchanged destination keeps the current "
           "session. Enter the plotfile path, or use Browse... to pick it "
           "on the remote machine."),
        &dialog);
    explanation->setWordWrap(true);
    layout->addRow(explanation);
    // Prefill from the live session so opening another path reuses it; a
    // fresh window falls back to the last destination used anywhere.
    const auto sessionDestination = m_sshRemoteSession
        ? QString::fromStdString(m_sshRemoteSession->destination())
        : QString();
    auto* destinationEdit = new QLineEdit(
        sessionDestination.isEmpty()
            ? makeSettings()
                  .value(QStringLiteral("remote/sshDestination"))
                  .toString()
            : sessionDestination,
        &dialog);
    destinationEdit->setPlaceholderText(tr("user@host or ssh alias"));
    layout->addRow(tr("SSH destination:"), destinationEdit);
    auto* executableEdit = new QLineEdit(
        remoteServerExecutableFor(destinationEdit->text().trimmed()),
        &dialog);
    executableEdit->setToolTip(
        tr("Name on the remote PATH, or a path such as "
           "~/bin/amrexplorer-server. Remembered per destination."));
    layout->addRow(tr("Remote amrexplorer-server:"), executableEdit);
    // The executable follows the destination (each remembers its own) until
    // the user edits it in this dialog; textEdited fires only on user edits.
    auto executableEdited = std::make_shared<bool>(false);
    connect(executableEdit, &QLineEdit::textEdited, &dialog,
        [executableEdited] { *executableEdited = true; });
    connect(destinationEdit, &QLineEdit::textChanged, &dialog,
        [executableEdit, executableEdited](const QString& text) {
            if (!*executableEdited) {
                executableEdit->setText(
                    remoteServerExecutableFor(text.trimmed()));
            }
        });
    QLineEdit* pathEdit = nullptr;
    QPlainTextEdit* pathsEdit = nullptr;
    if (sequence) {
        pathsEdit = new QPlainTextEdit(&dialog);
        pathsEdit->setPlaceholderText(
            tr("One plotfile path per line, in playback order"));
        pathsEdit->setTabChangesFocus(true);
        layout->addRow(tr("Plotfile paths on the remote machine:"), pathsEdit);
    } else {
        pathEdit = new QLineEdit(&dialog);
        pathEdit->setPlaceholderText(tr("/path/to/plt00010 or ~/run/plt00010"));
        layout->addRow(tr("Plotfile path on the remote machine:"), pathEdit);
    }
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Open"));
    // Browse... needs only the connection fields: it starts (or reuses) the
    // session and picks the path in the remote browser once it is ready.
    auto* browseButton = buttons->addButton(
        tr("Browse..."), QDialogButtonBox::ActionRole);
    browseButton->setToolTip(
        tr("Connect and choose the plotfile on the remote machine"));
    bool browse = false;
    connect(browseButton, &QPushButton::clicked, &dialog, [&] {
        browse = true;
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto destination = destinationEdit->text().trimmed();
    const auto executable = executableEdit->text().trimmed();
    const bool sessionMatches = hasRemoteConnection() && m_sshRemoteSession
        && destination.toStdString() == m_sshRemoteSession->destination()
        && executable.toStdString() == m_sshRemoteSession->serverExecutable();
    if (browse) {
        if (destination.isEmpty() || executable.isEmpty()) {
            QMessageBox::warning(this, dialog.windowTitle(),
                tr("The SSH destination and the server executable are "
                   "required."));
            return;
        }
        if (sessionMatches) {
            browseRemotePlotfiles(sequence);
        } else {
            startSshRemoteSession(destination.toStdString(),
                executable.toStdString(), {},
                [this, sequence] { browseRemotePlotfiles(sequence); });
        }
        return;
    }
    std::vector<std::string> paths;
    if (sequence) {
        for (const auto& line : pathsEdit->toPlainText().split(
                 QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const auto path = line.trimmed();
            if (!path.isEmpty()) {
                paths.push_back(path.toStdString());
            }
        }
    } else if (!pathEdit->text().trimmed().isEmpty()) {
        paths.push_back(pathEdit->text().trimmed().toStdString());
    }
    if (destination.isEmpty() || executable.isEmpty() || paths.empty()) {
        QMessageBox::warning(this, dialog.windowTitle(),
            tr("The SSH destination, the server executable, and at least one "
               "plotfile path are required."));
        return;
    }
    // An unchanged destination and executable mean the current session is the
    // one asked for; open over it directly instead of starting ssh again.
    if (sessionMatches) {
        if (sequence) {
            openRemoteSequence(paths);
        } else {
            openRemoteDataset(paths.front());
        }
        return;
    }
    startSshRemoteSession(
        destination.toStdString(), executable.toStdString(),
        std::move(paths));
}

void MainWindow::wireView(PlaneViewState& state)
{
    auto* view = state.view;
    view->setFocusPolicy(Qt::StrongFocus);
    connect(view, &ImageView::probeClicked, this,
        [this, &state](int x, int displayY) { probeClicked(state, x, displayY); });
    connect(view, &ImageView::probeMoved, this,
        [this, &state](int x, int displayY) { probeMoved(state, x, displayY); });
    connect(view, &ImageView::rubberBandSelected, this,
        [this, &state](const QRectF& sceneRect) { rubberBandZoom(state, sceneRect); });
    connect(view, &ImageView::panDragBegan, this,
        [this, &state] { beginPanDrag(state); });
    connect(view, &ImageView::panDragMoved, this,
        [this, &state](const QPointF& totalSceneDelta, const QPoint& viewportDelta) {
            updatePanDrag(state, totalSceneDelta, viewportDelta);
        });
    connect(view, &ImageView::panDragEnded, this,
        [this, &state](const QPointF& totalSceneDelta) {
            endPanDrag(state, totalSceneDelta);
        });
    connect(view, &ImageView::linePlotRequested, this,
        [this, &state](int x, int y, Qt::MouseButton button) {
            linePlotRequested(state, x, y, button);
        });
    connect(view, &ImageView::sliceMoveRequested, this,
        [this, &state](int x, int y, Qt::MouseButton button) {
            sliceMoveRequested(state, x, y, button);
        });
    // A wheel zoom demotes the view to Custom on its own; without this the
    // Scale button kept reading "32x" over a view that was no longer applying
    // it. This is the last path that moved the view without telling the
    // reporter.
    // No active-view guard: wheeling a *non-active* 3-D panel demotes only
    // that panel, which is exactly the disagreement refreshScaleReport reads
    // every view to find. Guarding on the active view suppressed the Mixed the
    // signal was added to surface.
    connect(view, &ImageView::zoomChanged, this,
        [this] { refreshScaleReport(); });
    connect(view, &ImageView::fitRequested, this,
        [this, &state] {
            resetViewZoom(state);
            // Derived, not asserted: this fits *one* panel, so with sync off in
            // 3-D the others keep whatever they had and the honest report is
            // Mixed. Hardcoding Fit here claimed all three had been fitted.
            refreshScaleReport();
        });
    connect(view, &ImageView::viewportResized, this,
        [this, &state](const QSize&) {
            if (!m_dataset || !std::dynamic_pointer_cast<
                    remote::RemoteDatasetSession>(m_dataset)) {
                return;
            }
            if (remoteDemandCanvas(state)) {
                updateRemoteFixedScaleDemand(state);
                return;
            }
            if (!state.hasCachedRequest
                || state.cachedRequest.outputSize != sliceOutputSize(state)) {
                scheduleSliceRequest(state);
            }
        });
    connect(view, &ImageView::canvasScrolled, this,
        [this, &state] { updateRemoteFixedScaleDemand(state); });
    connect(view, &ImageView::panStepRequested, this,
        [this, &state](const QPointF& direction) {
            ++m_panStepRequests;
            applyPanStep(state, direction);
        });
}

std::array<MainWindow::PlaneViewState*, 4> MainWindow::allViewStates()
{
    return {&m_view2d, &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
}

void MainWindow::setAllViewPlaceholders(const QString& text)
{
    for (auto* state : allViewStates()) {
        // Never over a panel that has something to show. Both callers are
        // catch blocks spanning the whole display path, so a throw from the
        // third panel reaches here with the first two already rendered, and
        // blanking those would turn a partial failure into a total one --
        // behind four placeholders, over a live and interactive dataset.
        if (state->view->hasImage()) {
            continue;
        }
        state->view->setPlaceholder(text);
    }
}

std::vector<MainWindow::PlaneViewState*> MainWindow::currentViews()
{
    if (m_viewDimension == 3) {
        return {&m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
    }
    if (m_viewDimension == 2) {
        return {&m_view2d};
    }
    return {};
}

void MainWindow::setActiveView(PlaneViewState& state)
{
    if (m_activeView == &state) {
        return;
    }
    if (m_activeView != nullptr && m_viewDimension == 3) {
        m_activeView->view->setActiveBorder(false);
    }
    m_activeView = &state;
    if (m_viewDimension == 3) {
        state.view->setActiveBorder(true);
    }
    // The clamped scale report is computed over the active view's axis pair,
    // so switching 3-D panels can change it: a panel whose axes both fit under
    // the clamp is not reduced even when the one beside it is. Re-state it
    // here rather than leaving a number, and a tool tip naming a limit, that
    // describes the panel the user just left.
    refreshScaleReport();
    if (state.plane->width <= 0 || state.plane->height <= 0) {
        return;
    }
    syncActiveViewColorControls(state);
}

void MainWindow::syncActiveViewColorControls(const PlaneViewState& state)
{
    // The color scale and range boxes track the active view.
    m_colorBar->setLogarithmic(state.displayLogarithmic);
    m_colorBar->setFieldRange(state.displayLogarithmic
        ? state.fieldName + tr(" (log)") : state.fieldName,
        state.displayMinimum, state.displayMaximum);
    if (m_logarithmic->isChecked() != state.displayLogarithmic) {
        const QSignalBlocker logarithmicBlocker(m_logarithmic);
        m_logarithmic->setChecked(state.displayLogarithmic);
    }
    if (static_cast<RangeMode>(m_rangeMode->currentData().toInt())
        != RangeMode::User) {
        const QSignalBlocker minimumBlocker(m_rangeMinimum);
        const QSignalBlocker maximumBlocker(m_rangeMaximum);
        m_rangeMinimum->setValue(state.displayMinimum);
        m_rangeMaximum->setValue(state.displayMaximum);
    }
}

std::array<int, 2> MainWindow::displayAxes(int normal) const
{
    std::array<int, 2> axes{0, 1};
    if (m_dataset && m_dataset->metadata().dimension == 3) {
        std::size_t next = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis != normal) {
                axes[next++] = axis;
            }
        }
    }
    return axes;
}

std::array<int, 2> MainWindow::nativeOutputSize(
    const PlaneViewState& state) const
{
    if (!m_openMetadata || m_openMetadata->levels.empty()) {
        return {1, 1};
    }
    const auto target = state.visibleRegion.value_or(
        datasetSampleBounds(*m_openMetadata));
    return finestNativeOutputSize(
        *m_openMetadata, target, state.normal);
}

std::array<int, 2> MainWindow::sliceOutputSize(
    const PlaneViewState& state, bool forceRemote) const
{
    if (!forceRemote
        && !std::dynamic_pointer_cast<remote::RemoteDatasetSession>(m_dataset)) {
        return nativeOutputSize(state);
    }
    if (!m_openMetadata || m_openMetadata->levels.empty()) {
        return {1, 1};
    }
    const auto viewportPixels = viewportPixelSize(state);
    const auto target = state.visibleRegion.value_or(
        datasetSampleBounds(*m_openMetadata));
    std::array<int, 2> outputSize{};
    if (state.view->transformMode() == ImageView::TransformMode::FixedScale) {
        outputSize = finestNativeOutputSize(
            *m_openMetadata, target, state.normal);
    } else if (!state.visibleRegion.has_value()) {
        outputSize = nativeBoundedViewportOutputSize(
            *m_openMetadata, target, state.normal, viewportPixels);
    } else {
        // Rubber-band selections intentionally retain their exact physical
        // aspect. Their fractional edges need not have the rounded native-cell
        // aspect used to suppress Fit supersampling on a whole-domain view.
        outputSize = viewportBoundedOutputSize(
            *m_openMetadata, target, state.normal, viewportPixels);
    }
    return frameBudgetBoundedOutputSize(
        outputSize,
        m_dataset ? m_dataset->maximumResponseBytes() : std::nullopt);
}

std::array<int, 2> MainWindow::viewportPixelSize(
    const PlaneViewState& state) const
{
    const auto* viewport = state.view == nullptr ? nullptr : state.view->viewport();
    if (viewport == nullptr || viewport->width() < 1 || viewport->height() < 1) {
        return {1, 1};
    }
    const auto scale = state.view->devicePixelRatioF();
    return {
        std::clamp(static_cast<int>(std::lround(viewport->width() * scale)),
            1, maxSliceOutputDimension),
        std::clamp(static_cast<int>(std::lround(viewport->height() * scale)),
            1, maxSliceOutputDimension)};
}

QSize MainWindow::logicalImageSize(const PlaneViewState& state,
    const ScalarPlane& plane, const QImage& image) const
{
    if (!m_openMetadata || m_openMetadata->levels.empty()
        || displayIsSpherical()) {
        return image.size();
    }
    const auto native = finestNativeOutputSize(
        *m_openMetadata, plane.physicalRegion, state.normal);
    return {native[0], native[1]};
}

bool MainWindow::displayIsSpherical() const
{
    return m_dataset && isSpherical2D(m_dataset->metadata());
}

bool MainWindow::displayIsSphericalWarp() const
{
    return displayIsSpherical() && m_sphericalDisplay == SphericalDisplay::RZ;
}

void MainWindow::updateSphericalControls()
{
    const bool spherical = displayIsSpherical();
    if (m_sphericalMenu != nullptr) {
        m_sphericalMenu->setEnabled(spherical);
    }
    if (m_sphericalSupersampleMenu != nullptr) {
        // Supersampling only affects the R-Z warp.
        m_sphericalSupersampleMenu->setEnabled(
            spherical && m_sphericalDisplay == SphericalDisplay::RZ);
    }
}

std::array<QString, 2> MainWindow::sphericalAxisLabels(SphericalDisplay mode)
{
    const QString theta(QChar(0x03B8));
    switch (mode) {
    case SphericalDisplay::RTheta:
        return {QStringLiteral("r"), theta};
    case SphericalDisplay::ThetaR:
        return {theta, QStringLiteral("r")};
    case SphericalDisplay::RZ:
    default:
        return {QStringLiteral("R"), QStringLiteral("Z")};
    }
}

PlaneMapping MainWindow::planeMapping(const PlaneViewState& state) const
{
    PlaneMapping mapping;
    mapping.spherical = displayIsSpherical();
    mapping.mode = state.sphericalDisplay;
    mapping.logicalRegion = state.plane->physicalRegion;
    mapping.displayRegion = state.displayRegion;
    mapping.sceneWidth = std::max(1, state.view->image().width());
    mapping.sceneHeight = std::max(1, state.view->image().height());
    mapping.planeWidth = std::max(1, state.plane->width);
    mapping.planeHeight = std::max(1, state.plane->height);
    return mapping;
}

void MainWindow::createMenus()
{
    auto* newWindowAction = new QAction(tr("Open &New Window"), this);
    newWindowAction->setShortcut(QKeySequence::New);
    connect(newWindowAction, &QAction::triggered, this, [this] { createNewWindow(); });

    auto* openAction = new QAction(tr("&Open Plotfile Directory..."), this);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, [this] { chooseDataset(); });

    auto* openSequenceAction = new QAction(tr("Open Plotfile &Sequence..."), this);
    connect(openSequenceAction, &QAction::triggered, this,
        [this] { choosePlotfileSequence(); });

    auto* openRemoteAction = new QAction(
        tr("Open Remote &Plotfile..."), this);
    connect(openRemoteAction, &QAction::triggered, this,
        [this] { promptRemoteOpen(false); });

    auto* openRemoteSequenceAction = new QAction(
        tr("Open &Remote Plotfile Sequence..."), this);
    connect(openRemoteSequenceAction, &QAction::triggered, this,
        [this] { promptRemoteOpen(true); });

    auto* openFabAction = new QAction(tr("Open &FAB..."), this);
    connect(openFabAction, &QAction::triggered, this,
        [this] { chooseStandaloneDataset(tr("Open AMReX FAB"), true); });

    auto* openMultiFabAction = new QAction(tr("Open &MultiFab..."), this);
    connect(openMultiFabAction, &QAction::triggered, this,
        [this] {
            chooseStandaloneDataset(tr("Open AMReX MultiFab header"), false);
        });

    auto* paletteMenu = m_paletteController->createMenu(this);

    auto* exportAction = new QAction(tr("&Export Image..."), this);
    connect(exportAction, &QAction::triggered, this, [this] { exportImage(); });

    m_exportAnimationAction = new QAction(tr("Export &Animation..."), this);
    m_exportAnimationAction->setEnabled(false);
    connect(m_exportAnimationAction, &QAction::triggered,
        this, [this] { exportAnimation(); });

    auto* quitAction = new QAction(tr("&Quit"), this);
    quitAction->setShortcut(QKeySequence::Quit);
    // Application-wide: close every main window (each runs its own close
    // handling) rather than just this one.
    connect(quitAction, &QAction::triggered, qApp, &QApplication::closeAllWindows);

    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(newWindowAction);
    fileMenu->addSeparator();
    fileMenu->addAction(openAction);
    fileMenu->addAction(openSequenceAction);
    fileMenu->addSeparator();
    fileMenu->addAction(openRemoteAction);
    fileMenu->addAction(openRemoteSequenceAction);
    fileMenu->addSeparator();
    fileMenu->addAction(openFabAction);
    fileMenu->addAction(openMultiFabAction);
    fileMenu->addSeparator();
    fileMenu->addAction(exportAction);
    fileMenu->addAction(m_exportAnimationAction);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAction);

    m_scaleGroup = new QActionGroup(this);
    auto* scaleMenu = new QMenu(tr("&Scale"), this);
    m_resetZoomAction = new QAction(tr("&Reset Zoom"), scaleMenu);
    m_resetZoomAction->setCheckable(true);
    m_resetZoomAction->setActionGroup(m_scaleGroup);
    m_resetZoomAction->setChecked(true);
    m_resetZoomAction->setShortcut(QKeySequence(Qt::Key_0));
    connect(m_resetZoomAction, &QAction::triggered,
        this, [this] { resetZoomAllViews(); });
    scaleMenu->addAction(m_resetZoomAction);
    constexpr std::array<int, 6> fixedScales{1, 2, 4, 8, 16, 32};
    for (std::size_t index = 0; index < fixedScales.size(); ++index) {
        const auto factor = fixedScales[index];
        auto* action = new QAction(plainScaleLabel(factor), scaleMenu);
        action->setCheckable(true);
        action->setActionGroup(m_scaleGroup);
        action->setShortcut(QKeySequence(Qt::Key_1 + static_cast<int>(index)));
        connect(action, &QAction::triggered, this, [this, factor] {
            applyFixedScale(factor);   // then report; see the toolbar menu
            refreshScaleReport();
        });
        scaleMenu->addAction(action);
    }
    scaleMenu->addSeparator();
    scaleMenu->addAction(m_syncRubberBandZoomAction);

    // "2-D Spherical" groups the options specific to warped spherical
    // (r, theta) -> (R, Z) display; the whole submenu is enabled only while
    // such a dataset is shown (see showSlice). More options will be added here.
    m_sphericalMenu = new QMenu(tr("2-D Spherical"), this);
    m_sphericalMenu->setEnabled(false);

    // Display layout: the physical R-Z warp, or the logical r-theta / theta-r
    // (transposed) grid. Only R-Z uses the supersample control below.
    m_sphericalDisplayGroup = new QActionGroup(this);
    m_sphericalDisplayMenu = new QMenu(tr("&Display"), this);
    const QString theta(QChar(0x03B8));
    const std::array<SphericalDisplay, 3> displayModes{
        SphericalDisplay::RZ, SphericalDisplay::RTheta, SphericalDisplay::ThetaR};
    const std::array<QString, 3> displayLabels{
        tr("R-Z (physical)"), QStringLiteral("r-") + theta,
        theta + QStringLiteral("-r")};
    for (std::size_t index = 0; index < displayModes.size(); ++index) {
        const auto displayMode = displayModes[index];
        auto* action = new QAction(displayLabels[index], m_sphericalDisplayMenu);
        action->setCheckable(true);
        action->setActionGroup(m_sphericalDisplayGroup);
        action->setData(static_cast<int>(displayMode));
        action->setChecked(displayMode == m_sphericalDisplay);
        connect(action, &QAction::triggered, this, [this, displayMode] {
            if (displayMode == m_sphericalDisplay) {
                return;
            }
            m_sphericalDisplay = displayMode;
            saveSettings();
            updateSphericalControls();
            // Display-only change: re-render from the cached planes, no query.
            if (displayIsSpherical()) {
                scheduleSliceRequest(true);
            }
        });
        m_sphericalDisplayMenu->addAction(action);
    }
    m_sphericalMenu->addMenu(m_sphericalDisplayMenu);

    // Warp resolution: higher factors trace the curved cell boundaries more
    // smoothly at the cost of a larger warped raster.
    m_sphericalSupersampleGroup = new QActionGroup(this);
    m_sphericalSupersampleMenu = new QMenu(tr("&Supersampling"), this);
    constexpr std::array<int, 5> supersampleFactors{1, 2, 4, 8, 16};
    for (const auto factor : supersampleFactors) {
        auto* action = new QAction(
            tr("%1x").arg(factor), m_sphericalSupersampleMenu);
        action->setCheckable(true);
        action->setActionGroup(m_sphericalSupersampleGroup);
        action->setData(factor);
        action->setChecked(factor == m_sphericalSupersample);
        connect(action, &QAction::triggered, this, [this, factor] {
            if (factor == m_sphericalSupersample) {
                return;
            }
            m_sphericalSupersample = factor;
            saveSettings();
            // Display-only change: re-warp the cached planes, no new query.
            if (displayIsSpherical()) {
                scheduleSliceRequest(true);
            }
        });
        m_sphericalSupersampleMenu->addAction(action);
    }
    m_sphericalMenu->addMenu(m_sphericalSupersampleMenu);

    m_levelMenu = new QMenu(tr("&Level"), this);
    m_levelGroup = new QActionGroup(this);
    m_levelMenu->setEnabled(false);

    m_boxesAction = new QAction(tr("&Boxes"), this);
    m_boxesAction->setCheckable(true);
    m_boxesAction->setShortcuts(
        {QKeySequence(Qt::Key_B), QKeySequence(Qt::SHIFT | Qt::Key_B)});
    m_boxesAction->setEnabled(false);
    connect(m_boxesAction, &QAction::toggled, this, [this](bool visible) {
        if (visible) {
            for (auto* state : currentViews()) {
                state->gridBoxes.clear();
                // The displayed raster remains valid, but the next request
                // must fetch geometry even if the last one also had boxes.
                if (state->hasCachedRequest) {
                    state->cachedRequest.includeGridBoxes = false;
                }
            }
        }
        updateGridBoxes();
        if (visible && m_controlsReady) {
            scheduleSliceRequest(false);
        }
        saveSettings();  // overlay/boxes
    });
    m_slicePlanesAction = new QAction(tr("Sl&ice Planes"), this);
    m_slicePlanesAction->setCheckable(true);
    m_slicePlanesAction->setEnabled(false);
    connect(m_slicePlanesAction, &QAction::toggled, this,
        [this](bool visible) { m_isoWidget->setSlicePlanesVisible(visible); });

    m_contoursAction = new QAction(tr("&Contours..."), this);
    m_contoursAction->setObjectName(QStringLiteral("contoursAction"));
    m_contoursAction->setEnabled(false);
    connect(m_contoursAction, &QAction::triggered,
        this, [this] { showContoursDialog(); });

    auto* particlesAction = m_particleController->createAction(this);
    connect(particlesAction, &QAction::triggered, this,
        [this] { m_particleController->showDialog(this); });

    m_datasetAction = new QAction(tr("&Dataset..."), this);
    m_datasetAction->setEnabled(false);
    m_datasetAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    connect(m_datasetAction, &QAction::triggered,
        this, [this] { showDatasetWindow(); });

    // Legacy View menu order: Contours..., Range..., Dataset..., Number
    // Format... (the range lives in the toolbar here, not in a dialog).
    auto* numberFormatAction = new QAction(tr("&Number Format..."), this);
    connect(numberFormatAction, &QAction::triggered,
        this, [this] { showNumberFormatDialog(); });

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addMenu(scaleMenu);
    viewMenu->addMenu(m_levelMenu);
    viewMenu->addAction(m_boxesAction);
    viewMenu->addAction(m_slicePlanesAction);
    viewMenu->addMenu(paletteMenu);
    viewMenu->addSeparator();
    viewMenu->addMenu(m_sphericalMenu);
    viewMenu->addSeparator();
    viewMenu->addAction(m_contoursAction);
    viewMenu->addAction(particlesAction);
    viewMenu->addAction(m_datasetAction);
    viewMenu->addAction(numberFormatAction);
    viewMenu->addSeparator();
    // Toolbar visibility toggles.
    viewMenu->addAction(m_sliceToolbar->toggleViewAction());
    viewMenu->addAction(m_rangeToolbar->toggleViewAction());
    viewMenu->addSeparator();
    // Panel visibility toggles. Color Scale is visible by default; Dataset
    // Metadata and Diagnostics start hidden, and Animation is auto-shown for
    // 3-D datasets and plotfile sequences.
    viewMenu->addAction(m_metadataDock->toggleViewAction());
    viewMenu->addAction(m_colorBarDock->toggleViewAction());
    viewMenu->addAction(m_diagnosticsDock->toggleViewAction());
    viewMenu->addAction(m_animationDock->toggleViewAction());
    viewMenu->addAction(m_fabSelectorDock->toggleViewAction());

    // Variable menu: lists all fields with a bullet on the active one.
    m_variableMenu = menuBar()->addMenu(tr("&Variable"));
    m_variableGroup = new QActionGroup(this);

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    auto* guideAction = new QAction(tr("&User Guide..."), this);
    guideAction->setShortcut(QKeySequence::HelpContents);
    connect(guideAction, &QAction::triggered,
        this, [this] { showUserGuide(); });
    auto* referenceAction = new QAction(tr("&Keyboard && Mouse..."), this);
    connect(referenceAction, &QAction::triggered,
        this, [this] { showKeyboardMouseReference(); });
    auto* aboutAction = new QAction(tr("&About AMReXplorer..."), this);
    connect(aboutAction, &QAction::triggered, this, [this] { showAboutDialog(); });
    helpMenu->addAction(guideAction);
    helpMenu->addAction(referenceAction);
    helpMenu->addSeparator();
    helpMenu->addAction(aboutAction);
}

void MainWindow::rebuildLevelMenu()
{
    m_levelMenu->clear();
    if (!m_dataset) {
        return;
    }
    const auto& metadata = m_dataset->metadata();
    auto* finest = new QAction(tr("Finest available"), m_levelMenu);
    finest->setCheckable(true);
    finest->setActionGroup(m_levelGroup);
    finest->setData(-1);
    {
        QList<QKeySequence> finestShortcuts{QKeySequence(Qt::CTRL | Qt::Key_0)};
        if (metadata.finestLevel >= 1 && metadata.finestLevel <= 9) {
            finestShortcuts.append(QKeySequence(
                Qt::CTRL | static_cast<Qt::Key>(Qt::Key_0 + metadata.finestLevel)));
        }
        finest->setShortcuts(finestShortcuts);
    }
    connect(finest, &QAction::triggered, this, [this] {
        const auto index = m_levelSelector->findData(-1);
        if (index >= 0) {
            m_levelSelector->setCurrentIndex(index);
        }
    });
    m_levelMenu->addAction(finest);
    // "Levs 0-N" entries, descending, only when there are at least three levels.
    for (int level = metadata.finestLevel - 1; level >= 1; --level) {
        const auto comboData = kUpdateToLevelOffset + level;
        auto* action = new QAction(tr("Levs 0-%1").arg(level), m_levelMenu);
        action->setCheckable(true);
        action->setActionGroup(m_levelGroup);
        action->setData(comboData);
        if (level < 10) {
            action->setShortcut(QKeySequence(
                Qt::CTRL | static_cast<Qt::Key>(Qt::Key_0 + level)));
        }
        connect(action, &QAction::triggered, this, [this, comboData] {
            const auto index = m_levelSelector->findData(comboData);
            if (index >= 0) {
                m_levelSelector->setCurrentIndex(index);
            }
        });
        m_levelMenu->addAction(action);
    }
    // "Level N only" is redundant for a single-level dataset (mirrors
    // populateLevelCombo in MainWindowSlice.cpp, which also drops these for
    // finestLevel == 0; the menu and the combo must offer the same entries or
    // the level shortcuts find no matching combo item).
    if (metadata.finestLevel > 0) {
        for (int level = 0; level <= metadata.finestLevel; ++level) {
            auto* action = new QAction(tr("Level %1 only").arg(level), m_levelMenu);
            action->setCheckable(true);
            action->setActionGroup(m_levelGroup);
            action->setData(level);
            if (level < 10) {
                action->setShortcut(QKeySequence(
                    Qt::ALT | static_cast<Qt::Key>(Qt::Key_0 + level)));
            }
            connect(action, &QAction::triggered, this, [this, level] {
                const auto index = m_levelSelector->findData(level);
                if (index >= 0) {
                    m_levelSelector->setCurrentIndex(index);
                }
            });
            m_levelMenu->addAction(action);
        }
    }
    syncMenuChecks();
}

void MainWindow::syncMenuChecks()
{
    const auto currentData = m_levelSelector->currentData().toInt();
    const auto levelActions = m_levelMenu->actions();
    for (auto* action : levelActions) {
        action->setChecked(action->data().toInt() == currentData);
    }
}

void MainWindow::rebuildVariableMenu()
{
    m_variableMenu->clear();
    if (!m_dataset) {
        m_variableMenu->setEnabled(false);
        return;
    }
    m_variableMenu->setEnabled(true);
    const auto& metadata = m_dataset->metadata();
    const auto currentField = m_fieldSelector->currentIndex() >= 0
        ? m_fieldSelector->currentData().toUInt() : 0;
    for (std::size_t field = 0; field < metadata.fields.size(); ++field) {
        const auto name = QString::fromStdString(metadata.fields[field].name);
        auto* action = m_variableMenu->addAction(name);
        action->setCheckable(true);
        action->setActionGroup(m_variableGroup);
        action->setChecked(static_cast<std::uint32_t>(field) == currentField);
        action->setData(static_cast<unsigned int>(field));
        connect(action, &QAction::triggered, this, [this, field] {
            const auto index = m_fieldSelector->findData(
                static_cast<unsigned int>(field));
            if (index >= 0) {
                m_fieldSelector->setCurrentIndex(index);
            }
        });
    }
}

void MainWindow::syncVariableMenu()
{
    if (!m_dataset) {
        return;
    }
    const auto currentField = m_fieldSelector->currentIndex() >= 0
        ? m_fieldSelector->currentData().toUInt() : 0;
    const auto actions = m_variableMenu->actions();
    for (int i = 0; i < actions.size(); ++i) {
        actions[i]->setChecked(
            static_cast<std::uint32_t>(i) == currentField);
    }
}

void MainWindow::loadPaletteFile()
{
    const auto settings = makeSettings();
    const auto filename = QFileDialog::getOpenFileName(this,
        tr("Load Palette File"),
        settings.value(QStringLiteral("lastOpenDirectory")).toString(),
        tr("Legacy palette files (*.pal);;All files (*)"));
    if (filename.isEmpty()) {
        return;
    }
    if (const auto error = m_paletteController->loadFile(filename)) {
        QMessageBox::critical(this, tr("Cannot load palette"), *error);
        return;
    }
    auto writableSettings = makeSettings();
    writableSettings.setValue(QStringLiteral("lastOpenDirectory"),
        QFileInfo(filename).absolutePath());
}

void MainWindow::refreshPaletteDisplay()
{
    m_colorBar->setPalette(&m_paletteController->palette());
    scheduleSliceRequest();
    updateGridBoxes();
    updateOverlays();
    updateCrosshairs();
    m_isoWidget->update();
}

void MainWindow::showContoursDialog()
{
    if (!m_dataset) {
        return;
    }
    if (m_contoursDialog != nullptr) {
        m_contoursDialog->raise();
        m_contoursDialog->activateWindow();
        return;
    }
    const auto& fields = m_dataset->metadata().fields;
    std::vector<std::string> fieldNames;
    fieldNames.reserve(fields.size());
    for (const auto& field : fields) {
        fieldNames.push_back(field.name);
    }
    auto* dialog = new SetContoursDialog(fieldNames,
        m_viewDimension == 3, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMode(m_displayMode);
    dialog->setContourCount(m_contourCount);
    dialog->setVectorFields(m_vectorUField, m_vectorVField, m_vectorWField);
    dialog->setContourColor(m_contourColor);
    connect(dialog, &SetContoursDialog::applied, this, [this, dialog] {
        applyContourSettings(dialog->mode(), dialog->contourCount(),
            dialog->uField(), dialog->vField(), dialog->wField(),
            dialog->contourColor());
    });
    connect(dialog, &QDialog::finished, this, [this] {
        m_contoursDialog = nullptr;
    });
    m_contoursDialog = dialog;
    dialog->show();
}

void MainWindow::applyContourSettings(
    DisplayMode mode, int count, int uField, int vField, int wField,
    int contourColor)
{
    const auto previousMode = m_displayMode;
    const auto previousCount = m_contourCount;
    const auto previousUField = m_vectorUField;
    const auto previousVField = m_vectorVField;
    const auto previousWField = m_vectorWField;
    m_displayMode = mode;
    m_contourCount = count;
    m_vectorUField = uField;
    m_vectorVField = vField;
    m_vectorWField = wField;
    m_contourColor = contourColor;
    if (mode == DisplayMode::VelocityVectors) {
        ensureVectorFieldDefaults();
    }
    // No saveSettings here either: nothing this function sets has a settings
    // key. Contour mode, count, color, and the vector field choices are all
    // dataset-dependent, so restoring them across sessions would be wrong.
    const auto involvesVectors = mode == DisplayMode::VelocityVectors
        || previousMode == DisplayMode::VelocityVectors;
    const auto inputsChanged = mode != previousMode || count != previousCount
        || uField != previousUField || vField != previousVField
        || wField != previousWField;
    if (inputsChanged) {
        if (involvesVectors) {
            for (auto* state : currentViews()) {
                state->vectorSegments.clear();
                state->view->setOverlaySegments({});
            }
        }
        scheduleSliceRequest(false);
    } else {
        updateOverlays();
    }
}

} // namespace amrvis::qt
