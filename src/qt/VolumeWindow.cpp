#include "VolumeWindow.hpp"

#include "CloseWindowAction.hpp"
#include "IsoWidget.hpp"
#include "OpacityCurveWidget.hpp"
#include "WidgetImageExport.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QSignalBlocker>
#include <QSlider>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWidget>

namespace amrvis::qt {

namespace {

// One place builds the export's prompts, so they share a title and a default.
// The default is passed rather than left to Qt: for Save | Cancel it picks
// Save, so a reflexive Return -- the key that dismissed the save dialog a
// moment earlier -- would confirm the overwrite the prompt is warning about.
//
// Owned by whatever the caller passes -- see exportImage on why that is not
// the volume window. Titled through VolumeWindow::tr so it shares a
// translation context with the strings passed into it, not QObject's.
int showExportPrompt(QMessageBox::Icon icon, const QString& text,
    QMessageBox::StandardButtons buttons, QMessageBox::StandardButton fallback,
    QWidget* owner = nullptr)
{
    QMessageBox box(icon, VolumeWindow::tr("Export Volume Image"), text,
        buttons, owner);
    box.setDefaultButton(fallback);
    return box.exec();
}

} // namespace

VolumeWindow::VolumeWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Volume Rendering"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(760, 620);
    m_view = new IsoWidget(this);
    m_view->setMinimumSize(320, 240);
    setCentralWidget(m_view);
    connect(m_view, &IsoWidget::cameraChanged, this,
        [this] { emit cameraChanged(); });
    connect(m_view, &IsoWidget::interactionEnded, this,
        [this] { emit interactionEnded(); });
    connect(m_view, &IsoWidget::viewResized, this,
        [this] { emit viewResized(); });
    connect(m_view, &IsoWidget::viewScaleChanged, this,
        [this] { emit viewScaleChanged(); });
    buildControls();

    // File > Export Image...: the view as drawn (frame and overlays), as PNG.
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* exportAction = new QAction(tr("&Export Image..."), this);
    exportAction->setShortcut(QKeySequence::Save);
    connect(exportAction, &QAction::triggered, this, [this] { exportImage(); });
    fileMenu->addAction(exportAction);
    auto* closeAction = addCloseWindowAction(*this, tr("&Close"));
    closeAction->setObjectName(QStringLiteral("volumeCloseAction"));
    fileMenu->addAction(closeAction);
}

