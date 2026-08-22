#include "MainWindowInternal.hpp"

namespace amrvis::qt {

void MainWindow::focusActiveViewForPanning()
{
    if (m_activeView == nullptr || m_activeView->view == nullptr) {
        return;
    }
    // Both callers run from a watcher's completion, which on a slow open lands
    // well after the file dialog closed and the user moved on to a toolbar
    // control. Move focus only when it is on nothing in particular or on
    // another image view; a control outranks a convenience.
    //
    // What this preserves is that focus stays *out of the view*, not that it
    // stays exactly where the user put it: the teardown disables the field,
    // level and range widgets, and Qt moves focus off a disabled focus widget
    // to a tab-chain neighbour before this runs. That neighbour is still a
    // control, so the guard declines either way -- which is the outcome that
    // matters, because the alternative is their next arrow key panning the
    // image.
    auto* const focused = QApplication::focusWidget();
    const QWidget* probe = focused;
    while (probe != nullptr && qobject_cast<const ImageView*>(probe) == nullptr) {
        probe = probe->parentWidget();
    }
    if (focused != nullptr && focused != this && probe == nullptr) {
        return;
    }
    m_activeView->view->setFocus(::Qt::OtherFocusReason);
}

void MainWindow::showNumberFormatDialog()
{
    if (m_numberFormatDialog != nullptr) {
        m_numberFormatDialog->raise();
        m_numberFormatDialog->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Number Format"));
    dialog->setWindowFlags(Qt::Window);

    auto* edit = new QLineEdit(m_numberFormat, dialog);
    edit->setMinimumWidth(160);
    auto* syntaxLabel = new QLabel(
        tr("C printf format, e.g. %1").arg(defaultNumberFormat()), dialog);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok
        | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, dialog);
    auto* defaultButton = buttons->addButton(
        tr("Default"), QDialogButtonBox::ResetRole);
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(syntaxLabel);
    layout->addWidget(edit);
    layout->addWidget(buttons);

    connect(defaultButton, &QPushButton::clicked, dialog, [this, edit] {
        edit->setText(defaultNumberFormat());
        applyNumberFormat(defaultNumberFormat());
    });
    connect(buttons, &QDialogButtonBox::clicked, dialog,
        [this, dialog, edit, buttons](QAbstractButton* button) {
            const auto role = buttons->buttonRole(button);
            if (role == QDialogButtonBox::AcceptRole
                || role == QDialogButtonBox::ApplyRole) {
                const auto format = edit->text();
                if (!isValidNumberFormat(format)) {
                    QMessageBox::warning(dialog, tr("Invalid number format"),
                        tr("\"%1\" is not a usable number format.\n"
                           "Use a printf-style format with exactly one floating "
                           "conversion, e.g. %2.")
                            .arg(format, defaultNumberFormat()));
                    return;
                }
                applyNumberFormat(format);
                if (role == QDialogButtonBox::AcceptRole) {
                    dialog->accept();
                }
            } else if (role == QDialogButtonBox::RejectRole) {
                dialog->reject();
            }
        });
    connect(dialog, &QDialog::finished, this, [this] {
        m_numberFormatDialog = nullptr;
    });
    m_numberFormatDialog = dialog;
    dialog->show();
}

void MainWindow::applyNumberFormat(const QString& format)
{
    if (!isValidNumberFormat(format) || m_numberFormat == format) {
        return;
    }
    m_numberFormat = format;
    m_range->setNumberFormat(format);
    m_colorBar->setNumberFormat(format);
    // Open child windows repaint against the stored format; a null pointer
    // means the window picks the format up when it is next created.
    if (m_datasetWindow != nullptr) {
        m_datasetWindow->setNumberFormat(format);
    }
    if (m_linePlotWindow != nullptr) {
        m_linePlotWindow->setNumberFormat(format);
    }
    saveSettings();
}

void MainWindow::validateVectorMode()
{
    if (m_displayMode != DisplayMode::VelocityVectors) {
        return;
    }
    const auto fieldCount = m_openMetadata ? m_openMetadata->fields.size() : 0;
    if (fieldCount < 2) {
        statusBar()->showMessage(
            tr("Velocity Vectors requires at least two fields"));
        m_displayMode = DisplayMode::Raster;
        return;
    }
    ensureVectorFieldDefaults();
}

void MainWindow::ensureVectorFieldDefaults()
{
    if (!m_openMetadata) {
        return;
    }
    const auto& fields = m_openMetadata->fields;
    const auto count = static_cast<int>(fields.size());
    if (m_vectorUField >= 0 && m_vectorUField < count
        && m_vectorVField >= 0 && m_vectorVField < count
        && m_vectorWField >= 0 && m_vectorWField < count) {
        return;
    }
    std::vector<std::string> fieldNames;
    fieldNames.reserve(fields.size());
    for (const auto& field : fields) {
        fieldNames.push_back(field.name);
    }
    auto [uField, vField, wField] = detectVectorFields(fieldNames);
    if (uField == vField && count > 1) {
        vField = (uField == 0) ? 1 : 0;
    }
    m_vectorUField = uField;
    m_vectorVField = vField;
    m_vectorWField = wField;
}

QLineF MainWindow::planeSegmentToScene(const PlaneViewState& state,
    float x0, float y0, float x1, float y1) const
{
    // Plane row 0 is the bottom row; the displayed image is mirrored
    // vertically, so scene y runs opposite to plane y (see showSlice).
    const auto top = static_cast<double>(state.plane->height) - 1.0;
    return QLineF(QPointF(x0, top - y0), QPointF(x1, top - y1));
}

QColor MainWindow::overlayColor() const
{
    if (m_contourColor == contourColorWhite) {
        return QColor(255, 255, 255);
    }
    if (m_contourColor >= 0 && m_contourColor < Palette::slotCount) {
        return QColor::fromRgba(static_cast<QRgb>(
            m_paletteController->palette().slotArgb(m_contourColor)));
    }
    return QColor(0, 0, 0);
}

QColor MainWindow::sliceAxisColor(int axis) const
{
    // Legacy Amrvis draws each slice plane's guides in a fixed palette slot:
    // x -> slot 65, y -> slot 220, z -> slot 255.
    constexpr std::array<int, 3> paletteSlots{65, 220, 255};
    return QColor::fromRgba(static_cast<QRgb>(
        m_paletteController->palette().slotArgb(paletteSlots[static_cast<std::size_t>(axis)])));
}

void MainWindow::updateOverlay(PlaneViewState& state)
{
    std::vector<OverlaySegment> overlays;
    std::vector<OverlayPath> paths;
    const auto planeReady = state.plane->width > 1 && state.plane->height > 1;
    if (!planeReady || m_displayMode == DisplayMode::Raster) {
        state.view->setOverlaySegments(overlays);
        state.view->setOverlayPaths(paths);
        return;
    }

    if (m_displayMode == DisplayMode::VelocityVectors) {
        // Segment coordinates depend on the layout the arrival was generated
        // for (state, not the in-flight menu selection): R-Z glyphs carry
        // display physical (R, Z) endpoints already rotated into physical
        // directions (see generateSphericalRZVectorGlyphs); the logical
        // r-theta / theta-r layouts carry plane pixels mapped through the
        // plane mapping (identity or transposed); Cartesian keeps the plain
        // pixel-to-scene flip.
        overlays.reserve(state.vectorSegments.size());
        const auto vectorColor = overlayColor();
        const bool spherical = displayIsSpherical();
        const bool sphericalRZ = spherical
            && state.sphericalDisplay == SphericalDisplay::RZ;
        const auto mapping = planeMapping(state);
        for (const auto& segment : state.vectorSegments) {
            const auto line = sphericalRZ
                ? QLineF(mapping.sceneFromDisplay(segment.x0, segment.y0),
                    mapping.sceneFromDisplay(segment.x1, segment.y1))
                : spherical
                ? QLineF(mapping.sceneFromPlanePixel(segment.x0, segment.y0),
                    mapping.sceneFromPlanePixel(segment.x1, segment.y1))
                : planeSegmentToScene(state,
                    segment.x0, segment.y0, segment.x1, segment.y1);
            overlays.push_back({line, vectorColor, 1.0F});
        }
        state.view->setOverlaySegments(overlays);
        state.view->setOverlayPaths(paths);
        return;
    }

    if (!(state.displayMinimum < state.displayMaximum)) {
        state.view->setOverlaySegments(overlays);
        state.view->setOverlayPaths(paths);
        return;
    }
    try {
        // The polylines were extracted from the refined data-resolution
        // contour plane on the slice worker and are already in display-plane
        // pixel space (see appendContours); this thread only converts them
        // to painter paths. Plane row 0 is the bottom row; the displayed
        // image is mirrored vertically, so scene y runs opposite to plane y
        // (see showSlice).
        const auto contourColor = overlayColor();
        const bool spherical = displayIsSpherical();
        const auto mapping = planeMapping(state);
        const auto top = static_cast<double>(state.plane->height) - 1.0;
        // Cartesian: plane pixel maps 1:1 to the scene (only the vertical flip).
        // Spherical: re-project each (r, theta) plane pixel through the warp.
        const auto toScene = [&](const auto& point) -> QPointF {
            if (spherical) {
                return mapping.sceneFromPlanePixel(point[0], point[1]);
            }
            return QPointF(point[0], top - point[1]);
        };
        std::map<double, QPainterPath> pathsByValue;
        for (const auto& polyline : state.contourPolylines) {
            if (polyline.points.empty()) {
                continue;
            }
            auto& path = pathsByValue[polyline.value];
            path.moveTo(toScene(polyline.points.front()));
            for (std::size_t i = 1; i < polyline.points.size(); ++i) {
                path.lineTo(toScene(polyline.points[i]));
            }
            if (polyline.closed) {
                path.closeSubpath();
            }
        }
        paths.reserve(pathsByValue.size());
        for (auto& [value, path] : pathsByValue) {
            const auto color = contourColor;
            paths.push_back({std::move(path), color, 1.0F});
        }
    } catch (const std::exception&) {
        paths.clear();
    }
    state.view->setOverlaySegments(overlays);
    state.view->setOverlayPaths(paths);
}

void MainWindow::updateOverlays()
{
    for (auto* state : currentViews()) {
        updateOverlay(*state);
    }
}

void MainWindow::updateParticleOverlay(PlaneViewState& state)
{
    std::vector<PointOverlay> overlays;
    if (!m_dataset || !state.view->hasImage()
        || state.plane->width <= 0 || state.plane->height <= 0
        // The warped R-Z view has no linear plane-pixel mapping for points.
        || displayIsSphericalWarp()) {
        state.view->setPointOverlays(overlays);
        return;
    }
    const bool spherical = displayIsSpherical();
    const auto mapping = planeMapping(state);
    const auto planeHeight = static_cast<double>(state.plane->height);
    const auto& samples = m_particleController->samples();
    overlays.reserve(samples.size());
    for (const auto& sample : samples) {
        PointOverlay overlay;
        overlay.color = m_particleController->colorFor(sample.species.name);
        overlay.size
            = static_cast<float>(m_particleController->settings().pointSize);
        const auto projected = projectParticlePoints(
            sample.points, *state.plane,
            m_dataset->metadata().dimension, state.normal);
        overlay.points.reserve(projected.size());
        for (const auto& point : projected) {
            if (spherical) {
                // projectParticlePoints returns r-theta scene coords (y flipped
                // from height to 0). Recover the plane pixel and re-map through
                // the active layout (identity for r-theta, transposed otherwise).
                const auto scene = mapping.sceneFromPlanePixel(
                    point.x, planeHeight - point.y);
                overlay.points.emplace_back(scene.x(), scene.y());
            } else {
                overlay.points.emplace_back(point.x, point.y);
            }
        }
        overlays.push_back(std::move(overlay));
    }
    state.view->setPointOverlays(overlays);
}

void MainWindow::updateParticleOverlays()
{
    for (auto* state : currentViews()) {
        updateParticleOverlay(*state);
    }
}

void MainWindow::showKeyboardMouseReference()
{
    QString rows;
    const auto add = [&rows](const QString& action, const QString& description) {
        rows += QStringLiteral(
            "<tr><td style='padding-right:14px;vertical-align:top;'><b>%1</b></td>"
            "<td>%2</td></tr>").arg(action, description);
    };
    add(tr("Left click"), tr("Probe the value under the cursor"));
    add(tr("Left drag"),
        tr("Zoom to the rubber-band subregion; Scale controls panel sync"));
    add(tr("Shift+left drag"), tr("Pan the view"));
    add(tr("Arrow keys"),
        tr("Pan the focused panel (5% of the view per step)"));
    add(tr("Shift+middle click"), tr("Line plot along the horizontal axis"));
    add(tr("Shift+right click"), tr("Line plot along the vertical axis"));
    add(tr("Right drag"), tr("Line plot (drag direction picks orientation)"));
    add(tr("Right click (3-D)"),
        tr("Move both slice planes to intersect at the clicked point"));
    add(tr("Wheel / double click"),
        tr("Zoom this panel in or out / reset the zoom"));
    add(tr("B"), tr("Toggle AMR grid boxes"));
    add(tr("0"), tr("Reset the zoom to the whole domain"));
    add(tr("1-6"), tr("Fixed zoom scales (1x-32x)"));
    add(tr("Ctrl+0"), tr("Composite the finest available level"));
    add(tr("Ctrl+1-9"), tr("Composite levels 0 through N (Levs 0-N)"));
    add(tr("Alt+0-9"), tr("Show one exact AMR level"));
    add(tr("Ctrl+D"), tr("Open the Dataset window (raw cell values per level)"));

    QMessageBox box(this);
    box.setWindowTitle(tr("Keyboard & Mouse"));
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral("<table>%1</table>").arg(rows));
    box.setInformativeText(
        tr("View \xE2\x86\x92 Number Format... sets the readout format; "
           "the View menu shows or hides the panels."));
    box.setIcon(QMessageBox::NoIcon);
    box.exec();
}

