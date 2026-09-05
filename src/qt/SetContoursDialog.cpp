#include "SetContoursDialog.hpp"

#include "Theme.hpp"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <array>
#include <cctype>

namespace amrvis::qt {

std::tuple<int, int, int> detectVectorFields(
    const std::vector<std::string>& fieldNames)
{
    const auto containsIgnoreCase = [&fieldNames](const char* needle) {
        for (std::size_t index = 0; index < fieldNames.size(); ++index) {
            std::string lowered = fieldNames[index];
            for (auto& character : lowered) {
                character = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(character)));
            }
            if (lowered.find(needle) != std::string::npos) {
                return static_cast<int>(index);
            }
        }
        return -1;
    };
    auto uField = containsIgnoreCase("x_velocity");
    if (uField < 0) uField = containsIgnoreCase("x-velocity");
    if (uField < 0) uField = containsIgnoreCase("velx");
    if (uField < 0) uField = containsIgnoreCase("x_vel");
    if (uField < 0) uField = containsIgnoreCase("xvel");
    if (uField < 0) uField = containsIgnoreCase("v_x");
    if (uField < 0) uField = containsIgnoreCase("vx");
    if (uField < 0) uField = containsIgnoreCase("u");
    auto vField = containsIgnoreCase("y_velocity");
    if (vField < 0) vField = containsIgnoreCase("y-velocity");
    if (vField < 0) vField = containsIgnoreCase("vely");
    if (vField < 0) vField = containsIgnoreCase("y_vel");
    if (vField < 0) vField = containsIgnoreCase("yvel");
    if (vField < 0) vField = containsIgnoreCase("v_y");
    if (vField < 0) vField = containsIgnoreCase("vy");
    if (vField < 0) vField = containsIgnoreCase("v");
    auto wField = containsIgnoreCase("z_velocity");
    if (wField < 0) wField = containsIgnoreCase("z-velocity");
    if (wField < 0) wField = containsIgnoreCase("velz");
    if (wField < 0) wField = containsIgnoreCase("z_vel");
    if (wField < 0) wField = containsIgnoreCase("zvel");
    if (wField < 0) wField = containsIgnoreCase("v_z");
    if (wField < 0) wField = containsIgnoreCase("vz");
    if (wField < 0) wField = containsIgnoreCase("w");
    if (fieldNames.empty()) {
        return {0, 0, 0};
    }
    const auto last = static_cast<int>(fieldNames.size()) - 1;
    if (uField < 0) {
        uField = 0;
    }
    if (vField < 0) {
        vField = last < 1 ? last : 1;
    }
    if (wField < 0) {
        wField = last < 2 ? last : 2;
    }
    return {uField, vField, wField};
}

