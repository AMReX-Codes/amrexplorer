// The color bar's tick labelling: when a narrow range gets a matplotlib-style
// offset factored out of its ticks, and when it prints values in full.
#include "ColorBarWidget.hpp"
#include "NumberFormat.hpp"
#include "RecordingPaintDevice.hpp"

#include <QApplication>
#include <QString>

#include <cstdlib>
#include <cmath>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    [[maybe_unused]] QApplication application(argc, argv);
    using namespace amrvis::qt;

    // The reported field: two values 1e-17 apart at 1.2566e-06.
    constexpr double narrowLow = 1.2566370621199999e-06;
    constexpr double narrowHigh = 1.25663706213e-06;

    ColorBarWidget bar;
    bar.setFieldRange(QStringLiteral("mu"), narrowLow, narrowHigh);
    const auto offset = bar.labelOffset();
    require(offset != 0.0, "a narrow range did not get an offset");
    require(offset <= narrowLow,
        "the offset is above the range, so a residual would be negative");
    require(narrowHigh - offset < 1.0e-16,
        "the offset left more than the span behind");
    // The residuals are small numbers, so they need no extra digits -- which
    // is the point of factoring the magnitude out.
    require(formatDigits(bar.tickFormat()) == minimumDisplayDigits,
        "residual labels asked for extra digits");
    // The offset itself must carry enough digits to be worth printing.
    require(formatDigits(bar.effectiveFormat()) > minimumDisplayDigits,
        "the offset label would print at the default precision");
    require(formatNumber(narrowLow - offset, bar.tickFormat())
            != formatNumber(narrowHigh - offset, bar.tickFormat()),
        "the two endpoints render as the same residual");

    // An ordinary range is left alone.
    bar.setFieldRange(QStringLiteral("rho"), 0.0, 1.0);
    require(bar.labelOffset() == 0.0, "an ordinary range got an offset");
    bar.setFieldRange(QStringLiteral("rho"), 1.0, 2.0);
    require(bar.labelOffset() == 0.0,
        "a range spanning its own magnitude got an offset");

    // Straddling zero there is no shared leading part to factor out.
    bar.setFieldRange(QStringLiteral("v"), -1.0e-12, 1.0e-12);
    require(bar.labelOffset() == 0.0, "a range straddling zero got an offset");

    // A degenerate range has nothing to separate.
    bar.setFieldRange(QStringLiteral("k"), 2.5, 2.5);
    require(bar.labelOffset() == 0.0, "a zero-span range got an offset");

    // Log ticks are decades apart, so they share no leading part.
    bar.setLogarithmic(true);
    bar.setFieldRange(QStringLiteral("mu"), narrowLow, narrowHigh);
    require(bar.labelOffset() == 0.0, "a logarithmic range got an offset");
    bar.setLogarithmic(false);

    // A negative narrow range offsets too, and downward.
    bar.setFieldRange(QStringLiteral("phi"), -narrowHigh, -narrowLow);
    const auto negativeOffset = bar.labelOffset();
    require(negativeOffset != 0.0, "a negative narrow range got no offset");
    require(negativeOffset <= -narrowHigh,
        "the negative offset would make a residual negative");

    // Offsets must reconstruct the values actually printed. Explicit coarse
    // formats disable an offset whose discarded digits exceed the range.
    bar.setFieldRange("mu", narrowLow, narrowHigh);
    for (const auto& format : {QString("%g"), QString("%.6g"), QString("%.17g"), QString("%+.17g")}) {
        bar.setNumberFormat(format);
        const auto actualOffset = bar.labelOffset();
        if (actualOffset != 0.0) {
            require(formatNumber(actualOffset, bar.effectiveFormat()).toDouble() == actualOffset,
                "ticks subtract an offset different from the printed one");
        } else {
            require(format == "%.6g", "adaptive/full precision lost a usable offset");
        }
    }

    // Use the real paint path at the exported width. Cover both the offset
    // line and the short-panel fallback, plus fixed/exponential notation.
    QFont font;
    font.setPixelSize(18);
    const QFontMetrics metrics(font);
    for (const auto& format : {QString("%g"), QString("%.6g"), QString("%.17g"),
             QString("%.2f"), QString("%.6e")}) {
        bar.setNumberFormat(format);
        for (const int height : {60, 100, 480}) {
            for (const auto& bounds : {std::pair{narrowLow, narrowHigh},
                     std::pair{12345678.9, 22345678.9}, std::pair{1e200, 2e200}}) {
                bar.setFieldRange("field", bounds.first, bounds.second);
                const auto width = ColorBarWidget::exportWidth(metrics,
                    bar.exportLabelWidth(metrics, metrics.horizontalAdvance("-1.234567e-308"), height));
                RecordingPaintDevice device(width, height);
                QPainter painter(&device);
                painter.setFont(font);
                bar.paintBar(&painter, QRect(0, 0, width, height), true, true);
                painter.end();
                int numericLabels = 0;
                for (const auto& label : device.text()) {
                    bool numeric = false;
                    label.value.toDouble(&numeric);
                    if (!numeric) { continue; }
                    ++numericLabels;
                    require(label.bounds.left() >= -0.5 && label.bounds.right() <= width + 0.5,
                        "export clipped a numeric tick or offset label");
                }
                require(numericLabels > 0, "export lost every numeric label");
            }
        }
    }

    return 0;
}