void MainWindow::showUserGuide()
{
    if (m_userGuideDialog == nullptr) {
        m_userGuideDialog = new UserGuideDialog(this);
    }
    m_userGuideDialog->show();
    m_userGuideDialog->raise();
    m_userGuideDialog->activateWindow();
}

void MainWindow::showAboutDialog()
{
    QMessageBox::about(this, tr("About AMReXplorer"),
        tr("<h3>AMReXplorer</h3>"
           "<p>Demand-driven AMR visualization.</p>"
           "<p>Version %1</p>"
           "<p>A C++20 / Qt 6 application for inspecting AMReX plotfiles.</p>")
            .arg(QString::fromLatin1(kVersion)));
}

double MainWindow::effectiveFixedScale(int factor) const
{
    // A view on the whole-domain virtual canvas fetches its visible window at
    // finest resolution, so its factor is always literal. That is the test,
    // not "is this remote": a remote *spherical* view refuses the canvas (see
    // applyFixedScale) and scales a clamped raster like any local one, so
    // asking about the session rather than the view reported no clamp exactly
    // where one was in force.
    //
    // The gate is the *warp*, not spherical-ness. Only the R-Z display warps
    // the raster into a physical wedge; r-theta draws the logical grid as-is
    // and theta-r transposes it (see SlicePipeline's sphericalDisplay switch),
    // so in both of those one raster pixel still stands for cells/pixels finest
    // cells and the clamp still applies. Excluding every spherical view left
    // the two unwarped modes stating a factor they were not applying -- the
    // exact over-claim this report exists to prevent. The transpose needs no
    // special case: the worst axis is a min over both, which a swap does not
    // change.
    if (!m_openMetadata || m_openMetadata->levels.empty() || factor <= 0
        || m_activeView == nullptr || m_activeView->view == nullptr
        || m_activeView->view->virtualCanvasActive()
        || displayIsSphericalWarp()) {
        return 0.0;
    }
    const auto& metadata = *m_openMetadata;
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::max(0, metadata.finestLevel))];
    // The region the raster actually covers, which is what nativeOutputSize
    // clamps -- not the whole domain. After a rubber-band zoom the raster
    // covers the selection, which may well fit under the clamp while the
    // domain does not, and announcing a clamp then contradicts the guide.
    const auto domain = m_activeView->visibleRegion.value_or(
        datasetSampleBounds(metadata));
    // Ask the pipeline what raster it will actually build rather than
    // re-deriving its clamp here: nativeOutputSize is the same
    // finestNativeOutputSize the worker calls, so if the clamp moves or stops
    // being a plain per-axis limit, this report follows instead of drifting.
    // slicePlaneAxes is what finestNativeOutputSize itself uses, keyed on the
    // same metadata -- not displayAxes, which keys on m_dataset and so
    // disagrees while a dataset is opened but not yet published, pairing
    // output[k] with the wrong axis.
    const auto output = nativeOutputSize(*m_activeView);
    const auto axes = slicePlaneAxes(metadata.dimension, m_activeView->normal);
    auto worst = static_cast<double>(factor);
    bool clamped = false;
    for (std::size_t k = 0; k < axes.size(); ++k) {
        const auto i = static_cast<std::size_t>(axes[k]);
        const auto cellSize = finest.cellSize[i];
        if (!(cellSize > 0.0)) {
            continue;
        }
        const auto cells = std::round(
            (domain.upper[i] - domain.lower[i]) / cellSize);
        const auto pixels = static_cast<double>(output[k]);
        if (!(cells > pixels)) {
            continue;
        }
        clamped = true;
        // One raster pixel now spans cells/pixels finest cells, and the view
        // scales raster pixels by the factor.
        worst = std::min(worst, factor * pixels / cells);
    }
    return clamped ? worst : 0.0;
}

