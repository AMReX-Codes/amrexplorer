#include "ColorBarWidget.hpp"
#include "ExportFrame.hpp"
#include "NumberFormat.hpp"
#include "RecordingPaintDevice.hpp"

#include <QApplication>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <utility>

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
    // A stretched footprint capped to a one-pixel axis is the size floor, not
    // a rounding: the same footprint must pass on the next frame, and a
    // raster that would not round down to one pixel must not.
    ExportLayout hairline;
    hairline.dataRect = QRect(0, 0, 1, 8192);
    require(exportAspectMatches(QSizeF(64.0, 6553600.0), hairline),
            "a one-pixel export axis was rejected as an aspect change");
    require(!exportAspectMatches(QSizeF(64.0, 64.0), hairline),
            "a square raster matched a one-pixel export axis");

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
    // The actual label calculation must stay finite over the full double
    // range, with exact endpoints and geometric spacing in between.
    for (const auto& bounds : {std::pair{1.0e-200, 1.0e200},
             std::pair{std::numeric_limits<double>::denorm_min(),
                 std::numeric_limits<double>::max()}}) {
        require(ColorBarWidget::tickValue(bounds.first, bounds.second, true, 0.0)
                    == bounds.second
                && ColorBarWidget::tickValue(bounds.first, bounds.second, true, 1.0)
                    == bounds.first,
            "logarithmic color bar lost its endpoints");
        double previous = bounds.second;
        for (int label = 0; label < 8; ++label) {
            const auto fraction = static_cast<double>(label) / 7.0;
            const auto value = ColorBarWidget::tickValue(
                bounds.first, bounds.second, true, fraction);
            require(std::isfinite(value) && value >= bounds.first && value <= previous,
                "logarithmic color bar has a nonfinite or unordered label");
            previous = value;
        }
    }
    require(std::abs(ColorBarWidget::tickValue(1.0e-200, 1.0e200, true, 0.5) - 1.0)
                < 1.0e-12,
        "logarithmic color bar lost geometric spacing");
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
        // Annotations wider than the data grow the canvas but not the font,
        // so the reference width stops at twice the data's longer side.
        const int dataLength = std::max(square.dataRect.width(), square.dataRect.height());
        const int referenceWidth = std::min(square.canvasSize.width(), 2 * dataLength);
        require(square.font.pixelSize() ==
                    std::max(12, static_cast<int>(
                                     std::lround(referenceWidth * 11.0 / (72.0 * 7.0)))),
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
    // Spatial precision follows each displayed coordinate range, independently
    // of the scalar field and of the other coordinate axis.
    constexpr double narrowLow = 1.25663706212e-6;
    constexpr double narrowHigh = 1.25663706213e-6;
    ColorBarWidget adaptiveBar;
    adaptiveBar.setFieldRange("field", narrowLow, narrowHigh);
    const std::array<ExportAxis, 2> mixedAxes{{{"x", 0.133333333333333, 1.0},
        {"y", narrowLow, narrowHigh}}};
    const auto adaptive = makeExportLayout(raster.size(), compactOptions, mixedAxes,
        &adaptiveBar);
    require(adaptive.axisFormats[0] == "%.6g"
            && formatDigits(adaptive.axisFormats[1]) > minimumDisplayDigits,
        "export axes did not resolve precision independently");
    require(exportNumber(0.133333333333333, adaptive.axisFormats[0],
                QFontMetrics(adaptive.font), adaptive.labelWidth) == "0.133333",
        "a narrow field or y range added noise digits to the x axis");
    auto explicitOptions = compactOptions;
    explicitOptions.numberFormat = "%.8g";
    const auto explicitLayout = makeExportLayout(raster.size(), explicitOptions, mixedAxes);
    require(explicitLayout.axisFormats[0] == "%.8g"
            && explicitLayout.axisFormats[1] == "%.8g",
        "export overrode explicitly requested axis precision");

    // Freeze residual precision and the offset line's presence. Later frames
    // span ordinary ranges and huge exponents but still print all six digits,
    // inside the first frame's rectangle.
    require(adaptive.colorBarPresentation.has_value(), "color bar notation was not frozen");
    const auto& presentation = *adaptive.colorBarPresentation;
    require(presentation.offsetLine && presentation.tickFormat == "%.6g",
        "first-frame residuals inherited full-value precision");
    for (double magnitude : {1.23456789, 1.23456789e-200, 1.23456789e200}) {
        adaptiveBar.setFieldRange("field", -magnitude, magnitude);
        RecordingPaintDevice device(adaptive.canvasSize.width(), adaptive.canvasSize.height());
        QPainter painter(&device);
        painter.setFont(adaptive.font);
        adaptiveBar.paintBar(&painter, adaptive.colorBarRect, true, true, &presentation);
        painter.end();
        require(device.text().size() >= 4, "a frozen color bar lost its offset or tick labels");
        const auto tickCount = device.text().size() - 2;
        require(device.text()[1].value == "+0",
            "an ordinary frame lost its reserved offset line");
        for (std::size_t tick = 0; tick < tickCount; ++tick) {
            const double value = std::lerp(magnitude, -magnitude,
                static_cast<double>(tick) / static_cast<double>(tickCount - 1));
            require(device.text()[tick + 2].value == QString::number(value, 'g', 6),
                "animation narrowed ticks below its frozen precision");
            require(QRectF(adaptive.colorBarRect).adjusted(-1, -1, 1, 1)
                    .contains(device.text()[tick + 2].bounds),
                "animation tick text escaped its frozen color bar");
        }
        QImage expected(adaptive.canvasSize, QImage::Format_ARGB32_Premultiplied);
        expected.fill(Qt::transparent);
        QPainter expectedPainter(&expected);
        expectedPainter.setFont(adaptive.font);
        adaptiveBar.paintBar(&expectedPainter, adaptive.colorBarRect, true, true, &presentation);
        expectedPainter.end();
        auto transparentOptions = compactOptions;
        transparentOptions.transparentBackground = true;
        const auto frame = composeExportImage(raster, mixedAxes, transparentOptions,
            adaptive, &adaptiveBar);
        require(frame.copy(adaptive.colorBarRect) == expected.copy(adaptive.colorBarRect),
            "frame composition did not use its frozen color bar presentation");
    }
    // A rounded offset can lie just above a later narrow range and become
    // unusable. Keep the reserved line, but print full values with enough
    // digits to distinguish the ticks instead of using residual precision.
    adaptiveBar.setFieldRange("field", narrowLow, narrowHigh);
    const auto narrowMovie = makeExportLayout(raster.size(), compactOptions,
        mixedAxes, &adaptiveBar, true);
    constexpr double laterLow = 1.3839999999999999e-11;
    constexpr double laterHigh = 1.3840000000138398e-11;
    adaptiveBar.setFieldRange("field", laterLow, laterHigh);
    require(adaptiveBar.labelOffset() == 0.0,
        "the later range no longer exercises an unusable offset");
    RecordingPaintDevice laterDevice(narrowMovie.canvasSize.width(), narrowMovie.canvasSize.height());
    QPainter laterPainter(&laterDevice);
    laterPainter.setFont(narrowMovie.font);
    adaptiveBar.paintBar(&laterPainter, narrowMovie.colorBarRect, true, true,
        &*narrowMovie.colorBarPresentation);
    laterPainter.end();
    require(laterDevice.text().size() >= 4 && laterDevice.text()[1].value == "+0",
        "a narrow animation frame lost its reserved offset line or ticks");
    const auto& valueFormat = narrowMovie.colorBarPresentation->valueFormat;
    const auto laterTickCount = laterDevice.text().size() - 2;
    for (std::size_t tick = 0; tick < laterTickCount; ++tick) {
        const auto value = ColorBarWidget::tickValue(laterLow, laterHigh, false,
            static_cast<double>(tick) / static_cast<double>(laterTickCount - 1));
        const auto& label = laterDevice.text()[tick + 2];
        require(label.value == formatNumber(value, valueFormat),
            "a narrow animation frame without an offset lost full-value precision");
        require(tick == 0 || label.value != laterDevice.text()[tick + 1].value,
            "a narrow animation frame has identical adjacent tick labels");
        require(QRectF(narrowMovie.colorBarRect).adjusted(-1, -1, 1, 1).contains(label.bounds),
            "a full-value animation tick escaped its frozen color bar");
    }

    // An ordinary first frame must not acquire an offset line later.
    adaptiveBar.setFieldRange("field", 0.0, 1.0);
    const auto ordinary = makeExportLayout(raster.size(), compactOptions, xy, &adaptiveBar);
    adaptiveBar.setFieldRange("field", narrowLow, narrowHigh);
    RecordingPaintDevice ordinaryDevice(ordinary.canvasSize.width(), ordinary.canvasSize.height());
    QPainter ordinaryPainter(&ordinaryDevice);
    ordinaryPainter.setFont(ordinary.font);
    adaptiveBar.paintBar(&ordinaryPainter, ordinary.colorBarRect, true, true,
        &*ordinary.colorBarPresentation);
    ordinaryPainter.end();
    require(ordinaryDevice.text().size() == 9,
        "a later narrow range added an offset line to an ordinary movie");

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
    // Fixed/exponential notation must retain ticks when compact notation is
    // needed, and enormous precision fields must not inflate the layout.
    for (const auto& format : {QString("%.2f"), QString("%.6e"),
             QString("%.2000g"), QString("%.99999999999g")}) {
        auto fixedOptions = options;
        fixedOptions.numberFormat = format;
        const std::array<ExportAxis, 2> largeAxes{{{"x", 12345678.9, 22345678.9},
            {"y", -1e200, 1e200}}};
        const auto fixed = makeExportLayout(QSize(600, 400), fixedOptions, largeAxes);
        require(fixed.canvasSize.width() < 4096 && fixed.font.pixelSize() < 128,
            "an oversized precision made the export layout unbounded");
        const QFontMetrics fixedMetrics(fixed.font);
        for (const auto& axis : largeAxes) {
            const auto labels = exportTicks(axis, 400, 100, format, fixedMetrics,
                fixed.labelWidth);
            require(!labels.empty(), "fixed/exponential format erased the export ticks");
            for (const auto& label : labels) {
                require(fixedMetrics.horizontalAdvance(label.label) <= fixed.labelWidth,
                    "an exported tick overflowed the frozen label budget");
            }
        }
    }

    // Font-dependent label widths must not feed back into ever-larger fonts.
    // DejaVu Sans reproduces the Linux CI failure; stretching also exercises
    // wide glyphs on systems where that family is unavailable.
    for (const auto& family : {QString("Sans Serif"), QString("DejaVu Sans")}) {
        for (const int stretch : {100, 150}) {
            auto wideOptions = options;
            wideOptions.font = QFont(family);
            wideOptions.font.setStretch(stretch);
            for (const auto& format : {QString("%g"), QString("%.17g"),
                     QString("%.2000g")}) {
                wideOptions.numberFormat = format;
                const std::array<ExportAxis, 2> narrowAxes{{{"x", narrowLow, narrowHigh},
                    {"y", narrowLow, narrowHigh}}};
                const auto wide = makeExportLayout(QSize(600, 400), wideOptions,
                    narrowAxes, &adaptiveBar, true);
                require(wide.font.pixelSize() < 40 && wide.canvasSize.width() < 4096,
                    "wide high-precision labels inflated the export font or canvas");
                require(wide.dataRect.size() == QSize(600, 400),
                    "bounding annotation size changed the exported data dimensions");
                require(formatDigits(wide.axisFormats[0]) >= 15,
                    "bounding annotation size discarded the requested axis precision");
            }
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
