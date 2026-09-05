#pragma once

#include <amrexplorer/pipeline/DisplayMode.hpp>

#include <QDialog>

#include <string>
#include <tuple>
#include <utility>
#include <vector>

class QButtonGroup;
class QComboBox;
class QEvent;
class QGroupBox;
class QLabel;
class QSpinBox;

namespace amrvis::qt {

// DisplayMode lives in the Qt-free pipeline layer; re-export it here so
// amrvis::qt::DisplayMode keeps resolving for the GUI and tests.
using amrvis::DisplayMode;

// Contour color selection: black, white, or a specific palette index.
// Stored as an int: -1 = black (default), -2 = white, 0–255 = palette slot.
constexpr int contourColorBlack = -1;
constexpr int contourColorWhite = -2;

// Legacy vector-field defaults: a case-insensitive match for velocity
// component names.  Returns (u, v, w) indices suitable for both 2-D
// (u,v) and 3-D (u,v,w) vector display.
[[nodiscard]] std::tuple<int, int, int> detectVectorFields(
    const std::vector<std::string>& fieldNames);

class SetContoursDialog final : public QDialog {
    Q_OBJECT

public:
    explicit SetContoursDialog(const std::vector<std::string>& fieldNames,
        bool is3D, QWidget* parent = nullptr);

    void setMode(DisplayMode mode);
    void setContourCount(int count);
    void setVectorFields(int uField, int vField, int wField);
    void setContourColor(int color);
    [[nodiscard]] DisplayMode mode() const;
    [[nodiscard]] int contourCount() const;
    [[nodiscard]] int uField() const;
    [[nodiscard]] int vField() const;
    [[nodiscard]] int wField() const;
    [[nodiscard]] int contourColor() const;

signals:
    void applied();

protected:
    void changeEvent(QEvent* event) override;

private:
    void updateWarningColor();
    DisplayMode m_mode = DisplayMode::Raster;
    QButtonGroup* m_modeButtons = nullptr;
    QSpinBox* m_contourCount = nullptr;
    QGroupBox* m_vectorBox = nullptr;
    QLabel* m_vectorWarning = nullptr;
    QComboBox* m_uField = nullptr;
    QComboBox* m_vField = nullptr;
    QComboBox* m_wField = nullptr;
    QComboBox* m_contourColorCombo = nullptr;
    QSpinBox* m_colorIndex = nullptr;
};

} // namespace amrvis::qt
