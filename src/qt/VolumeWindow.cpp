#include "VolumeWindow.hpp"

#include "IsoWidget.hpp"
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
#include <QSignalBlocker>
#include <QSlider>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWidget>

namespace amrvis::qt {

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
    buildControls();

    // File > Export Image...: the view as drawn (frame and overlays), as PNG.
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* exportAction = new QAction(tr("&Export Image..."), this);
    exportAction->setObjectName(QStringLiteral("volumeExportImageAction"));
    exportAction->setShortcut(QKeySequence::Save);
    connect(exportAction, &QAction::triggered, this, [this] { exportImage(); });
    fileMenu->addAction(exportAction);
    auto* closeAction = new QAction(tr("&Close"), this);
    closeAction->setObjectName(QStringLiteral("volumeCloseAction"));
    closeAction->setShortcut(QKeySequence::Close);
    connect(closeAction, &QAction::triggered, this, [this] { close(); });
    fileMenu->addAction(closeAction);
}

QImage VolumeWindow::renderedView(qreal devicePixelRatio) const
{
    if (!m_view->hasBackdropImage()) {
        return {};
    }
    return renderWidgetWithoutChildren(*m_view, devicePixelRatio);
}

void VolumeWindow::exportImage()
{
    // Refused before the dialog, as the main window's export does: with no
    // frame the view is the bare wireframe, or the "3-D overview" placeholder
    // text, and writing either out gives the user a file to puzzle over. The
    // cheap question here, not the picture -- that is taken after the dialog,
    // so it is the frame on screen when the save is confirmed rather than one
    // held across a nested event loop.
    if (!m_view->hasBackdropImage()) {
        QMessageBox::information(this, tr("Export Volume Image"),
            tr("Wait for the volume to render before exporting an image."));
        return;
    }
    const auto chosen = QFileDialog::getSaveFileName(this,
        tr("Export Volume Image"), QString(), tr("PNG images (*.png)"));
    if (chosen.isEmpty()) {
        return;
    }
    const auto path = pngExportPath(chosen);
    // Only when that appended something. The dialog has already confirmed
    // overwriting the name it returned, so asking again about that same name
    // would be a second prompt for one save; but it never asked about a name
    // appended to after it returned -- a typed "shot" beside an existing
    // shot.png passes its existence check and would be overwritten silently.
    const bool renamed = path != chosen;
    if (renamed && QFileInfo::exists(path)
        && QMessageBox::question(this, tr("Export Volume Image"),
               tr("%1 already exists. Overwrite it?").arg(path))
            != QMessageBox::Yes) {
        return;
    }
    const auto image = renderedView(devicePixelRatioF());
    if (image.isNull() || !image.save(path, "PNG")) {
        QMessageBox::critical(this, tr("Export Volume Image"),
            tr("Cannot write %1").arg(path));
        return;
    }
    // The name is not the one that was typed, so say where the file went
    // rather than leaving the user to find it. In the status line, not a box:
    // the save worked, and this does not need dismissing.
    if (renamed) {
        m_status->setText(tr("Exported to %1").arg(path));
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

    // The opacity window over the colour range: transparent below "from",
    // ramping to the maximum at "to"; the labels read the slider positions.
    const auto makeSlider = [panel](int initial) {
        auto* slider = new QSlider(Qt::Horizontal, panel);
        slider->setRange(0, 100);
        slider->setValue(initial);
        return slider;
    };
    m_lowSlider = makeSlider(0);
    m_highSlider = makeSlider(100);
    m_maximumSlider = makeSlider(100);
    m_lowLabel = new QLabel(panel);
    m_highLabel = new QLabel(panel);
    m_maximumLabel = new QLabel(panel);
    const auto rampRow = [panel](QSlider* slider, QLabel* label) {
        auto* row = new QWidget(panel);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->addWidget(slider, 1);
        label->setMinimumWidth(36);
        rowLayout->addWidget(label);
        return row;
    };
    form->addRow(tr("Opacity from:"), rampRow(m_lowSlider, m_lowLabel));
    form->addRow(tr("Opacity to:"), rampRow(m_highSlider, m_highLabel));
    form->addRow(tr("Maximum opacity:"), rampRow(m_maximumSlider, m_maximumLabel));
    m_paletteAlpha = new QCheckBox(tr("Use palette alpha ramp"), panel);
    m_paletteAlpha->setToolTip(tr("Take each colour's opacity from the palette's "
                                  "alpha ramp (legacy .pal files carry one) "
                                  "instead of the linear window above."));
    form->addRow(QString(), m_paletteAlpha);
    m_qualityCombo = new QComboBox(panel);
    m_qualityCombo->addItem(tr("Draft"), 0);
    m_qualityCombo->addItem(tr("Normal"), 1);
    m_qualityCombo->addItem(tr("High"), 2);
    m_qualityCombo->setCurrentIndex(1);
    form->addRow(tr("Quality:"), m_qualityCombo);
    m_boxesCheck = new QCheckBox(tr("Grid boxes"), panel);
    m_boxesCheck->setChecked(true);
    m_outlineCheck = new QCheckBox(tr("Domain outline"), panel);
    m_outlineCheck->setChecked(true);
    form->addRow(QString(), m_boxesCheck);
    form->addRow(QString(), m_outlineCheck);
    layout->addLayout(form);
    m_rendering = new QLabel(panel);
    m_rendering->setVisible(false);
    layout->addWidget(m_rendering);
    m_status = new QLabel(panel);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);
    layout->addStretch(1);
    dock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    const auto updateLabels = [this] {
        m_lowLabel->setText(QStringLiteral("%1%").arg(m_lowSlider->value()));
        m_highLabel->setText(QStringLiteral("%1%").arg(m_highSlider->value()));
        m_maximumLabel->setText(
            QStringLiteral("%1%").arg(m_maximumSlider->value()));
    };
    updateLabels();
    // The window is kept ordered: dragging one threshold past the other
    // pushes the other along.
    connect(m_lowSlider, &QSlider::valueChanged, this, [this, updateLabels](int value) {
        if (m_highSlider->value() < value) {
            m_highSlider->setValue(value);
        }
        updateLabels();
        emit rampChanged();
    });
    connect(m_highSlider, &QSlider::valueChanged, this, [this, updateLabels](int value) {
        if (m_lowSlider->value() > value) {
            m_lowSlider->setValue(value);
        }
        updateLabels();
        emit rampChanged();
    });
    connect(m_maximumSlider, &QSlider::valueChanged, this, [this, updateLabels] {
        updateLabels();
        emit rampChanged();
    });
    connect(m_paletteAlpha, &QCheckBox::toggled, this,
        [this] { emit paletteAlphaChanged(); });
    connect(m_qualityCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int) { emit qualityChanged(); });
    connect(m_boxesCheck, &QCheckBox::toggled, this,
        [this](bool checked) { m_view->setLevelBoxesVisible(checked); });
    connect(m_outlineCheck, &QCheckBox::toggled, this,
        [this](bool checked) { m_view->setDomainOutlineVisible(checked); });
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
    m_view->setColorPalette(palette);
}

void VolumeWindow::setPaletteHasAlpha(bool hasAlpha)
{
    m_paletteAlpha->setEnabled(hasAlpha);
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
             "instead of the linear window above.")
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

OpacityRamp VolumeWindow::ramp() const
{
    // Every field comes from a slider in [0, 100], and buildControls keeps
    // low <= high by pushing the other along. makeVolumeTransferFunction
    // throws on a non-finite or inverted window, so anything that lets these
    // controls produce one -- a text entry, a restored setting -- has to
    // resolve it here rather than hand it to the render.

    OpacityRamp ramp;
    ramp.lowThreshold = m_lowSlider->value() / 100.0;
    ramp.highThreshold = m_highSlider->value() / 100.0;
    ramp.maximumOpacity = m_maximumSlider->value() / 100.0;
    ramp.usePaletteAlpha = m_paletteAlpha->isEnabled() && m_paletteAlpha->isChecked();
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
