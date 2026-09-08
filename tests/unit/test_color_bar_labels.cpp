// The color bar's tick labelling: when a narrow range gets a matplotlib-style
// offset factored out of its ticks, and when it prints values in full.
#include "ColorBarWidget.hpp"
#include "NumberFormat.hpp"

#include <QApplication>
#include <QString>

#include <cstdlib>
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

    return 0;
}
