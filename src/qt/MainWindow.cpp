#include "MainWindowInternal.hpp"
#include "SshRemoteSession.hpp"

#include "CloseWindowAction.hpp"
#include "CurrentRowBulletDelegate.hpp"

#include <amrexplorer/core/Version.hpp>

#include <QKeySequence>
#include <QStyle>

namespace amrvis::qt {
namespace {

// The offscreen platform the smoke tests run on has no native dialog to drive,
// and a native one would not be scriptable there either.
[[nodiscard]] QFileDialog::Options fileDialogOptions()
{
    return QApplication::platformName() == QLatin1String("offscreen")
        ? QFileDialog::Options{QFileDialog::DontUseNativeDialog}
        : QFileDialog::Options{};
}

} // namespace

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

    // The skin is application-wide, so this is built first: constructing it
    // snapshots the desktop's style and palette, which restoreSettings may
    // then replace. skinChanged only persists, so it is safe to wire here.
    m_themeController = new ThemeController(this);
    connect(m_themeController, &ThemeController::skinChanged, this,
        [this] { saveSettings(); });

    // Derived fields: the controller owns this window's definition list and
    // its editor. It asks the window for the fields a definition may read (the
    // open dataset's stored ones) and for the reload that installs a committed
    // list, since only the window knows whether a sequence is running.
    // buildFrameSpec is where the list reaches a dataset.
    m_derivedFields = new DerivedFieldController(
        DerivedFieldController::Hooks{
            .unavailableReason = [this]() -> QString {
                // Each answer names the thing the user would have to change.
                // Not for a standalone FAB, drilled out of a MultiFab or opened
                // straight from a file: applying reopens m_datasetPath, and
                // what that entry describes -- a synthesised metadata, or one
                // record of a multi-record file -- is not what re-reading the
                // path produces.
                if (!m_dataset) {
                    return tr("Derived fields need an open dataset.");
                }
                // Asked before the shape of the next load, because
                // derivedFieldsReachNextLoad() is also false for a remote
                // sequence on a peer too old to install them -- and answering
                // that with the FAB wording below tells the user their dataset
                // is something it is not, which is the lie this reason exists
                // to remove. The one case a user can act on is checked first.
                if (!m_dataset->supportsDerivedFields()) {
                    // A remote session says so in the words both layers share;
                    // anything else cannot compute fields at all.
                    return std::dynamic_pointer_cast<
                               remote::RemoteDatasetSession>(m_dataset)
                        ? tr(remote::derivedFieldsUnsupportedMessage)
                        : tr("This dataset cannot compute fields.");
                }
                // Both FABs, not just the drilled one: a standalone FAB opened
                // straight from a file reopens no better than one drilled out
                // of a MultiFab.
                if (m_dataset->metadata().isFab
                    || m_fabNavigator->fabMode()) {
                    return tr("Derived fields are not available for a FAB.");
                }
                if (!derivedFieldsReachNextLoad()) {
                    // What is left once the two above are ruled out. Kept so
                    // every state the availability hook calls unavailable has
                    // a reason to show, rather than a greyed control saying
                    // nothing.
                    return tr("Derived fields are not available for this "
                              "remote sequence.");
                }
                return {};
            },
            .reload = [this] { reloadCurrentDataset(); },
            // Which asks nothing of a session that already carries the list,
            // and remembers the ask it does make, so pressing Apply on an
            // unchanged list repeatedly starts one reload rather than one
            // each time.
            .reloadIfMissing = [this] { reloadIfDefinitionsMoved(); },
            .chooseFile =
                [this](QWidget*, bool forSaving) {
                    // Parented to the window, not to the editor that asked:
                    // the editor is WA_DeleteOnClose, and a dataset load
                    // settling inside the file dialog's nested loop can close
                    // it -- destroying the file dialog with it. loadPaletteFile
                    // parents to the window for the same reason.
                    return chooseExpressionListPath(this, forSaving);
                },
            .storedFieldNames =
                [this] {
                    QStringList names;
                    // The same gate derivedFieldRows applies, not a weaker
                    // one: where no definition can be installed at all -- a
                    // FAB, a peer that predates 1.4 -- offering the fields as
                    // material tells the user they may write against them,
                    // and the refusal then comes from somewhere else
                    // entirely. available() is that one question, asked once.
                    if (!m_dataset || !m_derivedFields->available()) {
                        return names;
                    }
                    const auto& fields = m_dataset->metadata().fields;
                    // The stored ones alone. The derived tail is what the
                    // editor is for writing, and offering it back as material
                    // would suggest a definition may read one written below
                    // it, which installation does not allow.
                    const auto stored = storedFieldCount();
                    names.reserve(static_cast<qsizetype>(stored));
                    for (std::size_t field = 0; field < stored; ++field) {
                        names.append(
                            QString::fromStdString(fields[field].name));
                    }
                    return names;
                },
            .datasetShape =
                [this] {
                    if (!m_dataset || !m_derivedFields->available()) {
                        return QString{};
                    }
                    const auto& metadata = m_dataset->metadata();
                    // The centerings as well as the geometry: two plotfiles
                    // can share every field name and still resolve an
                    // expression differently, because installation refuses one
                    // that mixes them (DerivedField.cpp, "is not centered like
                    // the other fields the expression reads").
                    QString shape = QStringLiteral("%1d%2:")
                        .arg(metadata.dimension)
                        .arg(metadata.hasPhysicalGeometry
                                ? QStringLiteral("+geo")
                                : QString{});
                    const auto stored = storedFieldCount();
                    for (std::size_t field = 0; field < stored; ++field) {
                        shape += QString::number(
                            static_cast<int>(metadata.fields[field].centering));
                    }
                    return shape;
                },
            .resolveAgainstOpenDataset =
                [this](const std::vector<DerivedFieldDefinition>& definitions) {
                    std::vector<DerivedFieldSkip> skipped;
                    // As above. Answering here would be worse than saying
                    // nothing: a remote session on an old peer reports every
                    // field as stored, so every definition resolves and the
                    // editor would report that it works -- right up until
                    // Apply refuses it for a reason of its own.
                    if (!m_dataset || !m_derivedFields->available()) {
                        return skipped;
                    }
                    // Only what installDerivedFields consults, which is
                    // exhaustively: `fields` (and of those only `.name` and
                    // `.centering`), `hasPhysicalGeometry` and `dimension`
                    // -- see src/core/DerivedField.cpp. The whole metadata
                    // used to be copied so this asked the question exactly as
                    // an open asks it, but that copies every level's box list
                    // on every pause in typing and again on every Apply.
                    //
                    // If that function ever grows a fourth dependency it has
                    // to be added here too: until it is, the editor's verdict
                    // silently stops matching what an open actually installs.
                    // That tripwire is the price of not copying the hierarchy.
                    //
                    // The derived tail is cut off because installDerivedFields
                    // appends what it resolves -- handing it a list that
                    // already holds the last installation's fields would
                    // resolve every definition against itself.
                    const auto& open = m_dataset->metadata();
                    const auto stored = storedFieldCount();
                    DatasetMetadata metadata;
                    metadata.dimension = open.dimension;
                    metadata.hasPhysicalGeometry = open.hasPhysicalGeometry;
                    metadata.fields.assign(open.fields.begin(),
                        open.fields.begin()
                            + static_cast<std::ptrdiff_t>(stored));
                    try {
                        return installDerivedFields(metadata, definitions,
                            DerivedFieldPolicy::Skip)
                            .skipped;
                    } catch (const std::exception&) {
                        // Skip resolves every definition it can and records
                        // the rest, so nothing here should throw -- but this
                        // runs while the user types, and a diagnostic is not
                        // worth taking the editor down for.
                        return skipped;
                    }
                },
        },
        DerivedFieldStore::session(), this);
    connect(m_derivedFields, &DerivedFieldController::statusMessage, this,
        [this](const QString& message, int timeoutMs) {
            statusBar()->showMessage(message, timeoutMs);
        });

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
    // The range mode, User min/max and Log, and the per-field memory behind
    // them; the separator before Log matches the per-group separators on the
    // Slice Controls toolbar, as does the one before Palette below.
    m_range = new RangeController(this);
    m_range->createToolbarWidgets(rangeToolbar);
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
                m_range->switchField(m_fieldSelector->itemText(index));
            }
            // A deferred full-domain range store (m_pendingRangeStore) is keyed
            // to the field/level/mode in effect when it was queued. Changing any
            // of them means a completing 3-D Visible sync would store its union
            // under the wrong key, so drop the pending store here (the sync
            // completion also re-checks the key; see range-cache-staleness-races).
            m_pendingRangeStore.reset();
            updateRangeModeAvailability();
            scheduleSliceRequest();
            m_volumeController->refresh();
        });
    connect(m_levelSelector, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) {
            configureSlicePositionControls();
            m_pendingRangeStore.reset();  // see the field selector above
            updateRangeModeAvailability();
            scheduleSliceRequest();
            m_volumeController->refresh();
        });
    connect(m_range, &RangeController::modeChanged, this, [this] {
        m_pendingRangeStore.reset();  // see the field selector above
        updateRangeModeAvailability();
        scheduleSliceRequest();
        m_volumeController->refresh();
    });
    connect(m_range, &RangeController::userRangeChanged, this,
        [this] {
            scheduleSliceRequest();
            m_volumeController->refresh();
        });
    connect(m_range, &RangeController::logarithmicChanged, this,
        [this] {
            scheduleSliceRequest();
            m_volumeController->refresh();
        });
    connect(m_range, &RangeController::statusMessage, this,
        [this](const QString& message, int timeoutMs) {
            statusBar()->showMessage(message, timeoutMs);
        });
    m_fieldSelector->setEnabled(false);
    m_levelSelector->setEnabled(false);

    m_metadataDock = new QDockWidget(tr("Dataset Metadata"), this);
    m_metadataTree = new QTreeWidget(m_metadataDock);
    m_metadataTree->setObjectName(QStringLiteral("metadataTree"));
    m_metadataTree->setColumnCount(2);
    m_metadataTree->setHeaderLabels({tr("Property"), tr("Value")});
    m_metadataTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_metadataTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_metadataDock->setWidget(m_metadataTree);
    addDockWidget(Qt::LeftDockWidgetArea, m_metadataDock);
    m_metadataDock->setVisible(false);

    // The remote session: the ssh-launched server, its connection, and the
    // Open Remote dialogs and browser. It says what to open; this window opens
    // it over the connection and shows the session's messages.
    m_remoteSession = new RemoteSessionController(
        RemoteSessionController::Hooks{
            [this] { return m_closing; },
            [this]() -> std::optional<std::string> {
                if (auto remoteSession = std::dynamic_pointer_cast<
                        remote::RemoteDatasetSession>(m_dataset)) {
                    return remoteSession->remotePath();
                }
                return std::nullopt;
            },
            [] { return makeSettingsPtr(); },
        },
        amrvis::versionText(), this);
    connect(m_remoteSession, &RemoteSessionController::sessionChanged, this,
        [this] { updateDiagnostics(); });
    connect(m_remoteSession, &RemoteSessionController::openRequested, this,
        [this](const std::vector<std::string>& paths, bool sequence) {
            if (sequence) {
                openRemoteSequence(paths);
            } else {
                openRemoteDataset(paths.front());
            }
        });
    connect(m_remoteSession, &RemoteSessionController::statusMessage, this,
        [this](const QString& message, int timeoutMs) {
            statusBar()->showMessage(message, timeoutMs);
        });
    connect(m_remoteSession, &RemoteSessionController::errorReported, this,
        [this](const QString& message) { reportBackgroundError(message); });

    // The Diagnostics panel's counters, metrics and histories live in their
    // model; it renders the dock, and this window supplies the lines only it
    // knows.
    m_diagnosticsModel = new DiagnosticsModel(
        DiagnosticsModel::Hooks{
            [this] { return m_generation; },
            [this] { return m_sequenceController->lastFrameSwitchMs(); },
            [this] { return m_remoteSession->diagnosticsLines(); },
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
            m_volumeController->frameSwitchStarted();
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

    // The volume view: its window, camera, opacity controls and render
    // scheduling live in the controller; this window supplies what a render
    // is built from (the dataset, the field, level, range and palette in
    // effect, the slice positions) and folds its bookkeeping into the
    // diagnostics.
    m_volumeController = new VolumeController(
        VolumeController::Hooks{
            [this] { return m_dataset; },
            [this]() -> std::optional<std::pair<FieldId, QString>> {
                if (!m_dataset || m_fieldSelector->currentIndex() < 0) {
                    return std::nullopt;
                }
                return std::pair{FieldId{m_fieldSelector->currentData().toUInt()},
                    m_fieldSelector->currentText()};
            },
            [this] {
                // The live session, as every other decodeLevelData caller
                // does: m_openMetadata is the open-time snapshot and lags it
                // for the whole of each sequence frame's install.
                return decodeLevelData(m_levelSelector->currentData().toInt(),
                    m_dataset ? m_dataset->metadata().finestLevel : 0);
            },
            [this] { return m_range->selection(); },
            [this]() -> const Palette& { return m_paletteController->palette(); },
            [this] { return m_slicePosition3d; },
            [this] { return m_slicePlanesAction->isChecked(); },
            [this] { return m_closing; },
            [this] {
                // What the three plane views actually show, as one box. Each
                // narrows only the axes it displays, so this is where they are
                // combined rather than in the controller, which has no view to
                // ask.
                //
                // Derived from the raster the view holds and the part of it on
                // screen, NOT from state.visibleRegion: that field records the
                // region a slice was last *fetched* for, and a mouse-wheel
                // zoom or a scroll moves what you can see without re-fetching
                // anything. Reading it meant the box stayed at the whole
                // domain after the most ordinary way of zooming in, so the
                // check box appeared to do nothing.
                const auto domain = m_dataset
                    ? datasetSampleBounds(m_dataset->metadata())
                    : RealBox{};
                std::array<std::optional<RealBox>, 3> regions{};
                for (std::size_t index = 0; index < m_planeViews.size();
                    ++index) {
                    const auto& state = m_planeViews[index];
                    if (state.view == nullptr || !state.view->hasImage()
                        || state.plane->width <= 0
                        || state.plane->height <= 0) {
                        continue;
                    }
                    const auto visible = state.view->visibleImageRect();
                    if (visible.isEmpty()) {
                        continue;
                    }
                    regions[index] = physicalRegionForRasterRect(
                        state.plane->physicalRegion,
                        static_cast<double>(state.plane->width),
                        static_cast<double>(state.plane->height), visible,
                        displayAxes(state.normal));
                }
                return volumeVisibleRegion(domain, regions);
            },
            [this] { return m_playbackMode == PlaybackMode::Sequence; },
        },
        this);
    connect(m_volumeController, &VolumeController::renderActivityChanged, this,
        [this](int delta) {
            m_diagnosticsModel->adjustActivity(delta);
            updateDiagnostics();
        });
    connect(m_volumeController, &VolumeController::renderFailed, this,
        [this](const QString& message) { reportBackgroundError(message); });
    connect(m_volumeController, &VolumeController::statusMessage, this,
        [this](const QString& message, int timeoutMs) {
            statusBar()->showMessage(message, timeoutMs);
        });
    connect(m_volumeController, &VolumeController::staleResultDropped, this,
        [this] { m_diagnosticsModel->noteStaleResult(); });
    connect(m_volumeController, &VolumeController::frameDisplayed, this,
        [this] { emit volumeFrameDisplayed(); });
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
            // Armed across the stop below and cleared after it. Stopping is
            // where the frames' standing aside is made good -- setPlaybackMode
            // asks for the reload once there is no next frame to read the list
            // -- and asking from here would reopen the frame that just failed,
            // against the same unreadable file or the same connection that has
            // gone, for a second report of one failure. Cleared afterwards
            // because a reload handed to the sequence and then failed installs
            // nothing, so the memo must not remember it as asked: the epoch
            // has not moved, and without the clear the next load could not ask
            // either.
            m_reloadAskedFor = {m_derivedFields->definitions(), m_sessionEpoch};
            // Stop playing. Playback wraps, so without this it comes back to
            // the same unreadable frame -- or the same disconnected server --
            // every cycle, raising a diagnostic each time. The user is left on
            // the failed frame and can step or play again once they have dealt
            // with it. A sweep is left alone; only sequence playback can hit a
            // frame load.
            if (m_playbackMode == PlaybackMode::Sequence) {
                setPlaybackMode(PlaybackMode::None);
            }
            m_reloadAskedFor.reset();
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
        [this](const ExportOptions& options, qreal scale,
               std::map<QString, ExportLayout>& layouts) {
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
                    const auto suffix = QString::fromLatin1(suffixes[idx]);
                    frames.emplace_back(
                        suffix, composeExportFrame(panelView, options, scale, &layouts[suffix]));
                }
            } else {
                frames.emplace_back(
                    QString(),
                    composeExportFrame(m_activeView != nullptr ? m_activeView->view : nullptr,
                                       options, scale, &layouts[QString()]));
            }
            return frames;
        },
        [this](int index) { goToSequenceFrame(index); }, this);
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
    connect(m_range, &RangeController::logarithmicChanged, this,
        [this] { saveSettings(); });

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
        [this] {
            refreshScaleReport();
            // A wheel zoom re-requests nothing, so the slice funnel never sees
            // it; the volume's region of interest still has to follow.
            m_volumeController->regionChanged();
        });
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
    // Scrolling or resizing moves what is on screen without changing the
    // raster. canvasScrolled cannot carry this: it fires only over a virtual
    // canvas, so a local fixed-scale scroll emitted nothing at all.
    connect(view, &ImageView::viewportMoved, this,
        [this] { m_volumeController->regionChanged(); });
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
    m_colorBar->setScale(state.displayScale);
    const auto suffix = state.displayScale.scale == ColorScale::Logarithmic
        ? tr(" (log)") : state.displayScale.scale == ColorScale::SymLogarithmic
            ? tr(" (symlog)") : QString();
    m_colorBar->setFieldRange(state.fieldName + suffix,
        state.displayMinimum, state.displayMaximum);
    m_range->showColorScale(state.displayScale);
    if (m_range->mode() != RangeMode::User) {
        m_range->showDisplayRange(state.displayMinimum, state.displayMaximum);
    }
    syncDatasetWindowColors();
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
        // Rubber-band selections intentionally retain their exact aspect (in
        // finest cells, the display's unit). Their fractional edges need not
        // have the rounded native-cell aspect used to suppress Fit
        // supersampling on a whole-domain view.
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
        [this] { m_remoteSession->promptOpen(this, false); });

    auto* openRemoteSequenceAction = new QAction(
        tr("Open &Remote Plotfile Sequence..."), this);
    connect(openRemoteSequenceAction, &QAction::triggered, this,
        [this] { m_remoteSession->promptOpen(this, true); });

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

    // This window only: the others keep running, and closing the last one is
    // what quits (Qt's quitOnLastWindowClosed).
    auto* closeWindowAction = addCloseWindowAction(*this, tr("&Close Window"));
    closeWindowAction->setObjectName(QStringLiteral("closeWindowAction"));

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
    fileMenu->addAction(closeWindowAction);
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
    m_scaleBarAction = new QAction(tr("Scale Bar"), this);
    m_scaleBarAction->setCheckable(true);
    m_scaleBarAction->setChecked(m_scaleBarVisible);
    m_scaleBarAction->setEnabled(false);
    connect(m_scaleBarAction, &QAction::toggled, this, [this](bool visible) {
        m_scaleBarVisible = visible;
        updateScaleBars();
        saveSettings();  // overlay/scaleBar
    });
    auto* lengthUnitsAction = new QAction(tr("Length &Units..."), this);
    lengthUnitsAction->setObjectName(QStringLiteral("lengthUnitsAction"));
    connect(lengthUnitsAction, &QAction::triggered,
        this, [this] { showLengthUnitsDialog(); });
    m_slicePlanesAction = new QAction(tr("Sl&ice Planes"), this);
    m_slicePlanesAction->setCheckable(true);
    m_slicePlanesAction->setEnabled(false);
    connect(m_slicePlanesAction, &QAction::toggled, this,
        [this](bool visible) {
            m_isoWidget->setSlicePlanesVisible(visible);
            m_volumeController->slicePlanesVisibilityChanged();
        });

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
    viewMenu->addAction(m_scaleBarAction);
    viewMenu->addAction(lengthUnitsAction);
    viewMenu->addAction(m_slicePlanesAction);
    viewMenu->addAction(m_volumeController->createAction(this));
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
    viewMenu->addSeparator();
    // Application-wide rather than per-view, hence its own group at the end.
    viewMenu->addMenu(m_themeController->createMenu(this));

    // Variable menu: lists all fields with a bullet on the active one.
    m_variableMenu = menuBar()->addMenu(tr("Va&riable"));
    // Menus hide action tooltips unless asked: the derived fields carry their
    // expressions there, and the Expression Editor entry carries the reason it
    // is unavailable, neither of which reaches anyone otherwise.
    m_variableMenu->setToolTipsVisible(true);
    m_variableGroup = new QActionGroup(this);
    // Owned by the window, not the menu, so rebuildVariableMenu's clear()
    // leaves it alive to be re-added. The menu itself stays enabled with no
    // dataset open so the editor's own (disabled) entry is discoverable.
    m_expressionEditorAction = m_derivedFields->createAction(this);
    m_variableMenu->addAction(m_expressionEditorAction);
    m_variableMenu->setEnabled(true);

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