QString MainWindow::plainScaleLabel(int factor) const
{
    // The radio items' text, and what every scale lookup matches against. It
    // had been spelled out at six sites; the one that drifted was the test
    // helper, which kept this spelling for the *button* after the button
    // started decorating it.
    return tr("%1x").arg(factor);
}

QString MainWindow::fixedScaleLabel(int factor, double effective) const
{
    // One definition of the button's fixed-scale text. It used to be spelled
    // out at each site, and fixedScaleStateMatchesForTest kept the plain
    // tr("%1x") spelling -- which a clamped label can never equal, so the
    // helper could only ever fail on exactly the datasets the report exists
    // for.
    return effective > 0.0
        ? tr("%1x→%2x").arg(factor).arg(effective, 0, 'g', 3)
        : plainScaleLabel(factor);
}

QString MainWindow::defaultScaleToolTip() const
{
    return tr("Zoom scale and rubber-band synchronization for panels");
}

void MainWindow::refreshScaleReport()
{
    // Derive the state from the view rather than replaying the last one set.
    // Replaying re-asserted a state the view had already left: ImageView
    // demotes Custom to Fit when the coordinator returns Refit, so rubber-band
    // a sequence frame, step to a differently shaped one, and the button
    // claimed "Custom" over a view that had just refitted. Reading the mode
    // back cannot disagree with it, and it makes the report correct across
    // every path that changes a transform, including ones that report nothing
    // themselves.
    if (m_activeView == nullptr || m_activeView->view == nullptr) {
        return;
    }
    const auto* view = m_activeView->view;
    const auto mode = view->transformMode();
    const auto factor = view->fixedScaleFactor();
    // Panels can disagree -- an unsynchronized rubber-band zoom leaves one in
    // Custom and the others alone -- and Mixed is what says so. Deriving from
    // the active view alone would quietly report that one panel's state as if
    // it held for all three.
    for (const auto* state : currentViews()) {
        if (state->view->transformMode() != mode
            || (mode == ImageView::TransformMode::FixedScale
                && state->view->fixedScaleFactor() != factor)) {
            setScaleUiState(ScaleUiState::Mixed);
            return;
        }
    }
    switch (mode) {
    case ImageView::TransformMode::Fit:
        setScaleUiState(ScaleUiState::Fit);
        return;
    case ImageView::TransformMode::FixedScale:
        // The clamped label also depends on the dataset, which is why this
        // runs on a new frame: a factor picked on one domain can come to
        // something else on the next.
        setScaleUiState(ScaleUiState::Fixed, factor);
        return;
    case ImageView::TransformMode::Custom:
        setScaleUiState(ScaleUiState::Custom);
        return;
    }
}

void MainWindow::setScaleUiState(ScaleUiState state, int factor)
{
    QString label;
    // Computed once: it decides both the label and the tool tip below.
    double effective = 0.0;
    switch (state) {
    case ScaleUiState::Fit:
        label = tr("Fit");
        break;
    case ScaleUiState::Fixed:
        // Say what the factor really came to when the raster clamp reduced it,
        // so "32x" never silently means two different magnifications.
        effective = effectiveFixedScale(factor);
        label = fixedScaleLabel(factor, effective);
        break;
    case ScaleUiState::Custom:
        label = tr("Custom");
        break;
    case ScaleUiState::Mixed:
        label = tr("Mixed");
        break;
    }
    if (m_scaleButton != nullptr) {
        m_scaleButton->setText(label);
        if (state == ScaleUiState::Fixed && effective > 0.0) {
            m_scaleButton->setToolTip(
                tr("This view spans more than %1 finest cells, which is the "
                   "largest raster AMReXplorer builds for it, so one raster "
                   "pixel covers more than one cell: %2x is applied as %3x. "
                   "Zoom into a subregion to inspect it at %2x.")
                    .arg(maxSliceOutputDimension)
                    .arg(factor)
                    .arg(effective, 0, 'g', 3));
        } else {
            m_scaleButton->setToolTip(defaultScaleToolTip());
        }
    }
    if (m_scaleGroup == nullptr) {
        return;
    }
    // Custom and Mixed have no radio item; leave the group with nothing checked.
    if (state == ScaleUiState::Custom || state == ScaleUiState::Mixed) {
        if (auto* checked = m_scaleGroup->checkedAction()) {
            checked->setChecked(false);
        }
        return;
    }
    if (state == ScaleUiState::Fit) {
        if (m_resetZoomAction != nullptr) {
            m_resetZoomAction->setChecked(true);
        }
        return;
    }
    // Match on the plain factor, never on `label`: a clamped label reads
    // "32x->16x", which matches no radio item, and the menu would keep the
    // previous check -- the toolbar/menu split this setter exists to remove.
    const auto wanted = plainScaleLabel(factor);
    // setChecked, never trigger: the group's handlers call back into
    // applyFixedScale, and this is reporting the scale, not choosing one.
    for (auto* action : m_scaleGroup->actions()) {
        auto text = action->text();
        text.remove(QLatin1Char('&'));
        if (text == wanted) {
            action->setChecked(true);
            return;
        }
    }
}

void MainWindow::resetViewZoom(PlaneViewState& state)
{
    state.visibleRegion.reset();
    state.view->setVirtualCanvas(std::nullopt);
    state.view->fitToWindow();
    scheduleSliceRequest(state);
}

void MainWindow::resetZoomAllViews()
{
    for (auto* state : currentViews()) {
        resetViewZoom(*state);
    }
    // Reported here, not per view: once per user action, and *unconditionally*.
    // Leaving it to resetViewZoom meant a Reset Zoom with no dataset open --
    // reachable by its shortcut -- iterated nothing and reported nothing, so
    // the button kept a factor no view was applying.
    setScaleUiState(ScaleUiState::Fit);
}