SetContoursDialog::SetContoursDialog(const std::vector<std::string>& fieldNames,
    bool is3D, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("setContoursDialog"));
    setWindowTitle(tr("Set Contours"));
    setWindowFlags(Qt::Window);

    auto* layout = new QVBoxLayout(this);

    auto* modeBox = new QGroupBox(tr("Display mode"), this);
    auto* modeLayout = new QVBoxLayout(modeBox);
    m_modeButtons = new QButtonGroup(this);
    constexpr std::array<const char*, 3> modeLabels{
        "Raster", "Raster && Contours", "Velocity Vectors"};
    for (std::size_t index = 0; index < modeLabels.size(); ++index) {
        auto* button = new QRadioButton(tr(modeLabels[index]), modeBox);
        m_modeButtons->addButton(button, static_cast<int>(index));
        modeLayout->addWidget(button);
    }
    layout->addWidget(modeBox);

    auto* countRow = new QHBoxLayout;
    countRow->addWidget(new QLabel(tr("Number of lines:"), this));
    m_contourCount = new QSpinBox(this);
    m_contourCount->setRange(1, 99);
    m_contourCount->setValue(10);
    countRow->addWidget(m_contourCount);
    countRow->addStretch();
    layout->addLayout(countRow);

    auto* colorRow = new QHBoxLayout;
    colorRow->addWidget(new QLabel(tr("Contour color:"), this));
    m_contourColorCombo = new QComboBox(this);
    m_contourColorCombo->addItem(tr("Black"), contourColorBlack);
    m_contourColorCombo->addItem(tr("White"), contourColorWhite);
    m_contourColorCombo->addItem(tr("Palette index"), 0);
    colorRow->addWidget(m_contourColorCombo);
    m_colorIndex = new QSpinBox(this);
    m_colorIndex->setRange(0, 255);
    m_colorIndex->setValue(0);
    m_colorIndex->setEnabled(false);
    colorRow->addWidget(m_colorIndex);
    colorRow->addStretch();
    layout->addLayout(colorRow);
    connect(m_contourColorCombo, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int index) {
            m_colorIndex->setEnabled(index == 2);
        });

    m_vectorBox = new QGroupBox(tr("Velocity vector fields"), this);
    auto* vectorLayout = new QFormLayout(m_vectorBox);
    m_uField = new QComboBox(m_vectorBox);
    m_vField = new QComboBox(m_vectorBox);
    for (const auto& name : fieldNames) {
        const auto label = QString::fromStdString(name);
        m_uField->addItem(label);
        m_vField->addItem(label);
    }
    vectorLayout->addRow(tr("U field:"), m_uField);
    vectorLayout->addRow(tr("V field:"), m_vField);
    if (is3D) {
        m_wField = new QComboBox(m_vectorBox);
        for (const auto& name : fieldNames) {
            m_wField->addItem(QString::fromStdString(name));
        }
        vectorLayout->addRow(tr("W field:"), m_wField);
    }
    auto* vectorWarning = new QLabel(
        tr("U and V fields must be different"), m_vectorBox);
    vectorWarning->setStyleSheet(
        QStringLiteral("QLabel { color: %1; }").arg(errorTextColor().name()));
    vectorWarning->setVisible(false);
    vectorLayout->addRow(vectorWarning);
    const auto checkVectorFields = [this, vectorWarning] {
        const bool conflict = m_uField->currentIndex() == m_vField->currentIndex();
        vectorWarning->setVisible(conflict);
    };
    connect(m_uField, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [checkVectorFields](int) { checkVectorFields(); });
    connect(m_vField, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [checkVectorFields](int) { checkVectorFields(); });
    layout->addWidget(m_vectorBox);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok
        | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::clicked, this,
        [this, buttons, vectorWarning](QAbstractButton* button) {
            const auto role = buttons->buttonRole(button);
            if (role == QDialogButtonBox::AcceptRole
                || role == QDialogButtonBox::ApplyRole) {
                if (m_uField->currentIndex() == m_vField->currentIndex()
                    && m_mode == DisplayMode::VelocityVectors
                    && m_uField->count() > 1) {
                    vectorWarning->setVisible(true);
                    return;
                }
                emit applied();
                if (role == QDialogButtonBox::AcceptRole) {
                    accept();
                }
            } else {
                reject();
            }
        });
    layout->addWidget(buttons);

    connect(m_modeButtons, &QButtonGroup::idClicked, this, [this](int id) {
        m_mode = static_cast<DisplayMode>(id);
        m_vectorBox->setEnabled(id == static_cast<int>(DisplayMode::VelocityVectors));
    });

    const auto [uField, vField, wField] = detectVectorFields(fieldNames);
    setMode(DisplayMode::Raster);
    setVectorFields(uField, vField, wField);
}

void SetContoursDialog::setMode(DisplayMode mode)
{
    m_mode = mode;
    auto* button = m_modeButtons->button(static_cast<int>(mode));
    if (button != nullptr) {
        button->setChecked(true);
    }
    m_vectorBox->setEnabled(mode == DisplayMode::VelocityVectors);
}

void SetContoursDialog::setContourCount(int count)
{
    m_contourCount->setValue(count);
}

void SetContoursDialog::setVectorFields(int uField, int vField, int wField)
{
    if (uField >= 0 && uField < m_uField->count()) {
        m_uField->setCurrentIndex(uField);
    }
    if (vField >= 0 && vField < m_vField->count()) {
        m_vField->setCurrentIndex(vField);
    }
    if (m_wField != nullptr && wField >= 0 && wField < m_wField->count()) {
        m_wField->setCurrentIndex(wField);
    }
}

DisplayMode SetContoursDialog::mode() const
{
    return m_mode;
}

int SetContoursDialog::contourCount() const
{
    return m_contourCount->value();
}

int SetContoursDialog::uField() const
{
    return m_uField->currentIndex();
}

int SetContoursDialog::vField() const
{
    return m_vField->currentIndex();
}

int SetContoursDialog::wField() const
{
    return m_wField != nullptr ? m_wField->currentIndex() : 0;
}

void SetContoursDialog::setContourColor(int color)
{
    if (color == contourColorWhite) {
        m_contourColorCombo->setCurrentIndex(1);
    } else if (color >= 0) {
        m_contourColorCombo->setCurrentIndex(2);
        m_colorIndex->setValue(color);
    } else {
        m_contourColorCombo->setCurrentIndex(0);
    }
}

int SetContoursDialog::contourColor() const
{
    if (m_contourColorCombo->currentIndex() == 0) return contourColorBlack;
    if (m_contourColorCombo->currentIndex() == 1) return contourColorWhite;
    return m_colorIndex->value();
}

} // namespace amrvis::qt