void MainWindow::rebuildVariableMenu(const std::vector<DerivedFieldRow>& rows)
{
    m_variableMenu->clear();
    // The menu stays enabled with nothing open: it then holds the Expression
    // Editor entry alone, greyed out with the reason on its tooltip, which is
    // more discoverable than a menu that cannot be opened at all.
    m_variableMenu->setEnabled(true);
    if (!m_dataset) {
        m_variableMenu->addAction(m_expressionEditorAction);
        m_derivedFields->refreshAvailability();
        return;
    }
    const auto& metadata = m_dataset->metadata();
    const auto currentField = m_fieldSelector->currentIndex() >= 0
        ? m_fieldSelector->currentData().toUInt() : 0;
    const auto stored = storedFieldCount();
    const auto addField = [this, currentField](
                              const QString& name, std::size_t field) {
        auto* action = m_variableMenu->addAction(name);
        // Returned rather than looked up again: QMenu::actions() copies the
        // whole list, and "the one just added is last" stops being true the
        // moment addField grows a separator or a submenu.
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
        return action;
    };
    for (std::size_t field = 0; field < stored; ++field) {
        // A tooltip of its own, because the menu shows them for the derived
        // rows and QAction falls back to the action's own text: without this
        // every plotfile field pops a tooltip repeating its name.
        addField(QString::fromStdString(metadata.fields[field].name), field)
            ->setToolTip(tr("Stored in the plotfile"));
    }

    // The same rows the field selector was given, dimmed the same way.
    if (!rows.empty()) {
        m_variableMenu->addSeparator();
    }
    for (const auto& row : rows) {
        auto* action = row.field
            ? addField(row.name, static_cast<std::size_t>(*row.field))
            : m_variableMenu->addAction(row.name);
        action->setEnabled(row.field.has_value());
        action->setToolTip(row.tooltip);
    }

    // Re-added after every rebuild: clear() above only *removes* it, because
    // the action belongs to the window rather than to the menu.
    m_variableMenu->addSeparator();
    m_variableMenu->addAction(m_expressionEditorAction);
    m_derivedFields->refreshAvailability();
}