QString MainWindow::probeReadout(
    const PlaneViewState& state, int x, int displayY) const
{
    const auto& plane = *state.plane;
    if (!m_dataset || plane.width <= 0 || plane.height <= 0) {
        return tr("no data");
    }
    const auto& metadata = m_dataset->metadata();
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto& region = plane.physicalRegion;

    // Map the probed pixmap pixel to a logical (x, y)/(r, theta) position and
    // the source plane offset holding its value, validity, and level. Cartesian
    // pixmap pixels index the plane directly; spherical pixmap pixels are in
    // warped (R, Z) space, so invert the warp first and re-derive the pixel.
    std::array<double, 3> position{0.0, 0.0, 0.0};
    std::size_t offset = 0;
    if (displayIsSpherical()) {
        const auto mapping = planeMapping(state);
        const auto logical = mapping.logicalFromScene(
            static_cast<double>(x) + 0.5, static_cast<double>(displayY) + 0.5);
        if (logical[0] < region.lower[xAxis] || logical[0] > region.upper[xAxis]
            || logical[1] < region.lower[yAxis]
            || logical[1] > region.upper[yAxis]) {
            return tr("no data");  // cursor is outside the annular sector
        }
        position[xAxis] = logical[0];
        position[yAxis] = logical[1];
        const auto spanX = region.upper[xAxis] - region.lower[xAxis];
        const auto spanY = region.upper[yAxis] - region.lower[yAxis];
        const int col = std::clamp(static_cast<int>(
            (logical[0] - region.lower[xAxis]) / spanX * plane.width),
            0, plane.width - 1);
        const int row = std::clamp(static_cast<int>(
            (logical[1] - region.lower[yAxis]) / spanY * plane.height),
            0, plane.height - 1);
        offset = static_cast<std::size_t>(col)
            + static_cast<std::size_t>(plane.width)
                * static_cast<std::size_t>(row);
    } else {
        const auto y = plane.height - 1 - displayY;
        offset = static_cast<std::size_t>(x)
            + static_cast<std::size_t>(plane.width)
                * static_cast<std::size_t>(y);
        position[xAxis] = region.lower[xAxis]
            + (static_cast<double>(x) + 0.5) / static_cast<double>(plane.width)
                * (region.upper[xAxis] - region.lower[xAxis]);
        position[yAxis] = region.lower[yAxis]
            + (static_cast<double>(y) + 0.5) / static_cast<double>(plane.height)
                * (region.upper[yAxis] - region.lower[yAxis]);
    }
    if (offset >= plane.values.size() || plane.valid[offset] == 0) {
        return tr("no data");
    }
    if (metadata.dimension == 3) {
        position[static_cast<std::size_t>(state.normal)]
            = m_slicePosition3d[static_cast<std::size_t>(state.normal)];
    }
    const auto level = std::clamp(
        static_cast<int>(plane.sourceLevel[offset]), 0, metadata.finestLevel);
    const auto& levelMetadata = metadata.levels[static_cast<std::size_t>(level)];

    // Integer index of the cell/face/edge/node. Nodes sit on integer
    // positions so they round; everything else floors into its cell.
    const auto centering = (state.hasCachedRequest
            && state.cachedRequest.field.value < metadata.fields.size())
        ? metadata.fields[state.cachedRequest.field.value].centering
        : amrvis::Centering::Cell;
    std::array<int, 3> cell{0, 0, 0};
    for (int axis = 0; axis < metadata.dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        cell[i] = sampleIndex(levelMetadata, axis, position[i]);
    }

    // The AMR box (grid) at this level that contains the cell.
    int boxIndex = -1;
    for (int box = 0; box < static_cast<int>(levelMetadata.boxes.size()); ++box) {
        const auto& candidate = levelMetadata.boxes[static_cast<std::size_t>(box)];
        bool contains = true;
        for (int axis = 0; axis < metadata.dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            if (cell[i] < candidate.lower[i] || cell[i] > candidate.upper[i]) {
                contains = false;
                break;
            }
        }
        if (contains) {
            boxIndex = box;
            break;
        }
    }

    auto join = [&](const auto& triple) {
        QString text;
        for (int axis = 0; axis < metadata.dimension; ++axis) {
            if (axis != 0) {
                text += ',';
            }
            text += QString::number(triple[static_cast<std::size_t>(axis)]);
        }
        return text;
    };

    constexpr std::array<const char*, 3> axisNames{"x", "y", "z"};
    const char* indexKind = "cell";
    if (centering == amrvis::Centering::Node) {
        indexKind = "node";
    } else if (centering == amrvis::Centering::FaceX
        || centering == amrvis::Centering::FaceY
        || centering == amrvis::Centering::FaceZ) {
        indexKind = "face";
    } else if (centering == amrvis::Centering::EdgeX
        || centering == amrvis::Centering::EdgeY
        || centering == amrvis::Centering::EdgeZ) {
        indexKind = "edge";
    }

    QString boxText;
    if (boxIndex >= 0) {
        const auto& box = levelMetadata.boxes[static_cast<std::size_t>(boxIndex)];
        // Axis-major: ((xlo,xhi),(ylo,yhi),...,(index-type per axis)). The
        // trailing list is the box's AMReX IndexType (0 = cell, 1 = node).
        QString bounds;
        for (int axis = 0; axis < metadata.dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            if (axis != 0) {
                bounds += ',';
            }
            bounds += QStringLiteral("(%1,%2)").arg(box.lower[i]).arg(box.upper[i]);
        }
        QString indexType;
        for (int axis = 0; axis < metadata.dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            if (axis != 0) {
                indexType += ',';
            }
            indexType += QString::number(box.centering[i]);
        }
        boxText = tr("box #%1 (%2,(%3))").arg(boxIndex).arg(bounds, indexType);
    } else {
        boxText = tr("box=none");
    }

    // Standalone FABs and MultiFabs have no AMR hierarchy, so their readout
    // omits the level.
    const auto levelText = metadata.hasPhysicalGeometry
        ? tr(" level=%1").arg(level)
        : QString();
    const auto valueText = formatNumber(
        static_cast<double>(plane.values[offset]), m_numberFormat);
    if (displayIsSpherical()) {
        // position[xAxis] is r, position[yAxis] is theta (from logicalFromScene).
        const QString theta(QChar(0x03B8));
        const auto rText = formatNumber(position[xAxis], m_numberFormat);
        const auto thetaText = formatNumber(position[yAxis], m_numberFormat);
        QString coords;
        // The state's mode, not the menu selection: the readout labels must
        // match the mapping that produced the coordinates above.
        switch (state.sphericalDisplay) {
        case SphericalDisplay::RZ: {
            // Physical (R, Z) plus the native spherical (r, theta).
            const auto display = sphericalToDisplay(
                position[xAxis], position[yAxis]);
            coords = QStringLiteral("R=%1 Z=%2 r=%3 %4=%5").arg(
                formatNumber(display[0], m_numberFormat),
                formatNumber(display[1], m_numberFormat), rText, theta, thetaText);
            break;
        }
        case SphericalDisplay::ThetaR:
            coords = QStringLiteral("%1=%2 r=%3").arg(theta, thetaText, rText);
            break;
        case SphericalDisplay::RTheta:
        default:
            coords = QStringLiteral("r=%1 %2=%3").arg(rText, theta, thetaText);
            break;
        }
        return tr("%1 value=%2%3 %4=(%5) %6").arg(coords, valueText, levelText,
            QString::fromLatin1(indexKind), join(cell), boxText);
    }
    return tr("%1=%2 %3=%4 value=%5%6 %7=(%8) %9")
        .arg(QString::fromLatin1(axisNames[xAxis]))
        .arg(formatNumber(position[xAxis], m_numberFormat))
        .arg(QString::fromLatin1(axisNames[yAxis]))
        .arg(formatNumber(position[yAxis], m_numberFormat))
        .arg(valueText)
        .arg(levelText)
        .arg(QString::fromLatin1(indexKind))
        .arg(join(cell))
        .arg(boxText);
}

