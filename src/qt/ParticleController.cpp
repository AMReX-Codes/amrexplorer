#include "ParticleController.hpp"

#include "PaletteController.hpp"
#include "ScientificDoubleSpinBox.hpp"

#include "QtErrorText.hpp"

#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <utility>

namespace amrvis::qt {

namespace {

constexpr std::array<Qt::GlobalColor, 7> particleDefaultColors{
    Qt::white, Qt::yellow, Qt::cyan, Qt::magenta,
    Qt::green, Qt::red, Qt::lightGray};

QColor defaultParticleColor(std::size_t speciesIndex)
{
    return QColor(
        particleDefaultColors[speciesIndex % particleDefaultColors.size()]);
}

MarkerShape defaultParticleShape(std::size_t speciesIndex)
{
    return markerShapes[speciesIndex % markerShapes.size()];
}

QString markerShapeName(MarkerShape shape)
{
    switch (shape) {
    case MarkerShape::Circle: return ParticleController::tr("Circle");
    case MarkerShape::Square: return ParticleController::tr("Square");
    case MarkerShape::Diamond: return ParticleController::tr("Diamond");
    case MarkerShape::Triangle: return ParticleController::tr("Triangle");
    case MarkerShape::Cross: return ParticleController::tr("Cross");
    }
    return {};
}

void updateColorButton(QPushButton& button, const QColor& color)
{
    QPixmap swatch(18, 18);
    swatch.fill(color);
    button.setIcon(QIcon(swatch));
    button.setText(color.name(QColor::HexRgb).toUpper());
}

} // namespace

ParticleController::ParticleController(Hooks hooks, QObject* parent)
    : QObject(parent)
    , m_hooks(std::move(hooks))
{
}

QAction* ParticleController::createAction(QObject* parent)
{
    auto* action = new QAction(tr("&Particles..."), parent);
    action->setObjectName(QStringLiteral("particlesAction"));
    action->setEnabled(false);
    m_action = action;
    return action;
}

QProgressBar* ParticleController::createProgress(QWidget* parent)
{
    auto* progress = new QProgressBar(parent);
    progress->setRange(0, 0);
    progress->setFormat(tr("Loading particles..."));
    progress->setAccessibleName(tr("Loading particle sample"));
    progress->setFixedWidth(170);
    progress->setVisible(false);
    m_progress = progress;
    return progress;
}

QColor ParticleController::colorFor(const std::string& species) const
{
    const auto color = m_settings.colors.find(species);
    return color != m_settings.colors.end() ? color->second : QColor(Qt::white);
}

MarkerShape ParticleController::shapeFor(const std::string& species) const
{
    const auto shape = m_settings.shapes.find(species);
    return shape != m_settings.shapes.end() ? shape->second : MarkerShape::Circle;
}

std::vector<std::string> ParticleController::attributeNames() const
{
    const auto dataset = m_hooks.dataset ? m_hooks.dataset() : nullptr;
    std::vector<std::string> names;
    if (!dataset || !dataset->supportsParticleAttributes()) {
        return names;
    }
    const auto add = [&names](const std::vector<std::string>& components) {
        for (const auto& name : components) {
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
        }
    };
    for (const auto& species : dataset->particleSpecies()) {
        add(species.realComponentNames);
        add(species.intComponentNames);
    }
    return names;
}

bool ParticleController::sampleColored(const ParticleSample& sample) const
{
    const auto& attribute = m_settings.coloring.attribute;
    return !attribute.empty() && sample.attribute
        && findParticleAttribute(sample.species, attribute) == sample.attribute;
}

std::optional<ParticleColorRange> ParticleController::colorRange() const
{
    const auto& coloring = m_settings.coloring;
    const auto colored = [this](const ParticleSample& sample) { return sampleColored(sample); };
    if (std::none_of(m_samples.begin(), m_samples.end(), colored)) {
        return std::nullopt;
    }
    return coloring.range ? coloring.range
                          : particleValueRange(m_samples, coloring.logarithmic, colored);
}

const Palette& ParticleController::colorPalette() const
{
    const auto index = std::clamp(m_settings.coloring.palette, 0,
        static_cast<int>(builtinPalettes.size()) - 1);
    return builtinPalette(builtinPalettes[static_cast<std::size_t>(index)]);
}

void ParticleController::applySelection(std::vector<std::string> species,
    double fraction, int pointSize, std::uint64_t seed, bool sliceCellsOnly)
{
    applySelection(std::move(species), fraction, pointSize, seed,
        sliceCellsOnly, m_settings.coloring);
}

void ParticleController::applySelection(std::vector<std::string> species,
    double fraction, int pointSize, std::uint64_t seed, bool sliceCellsOnly,
    Coloring coloring)
{
    const bool sampleChanged = !m_settings.selectionInitialized
        || species != m_settings.species || fraction != m_settings.fraction
        || seed != m_settings.seed
        || coloring.attribute != m_settings.coloring.attribute;
    m_settings.coloring = std::move(coloring);
    m_settings.species = std::move(species);
    m_settings.fraction = fraction;
    m_settings.seed = seed;
    m_settings.pointSize = pointSize;
    m_settings.sliceCellsOnly = sliceCellsOnly;
    m_settings.selectionInitialized = true;
    if (!sampleChanged) {
        // Colour, alpha, shape, point size, the color scale and the slice-cell
        // filter only affect the
        // installed point batches; do not reread particle files when the
        // sampled identities are unchanged.
        emit overlaysChanged();
        return;
    }
    emit sampleSelectionChanged();
}

void ParticleController::setColor(const std::string& species, const QColor& color)
{
    m_settings.colors[species] = color;
    emit overlaysChanged();
}

void ParticleController::setShape(const std::string& species, MarkerShape shape)
{
    m_settings.shapes[species] = shape;
    emit overlaysChanged();
}

void ParticleController::restoreSelection(std::vector<std::string> species,
    double fraction, std::uint64_t seed, bool selectionInitialized,
    std::string colorAttribute)
{
    m_settings.coloring.attribute = std::move(colorAttribute);
    m_settings.species = std::move(species);
    m_settings.fraction = fraction;
    m_settings.seed = seed;
    m_settings.selectionInitialized = selectionInitialized;
}

void ParticleController::resetSettings()
{
    m_settings = Settings{};
}

void ParticleController::clearSelection()
{
    m_settings.species.clear();
    m_settings.selectionInitialized = false;
}

void ParticleController::configureForDataset(bool preserveSelection)
{
    // A dataset has landed (or none has): whatever the host suspended the
    // action for is over.
    m_suspended = false;
    const auto dataset = m_hooks.dataset ? m_hooks.dataset() : nullptr;
    if (dataset) {
        const auto& species = dataset->particleSpecies();
        if (!preserveSelection) {
            resetSettings();
        }
        for (std::size_t index = 0; index < species.size(); ++index) {
            m_settings.colors.try_emplace(
                species[index].name, defaultParticleColor(index));
            m_settings.shapes.try_emplace(
                species[index].name, defaultParticleShape(index));
        }
    }
    refreshActionEnabled();
}

void ParticleController::suspendAction()
{
    m_suspended = true;
    refreshActionEnabled();
}

void ParticleController::refreshActionEnabled()
{
    const auto dataset = m_hooks.dataset ? m_hooks.dataset() : nullptr;
    setActionEnabled(dataset && !dataset->particleSpecies().empty()
        && !m_loading && !m_suspended);
}

void ParticleController::setActionEnabled(bool enabled)
{
    if (m_action) {
        m_action->setEnabled(enabled);
    }
}

void ParticleController::setSamples(std::vector<ParticleSample> samples)
{
    // Silent: the host installs a frame's samples before it has finished
    // switching views, and redraws the overlays itself once it has.
    m_samples = std::move(samples);
}

void ParticleController::clearSamples()
{
    m_samples.clear();
}

void ParticleController::setLoadingUi(bool loading)
{
    m_loading = loading;
    if (m_progress) {
        m_progress->setVisible(loading);
    }
}

void ParticleController::cancel()
{
    m_stopSource.request_stop();
    ++m_generation;
    setLoadingUi(false);
    // The load took the action down and its stale result will not put it
    // back; refresh here, for whatever dataset the host still shows -- unless
    // the host suspended it, which holds until the next dataset lands.
    refreshActionEnabled();
}

void ParticleController::reload()
{
    m_stopSource.request_stop();
    m_stopSource = StopSource{};
    const auto cancellation = m_stopSource.get_token();
    const auto dataset = m_hooks.dataset ? m_hooks.dataset() : nullptr;
    const auto selectedSpecies = m_settings.species;
    const auto fraction = m_settings.fraction;
    const auto seed = m_settings.seed;
    const auto attribute = m_settings.coloring.attribute;
    const auto generation = ++m_generation;
    m_samples.clear();
    emit overlaysChanged();
    if (!dataset || selectedSpecies.empty()) {
        setLoadingUi(false);
        refreshActionEnabled();
        return;
    }

    setLoadingUi(true);
    refreshActionEnabled();
    emit loadActivityChanged(1);
    emit statusMessage(tr("Loading particle sample..."), 0);
    auto* watcher = new QFutureWatcher<std::vector<ParticleSample>>(this);
    connect(watcher, &QFutureWatcher<std::vector<ParticleSample>>::finished,
        this, [this, watcher, generation, cancellation] {
            emit loadActivityChanged(-1);
            // During shutdown the handler below touches the progress widget
            // and the overlays; drop the result instead.
            if (m_hooks.isShuttingDown && m_hooks.isShuttingDown()) {
                watcher->deleteLater();
                return;
            }
            try {
                auto samples = watcher->future().takeResult();
                if (generation == m_generation) {
                    m_samples = std::move(samples);
                    emit overlaysChanged();
                    std::uint64_t count = 0;
                    for (const auto& sample : m_samples) {
                        count += sample.points.size();
                    }
                    emit statusMessage(
                        tr("Showing %1 sampled particles").arg(count), 3000);
                } else {
                    emit staleResultDropped();
                }
            } catch (const std::exception& error) {
                if (generation == m_generation
                    && !cancellation.stop_requested()) {
                    emit loadFailed(tr("Particles were not loaded: %1")
                            .arg(exceptionMessage(error)));
                } else {
                    // Counted like every other superseded result.
                    emit staleResultDropped();
                }
            }
            if (generation == m_generation) {
                setLoadingUi(false);
                refreshActionEnabled();
            }
            emit loadFinished();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [dataset, selectedSpecies, fraction, seed, attribute, cancellation] {
            return loadParticleSamples(*dataset, selectedSpecies, fraction,
                seed, cancellation, attribute);
        }));
}

bool ParticleController::loadingUiActive() const
{
    return m_progress && m_progress->isVisible() && m_action
        && !m_action->isEnabled();
}

bool ParticleController::loadingUiSettled() const
{
    return m_progress && !m_progress->isVisible() && m_action
        && m_action->isEnabled();
}

void ParticleController::closeDialog()
{
    if (m_dialog) {
        auto* dialog = m_dialog.data();
        m_dialog = nullptr;
        dialog->close();
    }
}

void ParticleController::showDialog(QWidget* parent)
{
    if (m_dialog) {
        m_dialog->raise();
        m_dialog->activateWindow();
        return;
    }
    const auto dataset = m_hooks.dataset ? m_hooks.dataset() : nullptr;
    if (!dataset || dataset->particleSpecies().empty()) {
        return;
    }
    // Modeless, like the contours dialog: settings are worth trying against
    // the image, and a modal dialog made every attempt a reopen -- and dimmed
    // the main window while it was up on Linux.
    auto* dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("particlesDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Particles"));
    dialog->setWindowFlags(Qt::Window);
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(
        tr("Select particle species to draw. Sampling hashes the persistent "
           "particle ID/CPU identity, so the same particles remain selected "
           "across plotfile frames."),
        dialog));

    struct SpeciesControls {
        std::string name;
        QCheckBox* enabled = nullptr;
        QPushButton* colorButton = nullptr;
        QSpinBox* alpha = nullptr;
        QComboBox* shape = nullptr;
        QColor color;
    };
    // The dialog outlives this call, so the per-species state has to as
    // well; the button handlers keep it alive by shared ownership.
    auto speciesControls = std::make_shared<std::vector<SpeciesControls>>();
    const auto& allSpecies = dataset->particleSpecies();
    speciesControls->reserve(allSpecies.size());
    auto* speciesGrid = new QGridLayout;
    speciesGrid->addWidget(new QLabel(tr("Show"), dialog), 0, 0);
    speciesGrid->addWidget(new QLabel(tr("Species"), dialog), 0, 1);
    speciesGrid->addWidget(new QLabel(tr("Color"), dialog), 0, 2);
    speciesGrid->addWidget(new QLabel(tr("Alpha"), dialog), 0, 3);
    speciesGrid->addWidget(new QLabel(tr("Shape"), dialog), 0, 4);
    for (std::size_t speciesIndex = 0; speciesIndex < allSpecies.size();
         ++speciesIndex) {
        const auto& species = allSpecies[speciesIndex];
        auto* check = new QCheckBox(dialog);
        check->setObjectName(QStringLiteral("particleShow"));
        check->setChecked(!m_settings.selectionInitialized
            || std::find(m_settings.species.begin(), m_settings.species.end(),
                   species.name)
                != m_settings.species.end());
        auto* name = new QLabel(
            tr("%1 (%2 particles)")
                .arg(QString::fromStdString(species.name))
                .arg(species.particleCount),
            dialog);
        const auto storedColor = m_settings.colors.find(species.name);
        const auto stored = storedColor != m_settings.colors.end()
            ? storedColor->second
            : defaultParticleColor(speciesIndex);
        // The spin box owns opacity and the swatch shows the hue alone. Held
        // together, an Apply that folded the alpha into the swatch would
        // leave it disagreeing with the spin box beside it until the next
        // colour pick, which would then bake that alpha into the newly
        // picked colour.
        auto color = stored;
        color.setAlpha(255);
        auto* colorButton = new QPushButton(dialog);
        updateColorButton(*colorButton, color);
        auto* alpha = new QSpinBox(dialog);
        alpha->setRange(0, 100);
        alpha->setSuffix(tr("%"));
        alpha->setValue(qRound(stored.alphaF() * 100.0));
        auto* shape = new QComboBox(dialog);
        shape->setObjectName(QStringLiteral("particleShape"));
        for (const auto option : markerShapes) {
            shape->addItem(markerShapeName(option), static_cast<int>(option));
        }
        const auto storedShape = m_settings.shapes.find(species.name);
        shape->setCurrentIndex(shape->findData(static_cast<int>(
            storedShape != m_settings.shapes.end()
                ? storedShape->second
                : defaultParticleShape(speciesIndex))));
        const auto row = static_cast<int>(speciesIndex + 1);
        speciesGrid->addWidget(check, row, 0, Qt::AlignHCenter);
        speciesGrid->addWidget(name, row, 1);
        speciesGrid->addWidget(colorButton, row, 2);
        speciesGrid->addWidget(alpha, row, 3);
        speciesGrid->addWidget(shape, row, 4);
        speciesControls->push_back(
            {species.name, check, colorButton, alpha, shape, color});
    }
    for (std::size_t index = 0; index < speciesControls->size(); ++index) {
        connect((*speciesControls)[index].colorButton, &QPushButton::clicked,
            dialog, [dialog, speciesControls, index] {
                auto& controls = (*speciesControls)[index];
                auto chosen = QColorDialog::getColor(
                    controls.color, dialog, tr("Particle color"));
                if (!chosen.isValid()) {
                    return;
                }
                controls.color = chosen;
                updateColorButton(*controls.colorButton, controls.color);
            });
    }
    layout->addLayout(speciesGrid);

    auto* fractionRow = new QHBoxLayout;
    fractionRow->addWidget(new QLabel(tr("Visible subset:"), dialog));
    auto* fraction = new QDoubleSpinBox(dialog);
    fraction->setRange(0.01, 100.0);
    fraction->setDecimals(2);
    fraction->setSuffix(tr("%"));
    fraction->setValue(m_settings.fraction * 100.0);
    fractionRow->addWidget(fraction);
    fractionRow->addStretch(1);
    layout->addLayout(fractionRow);

    auto* seedRow = new QHBoxLayout;
    seedRow->addWidget(new QLabel(tr("Sampling seed:"), dialog));
    auto* seed = new QLineEdit(QString::number(m_settings.seed), dialog);
    seed->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{1,20}")), seed));
    seed->setToolTip(tr(
        "Change the seed to select a different stable particle subset."));
    seedRow->addWidget(seed);
    seedRow->addStretch(1);
    layout->addLayout(seedRow);

    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(new QLabel(tr("Point size:"), dialog));
    auto* pointSize = new QSpinBox(dialog);
    pointSize->setRange(1, 12);
    pointSize->setValue(m_settings.pointSize);
    sizeRow->addWidget(pointSize);
    sizeRow->addStretch(1);
    layout->addLayout(sizeRow);

    // Coloring by one attribute every species with that component shares.
    const auto& coloring = m_settings.coloring;
    auto* colorGrid = new QGridLayout;
    auto* colorBy = new QComboBox(dialog);
    colorBy->setObjectName(QStringLiteral("particleColorBy"));
    colorBy->addItem(tr("Species color"), QString());
    for (const auto& name : attributeNames()) {
        colorBy->addItem(QString::fromStdString(name), QString::fromStdString(name));
    }
    // One this frame lacks stays listed, so an Apply for something else
    // keeps it for the frames that have it.
    const auto stored = QString::fromStdString(coloring.attribute);
    if (!stored.isEmpty() && colorBy->findData(stored) < 0) {
        colorBy->addItem(tr("%1 (not in this frame)").arg(stored), stored);
    }
    const auto storedAttribute = colorBy->findData(stored);
    colorBy->setCurrentIndex(std::max(storedAttribute, 0));
    if (!dataset->supportsParticleAttributes()) {
        colorBy->setToolTip(tr("The server's protocol predates particle attributes."));
    }
    colorBy->setEnabled(colorBy->count() > 1);
    auto* colormap = new QComboBox(dialog);
    colormap->setObjectName(QStringLiteral("particleColormap"));
    for (std::size_t index = 0; index < builtinPalettes.size(); ++index) {
        colormap->addItem(builtinPaletteLabel(index));
    }
    colormap->setCurrentIndex(std::clamp(coloring.palette, 0, colormap->count() - 1));
    auto* logarithmic = new QCheckBox(tr("Log scale"), dialog);
    logarithmic->setObjectName(QStringLiteral("particleColorLog"));
    logarithmic->setChecked(coloring.logarithmic);
    auto* fixedRange = new QCheckBox(tr("Fixed range"), dialog);
    fixedRange->setObjectName(QStringLiteral("particleColorFixedRange"));
    fixedRange->setChecked(coloring.range.has_value());
    const auto shownRange = coloring.range ? coloring.range : colorRange();
    auto* rangeMinimum = new ScientificDoubleSpinBox(dialog);
    auto* rangeMaximum = new ScientificDoubleSpinBox(dialog);
    rangeMinimum->setObjectName(QStringLiteral("particleColorMinimum"));
    rangeMaximum->setObjectName(QStringLiteral("particleColorMaximum"));
    for (auto* bound : {rangeMinimum, rangeMaximum}) {
        bound->setRange(-std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max());
    }
    rangeMinimum->setValue(shownRange ? shownRange->minimum : 0.0);
    rangeMaximum->setValue(shownRange ? shownRange->maximum : 1.0);
    colorGrid->addWidget(new QLabel(tr("Color by:"), dialog), 0, 0);
    colorGrid->addWidget(colorBy, 0, 1, 1, 2);
    colorGrid->addWidget(new QLabel(tr("Colormap:"), dialog), 1, 0);
    colorGrid->addWidget(colormap, 1, 1, 1, 2);
    colorGrid->addWidget(logarithmic, 2, 1, 1, 2);
    colorGrid->addWidget(fixedRange, 3, 1, 1, 2);
    colorGrid->addWidget(rangeMinimum, 4, 1);
    colorGrid->addWidget(rangeMaximum, 4, 2);
    layout->addLayout(colorGrid);
    const auto refreshColorControls = [colorBy, colormap, logarithmic,
                                          fixedRange, rangeMinimum, rangeMaximum] {
        const bool colored = !colorBy->currentData().toString().isEmpty();
        colormap->setEnabled(colored);
        logarithmic->setEnabled(colored);
        fixedRange->setEnabled(colored);
        rangeMinimum->setEnabled(colored && fixedRange->isChecked());
        rangeMaximum->setEnabled(colored && fixedRange->isChecked());
    };
    connect(colorBy, &QComboBox::currentIndexChanged, dialog, refreshColorControls);
    connect(fixedRange, &QCheckBox::toggled, dialog, refreshColorControls);
    refreshColorControls();

    // 3-D only: in 2-D the slice is the whole domain, so every particle is
    // already in a cell the plane crosses and the filter would do nothing.
    QCheckBox* sliceCellsOnly = nullptr;
    if (dataset->metadata().dimension == 3) {
        sliceCellsOnly = new QCheckBox(
            tr("Only particles in cells the slice crosses"), dialog);
        // Named, like the species rows' boxes, so a test finds each by name.
        sliceCellsOnly->setObjectName(
            QStringLiteral("particlesSliceCellsOnly"));
        sliceCellsOnly->setChecked(m_settings.sliceCellsOnly);
        sliceCellsOnly->setToolTip(
            tr("Draw a particle only where it lies inside the cell the slice "
               "plane cuts. Unticked, every particle is projected onto the "
               "plane through the whole volume."));
        layout->addWidget(sliceCellsOnly);
        layout->addWidget(new QLabel(
            tr("In 3-D, points are projected onto each orthogonal view."),
            dialog));
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok
            | QDialogButtonBox::Apply | QDialogButtonBox::Cancel,
        dialog);
    buttons->setObjectName(QStringLiteral("particlesDialogButtons"));
    // Context is this controller: the connection cannot outlive the object
    // the lambda acts on, and it goes with the dialog's buttons anyway.
    connect(buttons, &QDialogButtonBox::clicked, this,
        [this, dialog, buttons, speciesControls, fraction, seed, pointSize,
            sliceCellsOnly, colorBy, colormap, logarithmic, fixedRange,
            rangeMinimum, rangeMaximum](QAbstractButton* button) {
            const auto role = buttons->buttonRole(button);
            if (role == QDialogButtonBox::RejectRole) {
                dialog->reject();
                return;
            }
            // Only Ok and Apply act; anything added later (a Reset, a Help)
            // must not be treated as a Cancel and dismiss the dialog.
            if (role != QDialogButtonBox::AcceptRole
                && role != QDialogButtonBox::ApplyRole) {
                return;
            }
            bool valid = false;
            const auto seedValue = seed->text().toULongLong(&valid);
            if (!valid) {
                QMessageBox::warning(dialog, tr("Invalid seed"),
                    tr("The sampling seed must be an integer from 0 through "
                       "18446744073709551615."));
                return;
            }
            Coloring newColoring;
            newColoring.attribute = colorBy->currentData().toString().toStdString();
            newColoring.palette = colormap->currentIndex();
            newColoring.logarithmic = logarithmic->isChecked();
            // Only a range in use is checked: on species colors its controls
            // are disabled and could not be corrected.
            if (!newColoring.attribute.empty() && fixedRange->isChecked()) {
                newColoring.range = ParticleColorRange{
                    rangeMinimum->value(), rangeMaximum->value()};
                if (!(newColoring.range->maximum > newColoring.range->minimum)
                    || (newColoring.logarithmic && !(newColoring.range->minimum > 0.0))) {
                    QMessageBox::warning(dialog, tr("Invalid color range"),
                        newColoring.logarithmic
                            ? tr("A log-scale range needs 0 < minimum < maximum.")
                            : tr("The color range needs minimum < maximum."));
                    return;
                }
            }
            std::vector<std::string> selectedSpecies;
            for (const auto& controls : *speciesControls) {
                if (controls.enabled->isChecked()) {
                    selectedSpecies.push_back(controls.name);
                }
                auto applied = controls.color;
                applied.setAlphaF(
                    static_cast<float>(controls.alpha->value()) / 100.0F);
                m_settings.colors[controls.name] = applied;
                m_settings.shapes[controls.name] = static_cast<MarkerShape>(
                    controls.shape->currentData().toInt());
            }
            // No check box below three dimensions; keep what is stored so
            // the filter is not silently cleared by a 2-D dataset's dialog.
            applySelection(std::move(selectedSpecies),
                fraction->value() / 100.0, pointSize->value(), seedValue,
                sliceCellsOnly != nullptr ? sliceCellsOnly->isChecked()
                                          : m_settings.sliceCellsOnly,
                std::move(newColoring));
            if (role == QDialogButtonBox::AcceptRole) {
                dialog->accept();
            }
        });
    layout->addWidget(buttons);
    // finished fires synchronously on Ok/Cancel/close, before the deferred
    // delete WA_DeleteOnClose schedules; forgetting the dialog here is what
    // lets the action reopen it immediately instead of raising a closing one.
    connect(dialog, &QDialog::finished, this, [this] { m_dialog = nullptr; });
    m_dialog = dialog;
    dialog->show();
}

} // namespace amrvis::qt