void MainWindow::syncVariableMenu()
{
    if (!m_dataset) {
        return;
    }
    const auto currentField = m_fieldSelector->currentIndex() >= 0
        ? m_fieldSelector->currentData().toUInt() : 0;
    // Only the field entries, which are the ones in the group: the separator
    // and the Expression Editor action follow them.
    // By the id each action carries, not by its position: the two agree only
    // while every field in the list is in the group, and a definition the
    // open session could not install is listed without being one of them.
    for (auto* action : m_variableGroup->actions()) {
        action->setChecked(action->data().toUInt() == currentField);
    }
}

QString MainWindow::rememberedDialogDirectory() const
{
    return makeSettings()
        .value(QStringLiteral("lastOpenDirectory"))
        .toString();
}

void MainWindow::rememberDialogDirectory(const QString& path)
{
    if (path.isEmpty()) {
        return;
    }
    auto settings = makeSettings();
    settings.setValue(QStringLiteral("lastOpenDirectory"),
        QFileInfo(path).absolutePath());
}

void MainWindow::loadPaletteFile()
{
    const auto filename = QFileDialog::getOpenFileName(this,
        tr("Load Palette File"), rememberedDialogDirectory(),
        tr("Legacy palette files (*.pal);;All files (*)"), nullptr,
        fileDialogOptions());
    if (filename.isEmpty()) {
        return;
    }
    if (const auto error = m_paletteController->loadFile(filename)) {
        QMessageBox::critical(this, tr("Cannot load palette"), *error);
        return;
    }
    rememberDialogDirectory(filename);
}