void MainWindow::probeMoved(PlaneViewState& state, int x, int displayY)
{
    m_probeLabel->setText(probeReadout(state, x, displayY));
}

void MainWindow::probeClicked(PlaneViewState& state, int x, int displayY)
{
    setActiveView(state);
    const auto line = probeReadout(state, x, displayY);
    m_probeLabel->setText(line);
    m_diagnosticsModel->appendProbeLine(line);
    updateDiagnostics();
}

void MainWindow::rubberBandZoom(PlaneViewState& state, const QRectF& sceneRect)
{
    setActiveView(state);
    const auto& plane = *state.plane;
    if (!m_dataset || plane.width <= 0 || plane.height <= 0) {
        return;
    }
    if (displayIsSpherical()) {
        // The scene is warped (R, Z); re-slicing a logical (r, theta) subregion
        // from it is deferred. Zoom the view only, leaving the full-domain
        // warped raster in place.
        const QRectF bounds(0.0, 0.0,
            static_cast<double>(state.view->image().width()),
            static_cast<double>(state.view->image().height()));
        const auto selection = sceneRect.normalized().intersected(bounds);
        if (selection.width() < 1.0 || selection.height() < 1.0) {
            return;
        }
        state.view->zoomToRect(selection);
        setScaleUiState(ScaleUiState::Custom);
        return;
    }
    const auto clamped = sceneRect.normalized().intersected(
        QRectF(0.0, 0.0, static_cast<double>(plane.width),
            static_cast<double>(plane.height)));
    if (clamped.width() < 1.0 || clamped.height() < 1.0) {
        return;
    }
    const QRectF normalizedRect(
        clamped.left() / static_cast<double>(plane.width),
        clamped.top() / static_cast<double>(plane.height),
        clamped.width() / static_cast<double>(plane.width),
        clamped.height() / static_cast<double>(plane.height));
    const auto views = currentViews();
    const bool synchronize = m_syncRubberBandZoomAction != nullptr
        && m_syncRubberBandZoomAction->isChecked()
        && views.size() > 1;
    if (synchronize) {
        for (auto* target : views) {
            applyRubberBandZoom(*target, normalizedRect);
        }
    } else {
        applyRubberBandZoom(state, normalizedRect);
    }
    setScaleUiState(views.size() > 1 && !synchronize
            ? ScaleUiState::Mixed
            : ScaleUiState::Custom);
}

void MainWindow::applyRubberBandZoom(
    PlaneViewState& state, const QRectF& normalizedRect)
{
    const auto& plane = *state.plane;
    if (!m_dataset || plane.width <= 0 || plane.height <= 0) {
        return;
    }
    const auto normalized = normalizedRect.normalized().intersected(
        QRectF(0.0, 0.0, 1.0, 1.0));
    if (normalized.isEmpty()) {
        return;
    }
    const auto width = static_cast<double>(plane.width);
    const auto height = static_cast<double>(plane.height);
    const QRectF clamped(
        normalized.left() * width, normalized.top() * height,
        normalized.width() * width, normalized.height() * height);
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto& region = plane.physicalRegion;
    const auto xExtent = region.upper[xAxis] - region.lower[xAxis];
    const auto yExtent = region.upper[yAxis] - region.lower[yAxis];
    auto visible = region;
    visible.lower[xAxis] = region.lower[xAxis] + clamped.left() / width * xExtent;
    visible.upper[xAxis] = region.lower[xAxis] + clamped.right() / width * xExtent;
    visible.lower[yAxis] = region.lower[yAxis]
        + (height - clamped.bottom()) / height * yExtent;
    visible.upper[yAxis] = region.lower[yAxis]
        + (height - clamped.top()) / height * yExtent;
    // Local slices use one output pixel per finest cell, so their edges land
    // on cell boundaries. Remote slices are viewport-resampled; retaining the
    // exact selection keeps an arbitrary rubber-band aspect ratio intact.
    if (!std::dynamic_pointer_cast<remote::RemoteDatasetSession>(m_dataset)) {
        const auto& metadata = m_dataset->metadata();
        const auto& finest = metadata.levels[static_cast<std::size_t>(
            std::max(0, metadata.finestLevel))];
        visible = snapToCellBoundaries(
            visible, datasetSampleBounds(metadata), finest.cellSize, axes);
    }
    state.visibleRegion = visible;
    // Rubber-band zoom leaves the virtual canvas: as with local data, the
    // selection is re-rendered as a standalone raster fitted to the pane,
    // with no domain-spanning scroll bars.
    state.view->setVirtualCanvas(std::nullopt);
    // Zoom to the requested region mapped back to scene pixels, so the view
    // transform matches the region the requested slice will actually cover.
    // Confined: the selection becomes a standalone raster with no
    // domain-spanning scroll bars, so the feedback zoom must not raise them
    // either — transient scroll bars shrink the viewport, and the remote
    // request would be sized to the stolen pixels and re-fetched (and
    // re-framed, visibly) once they vanish.
    const QRectF requestedScene(
        QPointF((visible.lower[xAxis] - region.lower[xAxis]) / xExtent * width,
            (region.upper[yAxis] - visible.upper[yAxis]) / yExtent * height),
        QPointF((visible.upper[xAxis] - region.lower[xAxis]) / xExtent * width,
            (region.upper[yAxis] - visible.lower[yAxis]) / yExtent * height));
    state.view->zoomToRect(requestedScene.normalized(), true);
    scheduleSliceRequest(state);
}

void MainWindow::beginPanDrag(PlaneViewState& state)
{
    setActiveView(state);
    m_panView = &state;
    m_panSceneDelta = QPointF();
    m_panLastScheduledDelta = QPointF();
    // A virtual canvas pans by scrolling (which fetches on its own); the
    // region-shifting refresh is for classic rasters of a zoomed subregion.
    m_panDataRefresh = state.visibleRegion.has_value()
        && !state.view->virtualCanvasActive();
    if (m_panDataRefresh) {
        m_panStartRegion = *state.visibleRegion;
        m_panPlaneWidth = state.plane->width;
        m_panPlaneHeight = state.plane->height;
    }
}

void MainWindow::updatePanDrag(PlaneViewState& state,
    const QPointF& totalSceneDelta, const QPoint& viewportDelta)
{
    if (m_panView != &state) {
        return;
    }
    m_panSceneDelta = totalSceneDelta;
    constexpr int minimumDrag = 4;
    if (std::max(std::abs(totalSceneDelta.x()),
            std::abs(totalSceneDelta.y())) < minimumDrag) {
        return;
    }
    if (m_panDataRefresh) {
        if (!m_panDebounce->isActive()) {
            flushPanDrag(false);
            m_panDebounce->start();
        }
    } else {
        state.view->panViewport(viewportDelta);
    }
}

void MainWindow::endPanDrag(PlaneViewState& state, const QPointF& totalSceneDelta)
{
    m_panDebounce->stop();
    if (m_panView != &state) {
        return;
    }
    m_panSceneDelta = totalSceneDelta;
    constexpr int minimumDrag = 4;
    if (std::max(std::abs(totalSceneDelta.x()),
            std::abs(totalSceneDelta.y())) >= minimumDrag
        && m_panDataRefresh) {
        flushPanDrag(true);
    }
    m_panView = nullptr;
    m_panDataRefresh = false;
}

void MainWindow::flushPanDrag(bool finalize)
{
    if (!m_panView || !m_panDataRefresh || !m_dataset) {
        return;
    }
    if (!finalize && m_panSceneDelta == m_panLastScheduledDelta) {
        return;
    }
    const auto region = shiftedPanRegion(*m_panView, m_panStartRegion,
        m_panPlaneWidth, m_panPlaneHeight, m_panSceneDelta);
    if (!region.has_value()) {
        return;
    }
    m_panView->visibleRegion = *region;
    m_panLastScheduledDelta = m_panSceneDelta;
    scheduleSliceRequest(*m_panView, false);
}

