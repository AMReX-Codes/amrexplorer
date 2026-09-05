#pragma once

#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QRect>
#include <QString>
#include <amrexplorer/core/CoordinateSystem.hpp>
#include <array>
#include <vector>

namespace amrvis::qt {
class ColorBarWidget;

struct ExportOptions {
    bool includeColorBar = true;
    bool includeAxes = false;
    bool transparentBackground = false;
    QFont font;
    QString numberFormat = QStringLiteral("%g");
    QString lengthUnit;
};

struct ExportAxis {
    QString label;
    double minimum = 0.0;
    double maximum = 1.0;
};

struct ExportTick {
    double fraction;
    QString label;
};

// Immutable for an animation panel after its first exported frame.
struct ExportLayout {
    QSize canvasSize;
    QRect dataRect;
    QRect colorBarRect;
    QFont font;
    int labelWidth = 0;
    int verticalLabelWidth = 0;
    int dotsPerMeter = 0;
};

[[nodiscard]] std::array<ExportAxis, 2>
exportAxes(const RealBox& displayRegion, int dimension, int normal, int coordinateSystem,
           SphericalDisplay spherical, bool hasPhysicalGeometry, const QString& lengthUnit);
[[nodiscard]] QString exportNumber(double value, const QString& format, const QFontMetrics& metrics,
                                   int availableWidth);
[[nodiscard]] std::vector<ExportTick> exportTicks(const ExportAxis& axis, int pixelLength,
                                                  int labelSpacing, const QString& format,
                                                  const QFontMetrics& metrics, int labelWidth);
// Measure the first frame's labels. Movies reserve a small additional budget
// for changing magnitudes; stills can disable it for the tightest margins.
[[nodiscard]] ExportLayout makeExportLayout(QSize rasterSize, const ExportOptions& options,
                                            const std::array<ExportAxis, 2>& axes = {},
                                            const ColorBarWidget* colorBar = nullptr,
                                            bool reserveLabelGrowth = true);
[[nodiscard]] bool exportAspectMatches(QSize rasterSize, const ExportLayout& layout);
[[nodiscard]] QImage composeExportImage(const QImage& raster, const std::array<ExportAxis, 2>& axes,
                                        const ExportOptions& options, const ExportLayout& layout,
                                        const ColorBarWidget* colorBar);
} // namespace amrvis::qt
