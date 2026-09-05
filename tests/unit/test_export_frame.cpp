#include "ColorBarWidget.hpp"
#include "ExportFrame.hpp"

#include <QApplication>
#include <QTemporaryDir>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    using namespace amrvis;
    using namespace amrvis::qt;
    const RealBox region{Real3{{-2.0, 4.0, 10.0}}, Real3{{3.0, 8.0, 20.0}}};
    const auto xy = exportAxes(region, 2, 2, 0, SphericalDisplay::RZ, true, {});
    require(xy[0].label == "x" && xy[1].label == "y", "unset units added text");
    require(xy[0].minimum == -2.0 && xy[1].maximum == 8.0, "cropped bounds changed");
    for (int normal = 0; normal < 3; ++normal) {
        const auto axes = exportAxes(region, 3, normal, 0, SphericalDisplay::RZ, true, "cm");
        int next = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis != normal) {
                const auto& description = axes[static_cast<std::size_t>(next++)];
                require(description.minimum == region.lower[static_cast<std::size_t>(axis)] &&
                            description.maximum == region.upper[static_cast<std::size_t>(axis)] &&
                            description.label.endsWith(" (cm)"),
                        "3-D axes use the wrong coordinates");
            }
        }
    }
    for (const auto mode :
         {SphericalDisplay::RZ, SphericalDisplay::RTheta, SphericalDisplay::ThetaR}) {
        const auto axes = exportAxes(region, 2, 2, 2, mode, true, "km");
        if (mode == SphericalDisplay::RZ) {
            require(axes[0].label == "R (km)" && axes[1].label == "Z (km)", "R-Z labels wrong");
        } else {
            const std::size_t angular = mode == SphericalDisplay::ThetaR ? 0 : 1;
            require(axes[angular].label == QString(QChar(0x03b8)) + " (rad)" &&
                        axes[1 - angular].label == "r (km)",
                    "logical spherical labels wrong");
            const auto unset = exportAxes(region, 2, 2, 2, mode, true, {});
            require(unset[angular].label == QString(QChar(0x03b8)), "unset units added radians");
        }
        require(axes[0].minimum == region.lower[0], "display bounds were swapped twice");
    }
    const auto raw = exportAxes(region, 2, 2, 2, SphericalDisplay::ThetaR, false, "cm");
    require(raw[0].label == "x" && raw[1].label == "y", "raw FAB acquired physical units");

    ExportOptions options;
    options.includeAxes = true;
    options.transparentBackground = true;
    options.font = QFont(QStringLiteral("Sans Serif"));
    options.numberFormat = QStringLiteral("%.12f");
    const auto layout = makeExportLayout(QSize(600, 400), options);
    const auto larger = makeExportLayout(QSize(1200, 800), options);
    require(larger.font.pixelSize() > layout.font.pixelSize(), "publication text did not scale");
    const double points = layout.font.pixelSize() * 72.0 / (layout.dotsPerMeter * 0.0254);
    require(std::abs(points - 11.0) < 0.1, "PNG print resolution does not yield 11-point labels");
    require(exportAspectMatches(QSize(1200, 800), layout), "same-aspect resolution change refused");
    require(!exportAspectMatches(QSize(1200, 600), layout), "aspect-ratio change accepted");
    // Export dimensions round both scaled axes independently. In a portrait
    // image, half a pixel of width rounding can imply several pixels of height.
    for (const QSize source : {QSize(256, 1024), QSize(1024, 256), QSize(16, 1024)}) {
        for (const double scale : {1.0021, 1.1, 3.9}) {
            const QSize scaled(static_cast<int>(std::round(source.width() * scale)),
                               static_cast<int>(std::round(source.height() * scale)));
            const auto frozen = makeExportLayout(scaled, options);
            require(exportAspectMatches(source, frozen),
                    "independently rounded export dimensions refused unchanged source aspect");
            require(exportAspectMatches(source * 2, frozen),
                    "rounded export refused same-aspect resolution change");
            require(!exportAspectMatches(QSize(source.width(), source.height() / 2), frozen),
                    "rounded export accepted a changed source aspect");
        }
    }
    ExportLayout roundingBoundary;
    roundingBoundary.dataRect = QRect(0, 0, 257, 1026);
    require(exportAspectMatches(QSize(256, 1024), roundingBoundary),
            "portrait rounding example was rejected");
    roundingBoundary.dataRect.setHeight(1025);
    require(!exportAspectMatches(QSize(256, 1024), roundingBoundary),
            "aspect check accepted dimensions outside the half-pixel rounding budget");

    QImage raster(600, 400, QImage::Format_ARGB32_Premultiplied);
    raster.fill(QColor(35, 85, 130));
    raster.setPixelColor(120, 90, Qt::yellow);
    raster.setPixelColor(121, 90, QColor(90, 180, 30, 120));
    raster.setPixelColor(122, 90, Qt::transparent);
    ColorBarWidget bar;
    bar.setFont(layout.font);
    bar.setNumberFormat(options.numberFormat);
    QImage first;
    for (const double magnitude : {1.0, 1.0e-200, 1.0e200}) {
        auto axes = xy;
        axes[0].minimum = -2.0 * magnitude;
        axes[0].maximum = 3.0 * magnitude;
        bar.setFieldRange("density", -magnitude, magnitude);
        const auto frame = composeExportImage(raster, axes, options, layout, &bar);
        require(!frame.isNull() && frame.size() == layout.canvasSize, "frame dimensions changed");
        require(frame.copy(layout.dataRect) == raster, "axes moved or painted over data");
        require(frame.pixelColor(0, 0).alpha() == 0, "export surround is not transparent");
        require(frame.pixelColor(layout.colorBarRect.topLeft()).alpha() == 0,
                "exported color bar background is not transparent");
        require(frame.pixelColor(layout.dataRect.left() - 1, layout.dataRect.top()) ==
                    QColor(Qt::black),
                "vertical axis not outside the raster");
        if (first.isNull())
            first = frame;
        const QFontMetrics fm(layout.font);
        const auto label = exportNumber(magnitude, options.numberFormat, fm, layout.labelWidth);
        require(!label.isEmpty() && fm.horizontalAdvance(label) <= layout.labelWidth,
                "a numeric label overflowed its frozen budget");
    }
    const QFontMetrics fm(layout.font);
    ExportOptions compactOptions;
    compactOptions.includeAxes = true;
    compactOptions.font = options.font;
    ColorBarWidget compactBar;
    compactBar.setNumberFormat("%g");
    compactBar.setFieldRange("density", 0.0, 7.0);
    const auto compact = makeExportLayout(raster.size(), compactOptions, xy, &compactBar, false);
    const QFontMetrics compactMetrics(compact.font);
    const int compactGap = std::max(4, compactMetrics.height() / 4);
    require(compact.colorBarRect.left() - compact.dataRect.right() - 1 == compactGap,
            "x-axis label overhang added a color-bar gutter");
    require(compact.canvasSize.width() == compact.colorBarRect.right() + 1,
            "extra margin remains beyond the color bar");
    require(compact.verticalLabelWidth <= compactMetrics.horizontalAdvance("8"),
            "still export reserved unused scientific-notation space beside the y axis");
    require(compact.dataRect.left() < 5 * compactMetrics.height(),
            "vertical title is separated from short tick labels by a wide empty margin");
    require(ColorBarWidget::exportWidth(compactMetrics, compactMetrics.horizontalAdvance("7")) <
                ColorBarWidget::panelWidth,
            "export inherited the on-screen color bar minimum width");
    const auto movie = makeExportLayout(raster.size(), compactOptions, xy, &compactBar, true);
    const RealBox tallDomain{Real3{{0.0, 0.0, 0.0}}, Real3{{0.5, 0.5, 1.0}}};
    for (const bool animation : {false, true}) {
        const auto squareAxes = exportAxes(tallDomain, 3, 2, 0, SphericalDisplay::RZ, true, {});
        const auto square =
            makeExportLayout(QSize(540, 540), compactOptions, squareAxes, &compactBar, animation);
        require(square.font.pixelSize() ==
                    std::max(12, static_cast<int>(
                                     std::lround(square.canvasSize.width() * 11.0 / (72.0 * 7.0)))),
                "square XY export font sizing changed");
        for (int normal : {0, 1}) {
            const auto tallAxes =
                exportAxes(tallDomain, 3, normal, 0, SphericalDisplay::RZ, true, {});
            const auto tall =
                makeExportLayout(QSize(270, 540), compactOptions, tallAxes, &compactBar, animation);
            require(tall.font.pixelSize() == square.font.pixelSize(),
                    "narrow XZ/YZ export shrank labels relative to XY");
            require(tall.dataRect.size() == QSize(270, 540),
                    "enlarging portrait labels resized the data");
            require(tall.dotsPerMeter == square.dotsPerMeter,
                    "portrait and square exports use different print scales");
        }
    }
    const QFontMetrics movieMetrics(movie.font);
    for (const auto& label : {QStringLiteral("-9e-308"), QStringLiteral("-9e+308")}) {
        require(movie.verticalLabelWidth >= movieMetrics.horizontalAdvance(label),
                "compact movie layout cannot fit scientific notation");
    }
    const auto ticks = exportTicks({"y", -2.0, 2.0}, 400, 40, "%g", fm, layout.labelWidth);
    require(ticks.size() >= 3 && ticks.front().fraction == 0.0 && ticks.back().fraction == 1.0,
            "axis endpoints do not map bottom-to-top");
    for (const auto& bounds : {std::pair{-1e308, 1e308}, std::pair{0.0, 1e-320}}) {
        const auto extreme =
            exportTicks({"x", bounds.first, bounds.second}, 400, 100, "%g", fm, layout.labelWidth);
        require(!extreme.empty(), "extreme finite bounds lost every tick");
        for (const auto& tick : extreme) {
            require(std::isfinite(tick.fraction) && tick.fraction >= 0.0 && tick.fraction <= 1.0,
                    "a tick has an invalid position");
        }
    }
    const auto savedPalette = app.palette();
    QPalette hostile;
    hostile.setColor(QPalette::Window, Qt::black);
    hostile.setColor(QPalette::WindowText, Qt::white);
    app.setPalette(hostile);
    bar.setFieldRange("density", -1.0, 1.0);
    require(composeExportImage(raster, xy, options, layout, &bar) == first,
            "the application palette changed publication styling");
    app.setPalette(savedPalette);
    options.transparentBackground = false;
    const auto white = composeExportImage(raster, xy, options, layout, &bar);
    require(white.pixelColor(0, 0) == QColor(Qt::white) &&
                white.pixelColor(layout.colorBarRect.topLeft()) == QColor(Qt::white),
            "white background selection left transparent margins");
    const auto checkWhiteRaster = [&](const QImage& image) {
        require(image.pixelColor(120, 90) == QColor(Qt::yellow),
                "white background changed an opaque data pixel");
        require(image.pixelColor(122, 90) == QColor(Qt::white),
                "white background left a transparent hole in the raster");
        const auto blended = image.pixelColor(121, 90);
        require(blended.alpha() == 255 && std::abs(blended.red() - 177) <= 1 &&
                    std::abs(blended.green() - 220) <= 1 &&
                    std::abs(blended.blue() - 149) <= 1,
                "partially transparent data was not blended onto white");
    };
    checkWhiteRaster(white.copy(layout.dataRect));
    options.includeAxes = false;
    options.includeColorBar = false;
    const auto plain = makeExportLayout(raster.size(), options);
    checkWhiteRaster(composeExportImage(raster, xy, options, plain, nullptr));
    options.transparentBackground = true;
    require(composeExportImage(raster, xy, options, plain, nullptr) == raster,
            "transparent unannotated export changed the raster");
    if (argc == 2) {
        const auto preview = composeExportImage(raster, xy, compactOptions, compact, &compactBar);
        require(preview.save(QString::fromLocal8Bit(argv[1])), "could not save preview");
    }
    std::cout << "export frame tests passed\n";
}