std::array<double, 2> MainWindow::viewCenterInData(
    const PlaneViewState& state) const
{
    const auto& plane = *state.plane;
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto& region = plane.physicalRegion;
    if (plane.width <= 0 || plane.height <= 0 || state.view == nullptr
        || state.view->viewport() == nullptr) {
        const auto centre = region.center();
        return {centre[xAxis], centre[yAxis]};
    }
    // The centre of the mapped viewport, not the mapping of the viewport's
    // centre *point*: QRect::center() is integral and rounds down, so on a
    // 640-pixel-wide viewport it answers 319 where the geometric centre is 320.
    // That half-to-one pixel is nothing at native resolution and a lot when one
    // raster pixel spans many finest cells -- on an 8192-cell domain fitted to
    // 640 pixels it is about 13 cells, which is what made a remote fixed-scale
    // switch land off the centre a local one keeps. This is also the reading
    // preservedDataWindow and updateRemoteFixedScaleDemand already use, so all
    // three now agree on where the view is looking.
    const auto scene = state.view->mapToScene(
        state.view->viewport()->rect()).boundingRect().center();
    if (state.view->virtualCanvasActive() && m_dataset
        && !m_dataset->metadata().levels.empty()) {
        // Virtual canvas: scene units are finest cells over the whole domain,
        // counted from the domain's physical top-left.
        const auto& metadata = m_dataset->metadata();
        const auto domain = datasetSampleBounds(metadata);
        const auto& finest = metadata.levels[static_cast<std::size_t>(
            std::max(0, metadata.finestLevel))];
        const auto sceneRect = state.view->sceneRect();
        const auto cellX = std::clamp(scene.x(), 0.0, sceneRect.width());
        const auto cellY = std::clamp(scene.y(), 0.0, sceneRect.height());
        return {
            domain.lower[xAxis] + cellX * finest.cellSize[xAxis],
            domain.upper[yAxis] - cellY * finest.cellSize[yAxis]};
    }
    const auto sceneX = std::clamp(
        scene.x(), 0.0, static_cast<double>(plane.width));
    const auto sceneY = std::clamp(
        scene.y(), 0.0, static_cast<double>(plane.height));
    return {
        region.lower[xAxis] + sceneX / plane.width
            * (region.upper[xAxis] - region.lower[xAxis]),
        region.upper[yAxis] - sceneY / plane.height
            * (region.upper[yAxis] - region.lower[yAxis])};
}

bool MainWindow::remoteDemandCanvas(const PlaneViewState& state) const
{
    return m_dataset != nullptr
        && std::dynamic_pointer_cast<remote::RemoteDatasetSession>(m_dataset)
            != nullptr
        && !displayIsSpherical() && state.view != nullptr
        && state.view->virtualCanvasActive();
}

std::optional<ImageView::VirtualPlacement> MainWindow::virtualPlacementFor(
    const PlaneViewState& state, const RealBox& region) const
{
    if (!m_dataset || m_dataset->metadata().levels.empty()) {
        return std::nullopt;
    }
    const auto& metadata = m_dataset->metadata();
    const auto domain = datasetSampleBounds(metadata);
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::max(0, metadata.finestLevel))];
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto dx = finest.cellSize[xAxis];
    const auto dy = finest.cellSize[yAxis];
    if (!(dx > 0.0) || !(dy > 0.0)) {
        return std::nullopt;
    }
    // Scene y grows downward: the region's upper physical y is its top row.
    const QRectF itemCells(
        (region.lower[xAxis] - domain.lower[xAxis]) / dx,
        (domain.upper[yAxis] - region.upper[yAxis]) / dy,
        (region.upper[xAxis] - region.lower[xAxis]) / dx,
        (region.upper[yAxis] - region.lower[yAxis]) / dy);
    const QSizeF domainCells(
        (domain.upper[xAxis] - domain.lower[xAxis]) / dx,
        (domain.upper[yAxis] - domain.lower[yAxis]) / dy);
    if (itemCells.isEmpty() || domainCells.isEmpty()) {
        return std::nullopt;
    }
    return ImageView::VirtualPlacement{itemCells, domainCells};
}

void MainWindow::centerViewOnData(
    PlaneViewState& state, const std::array<double, 2>& dataCenter)
{
    if (!m_dataset || m_dataset->metadata().levels.empty()
        || !state.view->virtualCanvasActive()) {
        return;
    }
    const auto& metadata = m_dataset->metadata();
    const auto domain = datasetSampleBounds(metadata);
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::max(0, metadata.finestLevel))];
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto dx = finest.cellSize[xAxis];
    const auto dy = finest.cellSize[yAxis];
    if (!(dx > 0.0) || !(dy > 0.0)) {
        return;
    }
    state.view->centerOn(
        (dataCenter[0] - domain.lower[xAxis]) / dx,
        (domain.upper[yAxis] - dataCenter[1]) / dy);
}

void MainWindow::applyFixedScale(int factor)
{
    const auto views = currentViews();
    std::vector<std::array<double, 2>> centers;
    centers.reserve(views.size());
    for (const auto* state : views) {
        centers.push_back(viewCenterInData(*state));
    }
    const bool demandDriven = std::dynamic_pointer_cast<
            remote::RemoteDatasetSession>(m_dataset) != nullptr
        && !displayIsSpherical();
    for (std::size_t index = 0; index < views.size(); ++index) {
        auto& state = *views[index];
        if (demandDriven) {
            // Host the raster on a whole-domain virtual canvas so the scroll
            // bars span the domain exactly as they do for a local fixed
            // scale; the canvas scroll state then decides which cells to
            // fetch (see updateRemoteFixedScaleDemand).
            state.view->setVirtualCanvas(virtualPlacementFor(
                state, state.plane->physicalRegion));
        } else {
            state.view->setVirtualCanvas(std::nullopt);
        }
        state.view->setFixedScale(factor);
        if (remoteDemandCanvas(state)) {
            centerViewOnData(state, centers[index]);
            updateRemoteFixedScaleDemand(state);
        }
    }
}

void MainWindow::updateRemoteFixedScaleDemand(PlaneViewState& state)
{
    if (!remoteDemandCanvas(state) || state.view->viewport() == nullptr) {
        return;
    }
    const auto& metadata = m_dataset->metadata();
    if (metadata.levels.empty()) {
        return;
    }
    const auto domain = datasetSampleBounds(metadata);
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::max(0, metadata.finestLevel))];
    const auto axes = displayAxes(state.normal);
    // What the canvas currently shows, in finest cells; fetch that window.
    // The raster cannot change the scroll bars (the scene spans the domain
    // regardless of what is loaded), so this can never feed back on itself.
    const auto visible = state.view->mapToScene(
        state.view->viewport()->rect()).boundingRect();
    const auto sceneRect = state.view->sceneRect();
    if (visible.isEmpty() || sceneRect.isEmpty()) {
        return;
    }
    const std::array<double, 2> visibleLower{visible.left(), visible.top()};
    const std::array<double, 2> visibleSpan{visible.width(), visible.height()};
    const std::array<double, 2> domainSpanCells{
        sceneRect.width(), sceneRect.height()};
    auto target = domain;
    for (std::size_t entry = 0; entry < axes.size(); ++entry) {
        const auto axis = static_cast<std::size_t>(axes[entry]);
        const auto dx = finest.cellSize[axis];
        const auto domainCells = std::max(1.0,
            std::round(domainSpanCells[entry]));
        // A constant one-cell slack keeps the fetched size independent of
        // the scroll phase, so a pan replaces the raster with an equal-size
        // one and the display transform is provably unaffected.
        const auto cells = std::min(domainCells,
            std::ceil(std::max(1.0, visibleSpan[entry])) + 1.0);
        if (cells >= domainCells) {
            continue;
        }
        const auto first = std::clamp(std::floor(visibleLower[entry]),
            0.0, domainCells - cells);
        if (entry == 0) {
            target.lower[axis] = domain.lower[axis] + first * dx;
            target.upper[axis] = target.lower[axis] + cells * dx;
        } else {
            // Scene y counts cells down from the domain's physical top.
            target.upper[axis] = domain.upper[axis] - first * dx;
            target.lower[axis] = target.upper[axis] - cells * dx;
        }
    }
    if (target == domain) {
        state.visibleRegion.reset();
    } else {
        state.visibleRegion = target;
    }
    if (!state.hasCachedRequest
        || state.cachedRequest.visibleRegion != target
        || state.cachedRequest.outputSize != sliceOutputSize(state)) {
        scheduleSliceRequest(state, true);
    }
}