QString MainWindow::chooseExpressionListPath(QWidget* parent, bool forSaving)
{
    const auto directory = rememberedDialogDirectory();
    const auto filter = tr("Expression lists (*.json);;All files (*)");
    const auto options = fileDialogOptions();
    QString path;
    if (forSaving) {
        // Built rather than taken from getSaveFileName so the default suffix
        // is applied *before* the overwrite confirmation: appending ".json"
        // afterwards means the dialog asks about "fields" while the write
        // lands on an existing "fields.json" it never mentioned.
        QFileDialog dialog(parent, tr("Export Derived Fields"),
            directory.isEmpty() ? QStringLiteral("expressions.json")
                                : directory + QStringLiteral("/expressions.json"),
            filter);
        dialog.setOptions(options);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix(QStringLiteral("json"));
        if (dialog.exec() == QDialog::Accepted
            && !dialog.selectedFiles().isEmpty()) {
            path = dialog.selectedFiles().front();
        }
    } else {
        path = QFileDialog::getOpenFileName(parent,
            tr("Import Derived Fields"), directory, filter, nullptr, options);
    }
    rememberDialogDirectory(path);
    return path;
}

bool MainWindow::reloadCurrentDataset()
{
    // Not while closing: another window's Apply reaches every window, and a
    // worker started here would hold the I/O mutex against the quit. The
    // completion handler checks m_closing, but the read still runs.
    if (m_closing || !m_dataset || m_datasetPath.empty() || !m_openMetadata) {
        return false;
    }
    if (m_playbackMode == PlaybackMode::Sequence) {
        // Mid-playback: reloading here would restart the frame under the user,
        // and every frame load reads the list for itself (buildFrameSpec), so
        // the next frame picks the change up on its own -- but only if it is
        // actually loaded. The frame after this one may already be prefetched,
        // rendered against the list as it was, and goToFrame publishes such a
        // frame instead of loading it; dropping it is what makes "the next
        // frame reads the list" true. A plane sweep is not this case at all --
        // it moves the slice position on the session already open and never
        // reopens it -- so skipping the reload there would leave the
        // definition uninstalled for good, with the editor reporting that it
        // had been applied.
        m_sequenceController->invalidatePrefetch();
        // Nothing was reloaded, which the caller has to know: a reload only
        // asked for is not one that happened, and treating the two alike is
        // how the definition would stay uninstalled for good.
        return false;
    }
    if (m_sequenceController->hasSequence()) {
        // A prefetched frame was rendered against the previous field list.
        m_sequenceController->invalidatePrefetch();
        m_sequenceController->goToFrame(
            m_sequenceController->currentIndex(), true);
        return true;
    }
    // Not openDataset: that ends the sequence, drops the zoom and closes the
    // line-plot, dataset and volume windows. This is the same reload a
    // sequence frame switch performs, on the frame already shown.
    // The Dataset window and the line plot are snapshots of the session this
    // is about to replace: the frame switch closes them for that reason
    // (frameSwitchStarted) and a reload is the same replacement. Left open
    // they would show the old field list with no sign of being stale, and
    // pin the outgoing session and its block cache besides.
    closeDatasetWindow();
    auto* linePlotWindow = m_linePlotWindow;
    m_linePlotWindow = nullptr;
    if (linePlotWindow != nullptr) {
        linePlotWindow->close();
    }
    // No m_initialStopSource reset here: requestInitialSlice stops and
    // replaces it on the way in, and the token the worker is given comes from
    // that one.
    const auto generation = ++m_generation;
    for (auto* state : allViewStates()) {
        state->stopSource.request_stop();
        ++state->sliceGeneration;
    }
    m_sliceDebounce->stop();
    // As openDatasetImpl and the frame-switch handler do: stopping the timer
    // cancels the flush but leaves what it had coalesced, and that would be
    // replayed against the new session by the next unrelated request.
    m_pendingAllViews = false;
    m_pendingViews.clear();
    // A remote dataset is reopened on *its own* connection, not on whatever
    // the session controller currently holds: server dataset ids come from a
    // counter per connection, so a reload over a newer connection would
    // restart them at 1 and let a cached display range alias a renumbered
    // field. A connection that has gone fails the reload cleanly instead,
    // which is the honest outcome.
    if (const auto remote
        = std::dynamic_pointer_cast<remote::RemoteDatasetSession>(m_dataset)) {
        requestInitialSlice(m_datasetPath, generation, std::nullopt, {},
            buildFrameSpec(),
            SliceLoad{{}, RemoteOpen{remote->connection(),
                           remote->remotePath()}});
        return true;
    }
    // No prepared metadata and no data root: with none, the session derives
    // both from the path, which is what re-reading a plotfile means. A FAB
    // drilled out of a MultiFab cannot be reopened this way -- its metadata is
    // synthesised by the navigator, not read from the path -- which is why the
    // editor is unavailable while one is on screen (see the controller's
    // availability hook).
    requestInitialSlice(
        m_datasetPath, generation, std::nullopt, {}, buildFrameSpec());
    return true;
}

void MainWindow::refreshMetadataDisplay()
{
    if (!m_dataset) {
        return;
    }
    PlotfileMetadataResult displayed;
    displayed.metadata =
        std::make_shared<const DatasetMetadata>(m_dataset->metadata());
    displayed.metrics = m_dataset->metadataReadMetrics();
    displayed.fileVersion = m_dataset->fileVersion();
    showMetadata(displayed, m_datasetPath);
}

void MainWindow::refreshPaletteDisplay()
{
    m_colorBar->setPalette(&m_paletteController->palette());
    syncDatasetWindowColors();
    scheduleSliceRequest();
    updateGridBoxes();
    updateOverlays();
    updateCrosshairs();
    m_isoWidget->update();
    m_volumeController->refresh();
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