void VolumeWindow::exportImage()
{
    // The picture first, before any dialog. A nested modal loop delivers
    // whatever the app has queued -- a finished render, a sequence frame, a
    // dataset switch that clears the frame outright -- so rendering after the
    // prompts writes out something the user never saw. Taking it here costs
    // the buffer for the length of the dialog and saves exactly what was on
    // screen when they asked.
    const auto image = renderWidgetWithoutChildren(
        *m_view, m_view->devicePixelRatioF());
    if (image.isNull()) {
        showExportPrompt(QMessageBox::Critical,
            tr("The picture could not be allocated at this size; "
               "nothing was written."),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }
    // Parented to the main window, not to this one: this window carries
    // WA_DeleteOnClose, and an async dataset switch can close it while a modal
    // is up, which freed a stack-allocated child. The main window outlives it,
    // so the dialogs keep an owner the platform can be transient for -- a
    // null parent loses the sheet on macOS and the owner window elsewhere.
    // `alive` still guards the members below across each nested loop.
    const QPointer<VolumeWindow> alive(this);
    auto* const owner = parentWidget();
    const auto chosen = QFileDialog::getSaveFileName(owner,
        tr("Export Volume Image"), QString(), tr("PNG images (*.png)"));
    if (alive.isNull() || chosen.isEmpty()) {
        return;
    }
    const auto path = pngExportPath(chosen);
    // Asked before writing, not reported after: someone who typed "shot.jpg"
    // may have meant JPEG, and a notice once the PNG is on disk is too late to
    // act on. Exactly one prompt per save either way -- the dialog already
    // confirmed the name it returned, so an unchanged name asks nothing.
    if (path != chosen) {
        auto text = tr("Saving as %1 instead: the picture is written as PNG.")
                        .arg(path);
        if (QFileInfo::exists(path)) {
            text += QLatin1Char('\n') + tr("That file already exists.");
        }
        if (showExportPrompt(QMessageBox::Question, text,
                QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel,
                owner)
                != QMessageBox::Save
            || alive.isNull()) {
            return;
        }
    }
    if (!image.save(path, "PNG")) {
        showExportPrompt(QMessageBox::Critical,
            tr("Cannot write %1").arg(path), QMessageBox::Ok, QMessageBox::Ok);
    }
}

void VolumeWindow::buildControls()
{
    auto* dock = new QDockWidget(tr("Volume"), this);
    dock->setObjectName(QStringLiteral("volumeControlsDock"));
    dock->setFeatures(QDockWidget::DockWidgetMovable
        | QDockWidget::DockWidgetFloatable);
    auto* panel = new QWidget(dock);
    auto* layout = new QVBoxLayout(panel);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    // The opacity over the colour range, as a curve drawn on the palette it
    // shapes. It replaces the "from" / "to" / maximum sliders: those were
    // three numbers describing one linear ramp, and a field with something
    // interesting between two duller features could not be given its own
    // opacity without also revealing them. The default curve is that linear
    // ramp, so an untouched window renders what it always did.
    m_curve = new OpacityCurveWidget(panel);
    m_curve->setObjectName(QStringLiteral("volumeOpacityCurve"));
    form->addRow(tr("Opacity:"), m_curve);
    m_paletteAlpha = new QCheckBox(tr("Use palette alpha ramp"), panel);
    m_paletteAlpha->setObjectName(QStringLiteral("volumePaletteAlphaCheck"));
    form->addRow(QString(), m_paletteAlpha);
    // Whether this is available says whether the palette carries a ramp, so it
    // starts unavailable: until a palette arrives there is nothing to take a
    // ramp from. Left at the check box's own default it would read as "this
    // palette has one" before any palette had been seen, and nothing would be
    // asserting otherwise. Through setPaletteHasAlpha rather than setEnabled
    // so the tooltip is the one that goes with the state -- the other says
    // what the box would do, which is not the question while it cannot be
    // ticked.
    setPaletteHasAlpha(false);
    m_qualityCombo = new QComboBox(panel);
    m_qualityCombo->addItem(tr("Draft"), 0);
    m_qualityCombo->addItem(tr("Normal"), 1);
    m_qualityCombo->addItem(tr("High"), 2);
    m_qualityCombo->setCurrentIndex(1);
    form->addRow(tr("Quality:"), m_qualityCombo);
    // Off by default: limiting the region is a deliberate act, and a volume
    // that cropped itself because a slice view happened to be zoomed would be
    // a surprise on opening the window.
    m_regionCheck = new QCheckBox(tr("Only the visible region"), panel);
    // Named so a test can reach this one rather than whichever check box
    // findChild happens to return first.
    m_regionCheck->setObjectName(QStringLiteral("volumeRegionLimitCheck"));
    m_regionCheck->setToolTip(
        tr("Sample only the part of the domain the slice views are zoomed to, "
           "which spends the voxel budget on it instead of the whole field"));
    form->addRow(QString(), m_regionCheck);
    m_smoothCheck = new QCheckBox(tr("Smooth sampling"), panel);
    m_smoothCheck->setObjectName(QStringLiteral("volumeSmoothSamplingCheck"));
    form->addRow(QString(), m_smoothCheck);
    // Available until a session says otherwise, the way the palette-alpha box
    // is: this is a local render's answer, and setSamplingSelectable revises
    // it for a server that cannot be asked.
    setSamplingSelectable(true);
    // Grid boxes off, domain outline on: box edges crossing a translucent
    // field read as structure in it, which is worth asking for rather than
    // having to switch off. Both are said here and only here -- the view's own
    // defaults suit the main window's quadrant, and the boxes below push these
    // onto it once the toggles exist, so the check box is what the view
    // follows rather than the two agreeing by luck.
    m_boxesCheck = new QCheckBox(tr("Grid boxes"), panel);
    m_boxesCheck->setObjectName(QStringLiteral("volumeGridBoxesCheck"));
    m_boxesCheck->setChecked(false);
    m_outlineCheck = new QCheckBox(tr("Domain outline"), panel);
    m_outlineCheck->setObjectName(QStringLiteral("volumeDomainOutlineCheck"));
    m_outlineCheck->setChecked(true);
    form->addRow(QString(), m_boxesCheck);
    form->addRow(QString(), m_outlineCheck);
    layout->addLayout(form);
    m_status = new QLabel(panel);
    // Named for the same reason the check boxes are: neither label is the one
    // an unqualified findChild would return twice running, and the order they
    // are built in is a layout decision, not an identity.
    m_status->setObjectName(QStringLiteral("volumeStatusLabel"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);
    // Under the status rather than over it, and bold. It comes and goes with
    // every render, so above the status it pushed those lines down and back
    // each time; below, the numbers it belongs to hold still and the weight is
    // what catches the eye instead of the movement.
    m_rendering = new QLabel(panel);
    m_rendering->setObjectName(QStringLiteral("volumeRenderingLabel"));
    auto renderingFont = m_rendering->font();
    renderingFont.setBold(true);
    m_rendering->setFont(renderingFont);
    m_rendering->setVisible(false);
    layout->addWidget(m_rendering);
    layout->addStretch(1);
    dock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    // The window is kept ordered: dragging one threshold past the other
    // pushes the other along.
    connect(m_curve, &OpacityCurveWidget::curveChanged, this,
        [this] { emit rampChanged(); });
    connect(m_paletteAlpha, &QCheckBox::toggled, this, [this] {
        // The curve has no say while the palette's own ramp is in use, so it
        // is disabled rather than left looking editable and doing nothing.
        syncCurveEnabled();
        emit paletteAlphaChanged();
    });
    connect(m_qualityCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int) { emit qualityChanged(); });
    connect(m_regionCheck, &QCheckBox::toggled, this,
        [this] { emit regionLimitChanged(); });
    connect(m_smoothCheck, &QCheckBox::toggled, this, [this](bool on) {
        // Only what the user asked for: the tick setSamplingSelectable takes
        // away and puts back is blocked, so it never overwrites this.
        m_smoothWanted = on;
        emit samplingChanged();
    });
    connect(m_boxesCheck, &QCheckBox::toggled, this,
        [this](bool checked) { m_view->setLevelBoxesVisible(checked); });
    connect(m_outlineCheck, &QCheckBox::toggled, this,
        [this](bool checked) { m_view->setDomainOutlineVisible(checked); });
    // The state above reaches the view once, here, because setting a box
    // before its connect fires no toggle. Both go through the same call the
    // toggles use, so there is one path from box to view and no dependence on
    // what the view happened to default to.
    m_view->setLevelBoxesVisible(m_boxesCheck->isChecked());
    m_view->setDomainOutlineVisible(m_outlineCheck->isChecked());
}

void VolumeWindow::setDatasetGeometry(const DatasetMetadata& metadata)
{
    m_view->setGeometry(metadata);
}

void VolumeWindow::setSlicePositions(double x, double y, double z)
{
    m_view->setSlicePositions(x, y, z);
}

void VolumeWindow::setSlicePlanesVisible(bool visible)
{
    m_view->setSlicePlanesVisible(visible);
}

void VolumeWindow::setColorPalette(const Palette* palette)
{
    m_curve->setColorPalette(palette);
    m_view->setColorPalette(palette);
}

void VolumeWindow::syncCurveEnabled()
{
    // The same condition ramp() reports usePaletteAlpha under, so what the
    // control offers and what the render does cannot disagree: a ticked box on
    // a palette with no ramp is not in effect, and the curve stays editable.
    m_curve->setEnabled(
        !(m_paletteAlpha->isEnabled() && m_paletteAlpha->isChecked()));
}

void VolumeWindow::setPaletteHasAlpha(bool hasAlpha)
{
    m_paletteAlpha->setEnabled(hasAlpha);
    syncCurveEnabled();
    if (!hasAlpha && m_paletteAlpha->isChecked()) {
        // ramp() ands in isEnabled(), so the render already ignores a ticked
        // box on a palette with no ramp -- but the box would sit there ticked,
        // claiming an opacity source that is not in use, and re-arm itself the
        // moment any palette with a ramp is selected. Silently: the palette
        // change that caused this is already scheduling a render.
        const QSignalBlocker blocker(m_paletteAlpha);
        m_paletteAlpha->setChecked(false);
    }
    m_paletteAlpha->setToolTip(hasAlpha
        ? tr("Take each colour's opacity from the palette's alpha ramp "
             "instead of the curve above, which is disabled while this is on.")
        : tr("This palette carries no alpha ramp; load a legacy .pal file "
             "with one to enable this."));
}

void VolumeWindow::showFrame(const VolumeFrame& frame,
    const OrthoCamera& camera, const QString& status)
{
    // Premultiplied ARGB, row 0 at the top: exactly the frame's layout, so
    // the image wraps the pixels and copies them once.
    const QImage wrapped(reinterpret_cast<const uchar*>(frame.pixels.data()),
        frame.width, frame.height, frame.width * static_cast<int>(sizeof(std::uint32_t)),
        QImage::Format_ARGB32_Premultiplied);
    m_view->setBackdropImage(wrapped.copy(), camera);
    m_status->setText(status);
}

void VolumeWindow::showFailure(const QString& message)
{
    // The frame stays: it is the last thing that did render, and replacing it
    // with nothing would lose the view. The status says why it is not moving,
    // which the main window's status bar cannot do from behind this window.
    m_status->setText(message);
}

void VolumeWindow::clearFrame()
{
    m_view->setBackdropImage(QImage(), OrthoCamera{});
    m_status->clear();
}

void VolumeWindow::showRendering(bool rendering)
{
    m_rendering->setText(rendering ? tr("Rendering…") : QString());
    m_rendering->setVisible(rendering);
}

const OrthoCamera& VolumeWindow::camera() const noexcept
{
    return m_view->camera();
}

QSize VolumeWindow::viewSize() const
{
    return m_view->size();
}

bool VolumeWindow::limitToVisibleRegion() const
{
    return m_regionCheck->isChecked();
}

SamplingPolicy VolumeWindow::sampling() const
{
    // The same shape as the palette-alpha box: a ticked box that is not
    // available is not in effect, so what the control offers and what the
    // render does cannot disagree.
    return m_smoothCheck->isEnabled() && m_smoothCheck->isChecked()
        ? SamplingPolicy::Linear
        : SamplingPolicy::Nearest;
}

void VolumeWindow::setSamplingSelectable(bool selectable)
{
    m_smoothCheck->setEnabled(selectable);
    // Unticked while unavailable, so it does not sit there claiming a
    // smoothness the picture does not have -- and ticked again from what was
    // last asked of it when it comes back. This is where it parts company
    // with the palette-alpha box it otherwise copies: that one is off by
    // default and has nothing to restore, while this one is on, so leaving it
    // clear after one older server would turn the default off for the rest of
    // the session. Silently either way, since whatever changed the session is
    // already rendering.
    const QSignalBlocker blocker(m_smoothCheck);
    m_smoothCheck->setChecked(selectable && m_smoothWanted);
    m_smoothCheck->setToolTip(selectable
            ? tr("Read each ray sample from the eight voxels around it "
                 "instead of the one it lands in, which is what stops a "
                 "coarse volume looking terraced")
            : tr("This server predates smooth sampling (protocol 1.3) and "
                 "always reads the nearest voxel; install a current "
                 "amrexplorer-server"));
}

qreal VolumeWindow::viewDevicePixelRatio() const
{
    return m_view->devicePixelRatioF();
}

OpacityRamp VolumeWindow::ramp() const
{
    // usePaletteAlpha decides which of the two shapes the render reads. With
    // it off, makeVolumeTransferFunction takes the curve. With it on, the
    // curve stands aside and that function reads the window fields instead --
    // which is why they are left at their defaults here rather than set from
    // anything. At those defaults the window spans the whole range at full
    // maximum, so it is no window at all and the palette's authored ramp comes
    // back untouched, which is the reason to tick the box.
    //
    // The widget keeps the points sorted, in range, and at least two: the
    // invariants the curve branch reads them under.
    OpacityRamp ramp;
    ramp.usePaletteAlpha = m_paletteAlpha->isEnabled() && m_paletteAlpha->isChecked();
    ramp.curve = m_curve->curve();
    return ramp;
}

VolumeWindow::Quality VolumeWindow::quality() const
{
    switch (m_qualityCombo->currentData().toInt()) {
    case 0:
        return {1, 128ULL * 128ULL * 128ULL};
    case 2:
        return {4, 384ULL * 384ULL * 384ULL};
    default:
        return {2, defaultVolumeVoxelBudget};
    }
}

} // namespace amrvis::qt