void MainWindow::applyPanStep(PlaneViewState& state, const QPointF& direction)
{
    if (!state.view->hasImage() || state.plane->width <= 0 || state.plane->height <= 0) {
        return;
    }
    setActiveView(state);
    const auto stepX = std::max(1.0, static_cast<double>(state.plane->width) * 0.05);
    const auto stepY = std::max(1.0, static_cast<double>(state.plane->height) * 0.05);
    const QPointF sceneDelta(direction.x() * stepX, direction.y() * stepY);

    if (state.visibleRegion.has_value() && m_dataset
        && !state.view->virtualCanvasActive()) {
        const auto region = shiftedPanRegion(state, *state.visibleRegion,
            state.plane->width, state.plane->height, sceneDelta);
        if (!region.has_value()) {
            return;
        }
        state.visibleRegion = *region;
        const bool remoteFixed = std::dynamic_pointer_cast<
            remote::RemoteDatasetSession>(m_dataset) != nullptr
            && state.view->transformMode()
                == ImageView::TransformMode::FixedScale;
        if (!remoteFixed) {
            state.view->fitToWindow();
            // One panel refitted; the others kept what they had, so the report
            // is derived rather than asserted (see the fitRequested handler).
            refreshScaleReport();
        }
        scheduleSliceRequest(state, false);
        return;
    }

    // shiftedPanRegion above and panViewport here share one convention: the
    // delta says how far the *content* moves. Handing both the same unnegated
    // sceneDelta is what keeps an arrow key moving the view rather than the
    // image once the pan falls through to the scroll bars. The axes convert
    // independently because a fixed scale over a raster whose aspect differs
    // from the logical size yields an anisotropic transform.
    const auto transform = state.view->transform();
    state.view->panViewport(QPoint(
        static_cast<int>(std::round(sceneDelta.x() * transform.m11())),
        static_cast<int>(std::round(sceneDelta.y() * transform.m22()))));
}

std::optional<RealBox> MainWindow::shiftedPanRegion(
    const PlaneViewState& state, const RealBox& baseRegion,
    int planeWidth, int planeHeight, const QPointF& sceneDelta) const
{
    if (!m_dataset || planeWidth <= 0 || planeHeight <= 0) {
        return std::nullopt;
    }
    auto visible = baseRegion;
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto domain = datasetSampleBounds(m_dataset->metadata());
    const auto width = static_cast<double>(planeWidth);
    const auto height = static_cast<double>(planeHeight);
    const auto xExtent = visible.upper[xAxis] - visible.lower[xAxis];
    const auto yExtent = visible.upper[yAxis] - visible.lower[yAxis];
    auto deltaX = -sceneDelta.x() / width * xExtent;
    auto deltaY = sceneDelta.y() / height * yExtent;

    if (visible.lower[xAxis] + deltaX < domain.lower[xAxis]) {
        deltaX = domain.lower[xAxis] - visible.lower[xAxis];
    }
    if (visible.upper[xAxis] + deltaX > domain.upper[xAxis]) {
        deltaX = domain.upper[xAxis] - visible.upper[xAxis];
    }
    if (visible.lower[yAxis] + deltaY < domain.lower[yAxis]) {
        deltaY = domain.lower[yAxis] - visible.lower[yAxis];
    }
    if (visible.upper[yAxis] + deltaY > domain.upper[yAxis]) {
        deltaY = domain.upper[yAxis] - visible.upper[yAxis];
    }
    if (deltaX == 0.0 && deltaY == 0.0) {
        return std::nullopt;
    }

    visible.lower[xAxis] += deltaX;
    visible.upper[xAxis] += deltaX;
    visible.lower[yAxis] += deltaY;
    visible.upper[yAxis] += deltaY;
    // Snap the translated region back onto the finest-level cell grid,
    // preserving its span. Fractional edges let the slice sampler's pixel
    // centers land on cell boundaries whenever the phase approaches half a
    // cell (arrow-key steps of 0.05*N cells hit exactly x.5 within a few
    // presses), and the floor in physicalToIndex then rounds either way —
    // the duplicated/skipped rows and columns this prevents.
    const auto& metadata = m_dataset->metadata();
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::max(0, metadata.finestLevel))];
    const auto snapped = snapToNearestCellGrid(
        visible, domain, finest.cellSize, axes);
    if (snapped == baseRegion) {
        return std::nullopt;
    }
    return snapped;
}

void MainWindow::linePlotRequested(PlaneViewState& state, int imageX, int imageY,
    Qt::MouseButton button)
{
    setActiveView(state);
    const auto& plane = *state.plane;
    if (!m_controlsReady || !m_dataset || plane.width <= 0 || plane.height <= 0) {
        // The drag that got here already painted a guide; a request that never
        // starts still has to take it down.
        state.view->clearLineGuide();
        return;
    }
    const auto dataset = m_dataset;
    const auto& metadata = dataset->metadata();
    const auto horizontal = button == Qt::MiddleButton;
    const auto level = m_levelSelector->currentData().toInt();
    const auto [composition, maximumLevel] = decodeLevelData(
        level, metadata.finestLevel);
    const auto field = m_fieldSelector->currentData().toUInt();
    const auto slicePosition = metadata.dimension == 3
        ? m_slicePosition3d[static_cast<std::size_t>(state.normal)] : 0.0;
    LineRequest request;
    if (displayIsSpherical()) {
        // Logical r-theta / theta-r layout: the click is in the possibly
        // transposed pixmap, so makeLineRequest's "r is horizontal" assumption
        // does not hold. Derive the varied axis and the fixed coordinate through
        // the mode-aware mapping instead. A horizontal drag varies whichever
        // logical axis is horizontal (r for r-theta, theta for theta-r).
        const auto mapping = planeMapping(state);
        const auto logical = mapping.logicalFromScene(
            static_cast<double>(imageX) + 0.5, static_cast<double>(imageY) + 0.5);
        // The state's mode, matching the mapping the click was interpreted
        // through (the raster on screen, not a pending menu selection).
        const int horizontalAxis =
            state.sphericalDisplay == SphericalDisplay::ThetaR ? 1 : 0;
        const int variedAxis = horizontal ? horizontalAxis : 1 - horizontalAxis;
        const int fixedAxis = 1 - variedAxis;
        request.dataset = dataset->id();
        request.field = FieldId{field};
        request.maximumLevel = maximumLevel;
        request.composition = composition;
        request.axis = variedAxis;
        request.fixedCoordinates[static_cast<std::size_t>(fixedAxis)] =
            logical[static_cast<std::size_t>(fixedAxis)];
        request.region = plane.physicalRegion;
    } else {
        request = makeLineRequest(plane.physicalRegion,
            plane.width, plane.height, imageX, imageY, horizontal,
            metadata.dimension, state.normal, slicePosition,
            dataset->id(), FieldId{field}, maximumLevel, composition);
    }
    const auto outputWidth = horizontal ? state.view->image().width()
                                        : state.view->image().height();
    const auto fieldName = metadata.fields[field].name;
    const auto dimension = metadata.dimension;
    // The other in-plane axis carries the cursor's fixed coordinate.
    const auto axes = displayAxes(state.normal);
    const auto primaryFixedAxis = request.axis == axes[0] ? axes[1] : axes[0];
    const auto generation = m_generation;
    // Renew the line-plot stop source only if a dataset switch or window close
    // stopped it, so concurrent line requests can still complete and stack
    // their curves in the shared window.
    if (m_linePlotStopSource.stop_requested()) {
        m_linePlotStopSource = StopSource{};
    }
    const auto cancellation = m_linePlotStopSource.get_token();
    m_diagnosticsModel->adjustActivity(1);
    statusBar()->showMessage(tr("Loading line plot for %1...").arg(
        QString::fromStdString(fieldName)));
    updateDiagnostics();

    auto* watcher = new QFutureWatcher<LineQueryResult>(this);
    auto* view = state.view;
    connect(watcher, &QFutureWatcher<LineQueryResult>::finished, this,
        [this, watcher, dataset, generation, cancellation, request, fieldName,
            dimension, primaryFixedAxis, maximumLevel, composition, view] {
            m_diagnosticsModel->adjustActivity(-1);
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            // The drag guide belongs to the request, not to its outcome: a
            // failed, cancelled, or superseded query used to leave it painted
            // until the next setImage replaced the whole scene.
            view->clearLineGuide();
            try {
                auto result = watcher->future().takeResult();
                if (generation != m_generation || cancellation.stop_requested()) {
                    m_diagnosticsModel->noteStaleResult();
                } else {
                    appendLinePlotCurve(result.line, fieldName, dimension,
                        primaryFixedAxis, request.axis,
                        request.fixedCoordinates,
                        maximumLevel, composition);
                    const auto cache = dataset->cacheMetrics();
                    m_diagnosticsModel->setCacheMetrics(cache);
                    m_diagnosticsModel->setSliceMetrics(result.metrics.blocksRead,
                        result.metrics.cacheHits, result.metrics.payloadBytesRead);
                    statusBar()->showMessage(tr("Added line plot curve for %1")
                        .arg(QString::fromStdString(fieldName)));
                }
            } catch (const CacheBudgetExceeded&) {
                // A line plot cannot shed resolution the way a slice can, so
                // translate the raw pinned-budget error into actionable advice
                // instead of degrading (see
                // cache-budget-exceeded-hard-fails-after-load).
                if (generation == m_generation && !cancellation.stop_requested()) {
                    reportBackgroundError(tr(
                        "The line plot cannot fit in the %1 cache. Choose a "
                        "lower level or increase AMREXPLORER_CACHE_SIZE_MB.")
                        .arg(cacheBudgetText(
                            dataset->cacheMetrics().budgetBytes)));
                } else {
                    m_diagnosticsModel->noteStaleResult();
                }
            } catch (const std::exception& error) {
                if (generation == m_generation && !cancellation.stop_requested()) {
                    reportBackgroundError(
                        tr("Cannot load line plot: %1").arg(exceptionMessage(error)));
                } else {
                    m_diagnosticsModel->noteStaleResult();
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [dataset, request, outputWidth, cancellation] {
            auto result = dataset->requestView(
                ViewDataRequest{LineViewRequest{request, outputWidth}},
                cancellation);
            return std::get<LineQueryResult>(std::move(result));
        }));
}

void MainWindow::sliceMoveRequested(PlaneViewState& state, int imageX, int imageY,
    Qt::MouseButton /*button*/)
{
    setActiveView(state);
    if (!m_dataset || m_dataset->metadata().dimension != 3
        || state.plane->width <= 0 || state.plane->height <= 0) {
        return;
    }
    // Move both in-plane axes so the three slices intersect at the clicked
    // point. A single right-click replaces the old middle=x / right=y split,
    // which was inaccessible on Mac (no middle button).
    const auto axes = displayAxes(state.normal);
    const auto& region = state.plane->physicalRegion;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto axis = axes[i];
        const auto fraction = (i == 0)
            ? (static_cast<double>(imageX) + 0.5)
                / static_cast<double>(state.plane->width)
            : (static_cast<double>(state.plane->height - 1 - imageY) + 0.5)
                / static_cast<double>(state.plane->height);
        const auto index = static_cast<std::size_t>(axis);
        setSlicePosition(axis, region.lower[index]
            + fraction * (region.upper[index] - region.lower[index]));
    }
}

void MainWindow::appendLinePlotCurve(const LineResult& line,
    const std::string& fieldName, int dimension, int primaryFixedAxis,
    int lineAxis, const std::array<double, 3>& fixedCoordinates,
    int maximumLevel, CompositionPolicy composition)
{
    if (m_linePlotWindow == nullptr) {
        auto name = QString::fromStdString(m_datasetPath.filename().string());
        if (name.isEmpty()) {
            name = QString::fromStdString(m_datasetPath.string());
        }
        auto* window = new LinePlotWindow(name);
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->setNumberFormat(m_numberFormat);
        connect(window, &QObject::destroyed, this, [this, window] {
            if (m_linePlotWindow == window) {
                m_linePlotWindow = nullptr;
            }
            // Stop in-flight line queries so a late result cannot reopen the
            // window the user just closed.
            m_linePlotStopSource.request_stop();
        });
        m_linePlotWindow = window;
    }
    LinePlotCurve curve;
    curve.line = line;
    curve.fieldName = fieldName;
    curve.primaryFixedAxis = primaryFixedAxis;
    curve.lineAxis = lineAxis;
    curve.fixedCoordinates = fixedCoordinates;
    curve.dimension = dimension;
    curve.maximumLevel = maximumLevel;
    curve.composition = composition;
    if (displayIsSpherical()) {
        // Logical axes 0 and 1 are always r and theta, whichever screen layout
        // is active.
        curve.axisNames = {QStringLiteral("r"), QString(QChar(0x03B8)), QString()};
    }
    m_linePlotWindow->addCurve(std::move(curve));
    m_linePlotWindow->show();
    m_linePlotWindow->raise();
    m_linePlotWindow->activateWindow();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Mark this window closing so asynchronous completion handlers that fire
    // during or after shutdown do not pop modal dialogs or reopen windows.
    m_closing = true;
    // Stop resubmit timers and request cancellation on every async task this
    // window owns; running tasks re-check their stop token and bail promptly,
    // so a task mid-read cannot leave the process lingering at quit. This does
    // NOT clear the shared global pool -- that would strand other windows'
    // queued work; the pool clear happens only on aboutToQuit (see
    // cancelInFlight).
    cancelInFlight();
    m_remoteSession->shutdown();
    // Secondary top-level windows are parentless or non-modal; close them with
    // the main window so none lingers and keeps the process alive.
    if (m_linePlotWindow != nullptr) {
        auto* linePlotWindow = m_linePlotWindow;
        m_linePlotWindow = nullptr;
        linePlotWindow->close();
    }
    closeDatasetWindow();
    m_volumeController->reset();
    if (m_contoursDialog != nullptr) {
        auto* dialog = m_contoursDialog;
        m_contoursDialog = nullptr;
        dialog->close();
    }
    m_particleController->closeDialog();
    if (m_numberFormatDialog != nullptr) {
        auto* dialog = m_numberFormatDialog;
        m_numberFormatDialog = nullptr;
        dialog->close();
    }
    if (m_userGuideDialog != nullptr) {
        auto* dialog = m_userGuideDialog;
        m_userGuideDialog = nullptr;
        dialog->close();
    }
    // Dismiss any export progress dialog and signal the encoder workers to
    // terminate their FFmpeg processes (see AnimationExporter).
    m_animationExporter->cancelForShutdown();
    saveSettings();
    auto settings = makeSettings();
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    QMainWindow::closeEvent(event);
}

} // namespace amrvis::qt
