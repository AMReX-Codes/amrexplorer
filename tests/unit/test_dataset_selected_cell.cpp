// Coverage for DatasetValueDelegate: a selected cell must keep the colors the
// model gives it. The default QStyledItemDelegate paints selected text in
// HighlightedText over a Highlight fill, which wipes out both the value's
// color-bar color and the no-data shade behind an uncovered sample -- and
// selecting is this window's ordinary interaction.
//
// Rendered rather than reasoned about: each case paints a real cell through
// the real delegate and counts pixels, which is the only way to see what the
// style actually did. The stock delegate is rendered beside it so the test
// states the regression it is guarding, not just the fix.

#include "DatasetValueDelegate.hpp"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

const QColor valueColor(0xE0, 0x20, 0x20);
const QColor noDataColor(0x30, 0x30, 0x30);
const QColor cellBase(0x88, 0x88, 0x88);
const QColor selectionColor(0x30, 0x60, 0xD0);
const QColor selectedTextColor(0xFF, 0xFF, 0xFF);

// Largest per-channel difference: a coarse "are these two colors visibly
// apart" measure, which is all the assertions below need.
int channelDistance(QRgb left, QRgb right)
{
    return std::max({std::abs(qRed(left) - qRed(right)),
        std::abs(qGreen(left) - qGreen(right)),
        std::abs(qBlue(left) - qBlue(right))});
}

// How far a pixel may sit from the value's color and still count as that
// value's: a style may draw a translucent focus rect over the current cell,
// which shifts the digits without repainting them. Fusion's wash moves the
// nearest pixel exactly 24 away, and a delegate that had lost the color
// instead lands at 127 (Windows) or 172 (Fusion), so this sits in the middle
// of [24, 127] rather than on either edge. The 24 is a property of the wash's
// alpha over an unantialiased glyph, not of the font: it is the same across
// every family, which only changes how many pixels are that close.
constexpr int focusWashTolerance = 40;

struct Render {
    // Pixels drawn in exactly the value's color: the digits.
    int valuePixels = 0;
    // Pixels near enough to it to still be reading as that value.
    int nearValuePixels = 0;
    // Pixels drawn in exactly the theme's selected-text color, and pixels
    // near enough to it to be that text under a focus wash.
    int selectedTextPixels = 0;
    int nearSelectedTextPixels = 0;
    // A corner, which no glyph reaches: the cell's background.
    QRgb background = 0;
};

enum class Selected : std::uint8_t { No, Yes };
// The clicked cell is also the view's current cell, so State_HasFocus rides
// along with State_Selected on every click -- the combination that actually
// occurs, and the one a style's focus rect reacts to.
enum class Current : std::uint8_t { No, Yes };

Render render(const QStyledItemDelegate& delegate, const QModelIndex& index,
    Selected selected, Current current = Current::No)
{
    QImage image(80, 24, QImage::Format_ARGB32_Premultiplied);
    // The view paints its viewport in Base and the delegate paints the item
    // on top, leaving that showing where it fills nothing; start from Base so
    // the corner below reads what the cell would really sit on.
    image.fill(cellBase);
    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, 80, 24);
    option.displayAlignment = Qt::AlignRight | Qt::AlignVCenter;
    option.features = QStyleOptionViewItem::HasDisplay;
    option.state = QStyle::State_Enabled;
    option.palette.setColor(QPalette::Base, cellBase);
    option.palette.setColor(QPalette::Highlight, selectionColor);
    option.palette.setColor(QPalette::HighlightedText, selectedTextColor);
    option.palette.setColor(QPalette::Text, Qt::black);
    // Pinned, because the counts below are of pixels in *exactly* the value's
    // color and an antialiased 9pt sans can leave as few as none of them: the
    // same cell renders 3 such pixels under this host's default sans and 0
    // under Liberation Sans, so a runner's fontconfig would decide whether
    // this test measured anything. A larger unantialiased face puts the count
    // in the tens on every family.
    QFont probeFont;
    probeFont.setPixelSize(14);
    probeFont.setStyleStrategy(QFont::NoAntialias);
    option.font = probeFont;
    option.fontMetrics = QFontMetrics(probeFont);
    if (selected == Selected::Yes) {
        option.state |= QStyle::State_Selected;
    }
    if (current == Current::Yes) {
        option.state |= QStyle::State_HasFocus;
    }
    {
        QPainter painter(&image);
        delegate.paint(&painter, option, index);
    }
    Render result;
    result.background = image.pixel(2, 2);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixel(x, y);
            result.valuePixels += pixel == valueColor.rgb();
            result.nearValuePixels
                += channelDistance(pixel, valueColor.rgb())
                <= focusWashTolerance;
            result.selectedTextPixels += pixel == selectedTextColor.rgb();
            result.nearSelectedTextPixels
                += channelDistance(pixel, selectedTextColor.rgb())
                <= focusWashTolerance;
        }
    }
    return result;
}

// Rec. 601 luma, only ever compared between two of these.
double luminance(QRgb pixel)
{
    return 0.299 * qRed(pixel) + 0.587 * qGreen(pixel) + 0.114 * qBlue(pixel);
}

std::string hex(QRgb pixel)
{
    return QColor(pixel).name().toStdString();
}

// Both halves of what a selected cell's background has to be: visibly moved
// from where it was, and still nearer to that than to the theme's fill. The
// second is what keeps the blend from collapsing into the flat highlight the
// delegate exists to avoid -- a weight of 0.99 satisfies the first alone.
void requireReadsAsSelected(
    QRgb plain, QRgb selected, const std::string& what)
{
    constexpr int visible = 16;
    const auto moved = channelDistance(selected, plain);
    require(moved >= visible,
        what + ": a selected cell (" + hex(selected) + ") is not visibly apart"
            " from an unselected one (" + hex(plain) + "): "
            + std::to_string(moved) + " < " + std::to_string(visible));
    const auto towardsHighlight
        = channelDistance(selected, selectionColor.rgb());
    require(moved < towardsHighlight,
        what + ": a selected cell (" + hex(selected) + ") is nearer the"
            " theme's fill (" + std::to_string(towardsHighlight) + ") than its"
            " own background (" + std::to_string(moved) + "), so it has lost"
            " what was under it");
}

// Every channel of the blend lies between the two it mixes -- strictly, where
// those two differ. A property rather than a recomputed weight, and enough to
// catch a mix that drops a channel or repeats another one in its place, which
// the rendered assertions cannot see: both still produce a visibly shifted,
// ground-nearer color.
void requireBlendsEveryChannel(const QColor& ground, const QColor& highlight,
    const std::string& what)
{
    const auto blended = amrvis::qt::datasetSelectionBackground(
        ground, highlight);
    const auto between = [](int from, int to, int mixed) {
        return from == to ? mixed == from
                          : mixed > std::min(from, to) && mixed < std::max(from, to);
    };
    require(between(ground.red(), highlight.red(), blended.red())
            && between(ground.green(), highlight.green(), blended.green())
            && between(ground.blue(), highlight.blue(), blended.blue()),
        what + ": " + blended.name().toStdString() + " does not mix every"
            " channel of " + ground.name().toStdString() + " towards "
            + highlight.name().toStdString());
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    QStandardItemModel model(2, 1);
    // A covered sample: a number in its color-bar color.
    model.setData(model.index(0, 0), QStringLiteral("1.5"), Qt::DisplayRole);
    model.setData(model.index(0, 0), valueColor, Qt::ForegroundRole);
    // An uncovered one: blank, on the renderer's no-data shade.
    model.setData(model.index(1, 0), noDataColor, Qt::BackgroundRole);

    const QStyledItemDelegate stock;
    const amrvis::qt::DatasetValueDelegate delegate;
    const auto covered = model.index(0, 0);
    const auto uncovered = model.index(1, 0);

    // What the review found, pinned so the fix cannot quietly regress to it:
    // under the stock delegate a selected value loses its color outright and
    // comes back in the theme's selected-text color.
    const auto stockPlain = render(stock, covered, Selected::No);
    const auto stockSelected = render(stock, covered, Selected::Yes);
    require(stockPlain.valuePixels > 0,
        "the stock delegate drew no value-colored pixels at all, so this test "
        "is measuring nothing");
    require(stockSelected.valuePixels == 0,
        "the stock delegate no longer drops the value color -- if Qt changed "
        "this, the delegate may no longer be needed");
    require(stockSelected.selectedTextPixels > 0,
        "the stock delegate did not repaint the value in HighlightedText");

    // The fix: the same digits, in the same color, selected or not.
    const auto plain = render(delegate, covered, Selected::No);
    const auto selected = render(delegate, covered, Selected::Yes);
    require(plain.valuePixels == stockPlain.valuePixels,
        "the delegate changed how an unselected value is drawn");
    require(selected.valuePixels == plain.valuePixels,
        "a selected value lost its color: " + std::to_string(plain.valuePixels)
            + " colored pixels became " + std::to_string(selected.valuePixels));
    require(selected.selectedTextPixels == 0,
        "a selected value was painted in the theme's selected-text color");

    // It still has to read as selected, and the cell's own background is what
    // says so -- not the theme's fill, which would hide the shading below.
    require(plain.background == cellBase.rgb(),
        "an unselected covered cell no longer sits on the view's base color: "
            + hex(plain.background));
    requireReadsAsSelected(plain.background, selected.background, "covered");

    // The state every click actually produces: selected *and* current. A
    // style may wash the focus rect over the cell, which shifts the value's
    // color a little -- it does that to an unselected current cell too -- but
    // the value must still be its own color and not the selected-text one.
    const auto focused = render(delegate, covered, Selected::Yes, Current::Yes);
    const auto stockFocused = render(stock, covered, Selected::Yes, Current::Yes);
    require(focused.nearValuePixels > 0,
        "a clicked (selected and current) value is not recognisably its own "
        "color any more");
    // Near-white rather than exactly white: the same wash that shifts our
    // value color shifts the stock delegate's white text off pure white, so
    // an exact count is zero for both delegates here and discriminates
    // nothing on the style this test actually runs under.
    require(focused.nearSelectedTextPixels == 0,
        "a clicked value was painted in the theme's selected-text color");
    require(stockFocused.nearValuePixels == 0,
        "the stock delegate no longer loses the value color on a clicked cell "
        "-- if Qt changed this, the delegate may no longer be needed");
    require(stockFocused.nearSelectedTextPixels > 0,
        "the stock delegate did not repaint a clicked value in the theme's "
        "selected-text color, so the check above is watching for nothing");

    // The no-data shade survives selection too, and stays darker than a
    // selected covered cell, so a selection dragged past the grids still shows
    // where the data stops.
    const auto blankPlain = render(delegate, uncovered, Selected::No);
    const auto blankSelected = render(delegate, uncovered, Selected::Yes);
    require(blankPlain.background == noDataColor.rgb(),
        "an unselected uncovered cell lost its no-data shade: "
            + hex(blankPlain.background));
    requireReadsAsSelected(
        blankPlain.background, blankSelected.background, "uncovered");
    require(luminance(blankSelected.background) < luminance(selected.background),
        "a selected uncovered cell (" + hex(blankSelected.background)
            + ") is no darker than a selected covered one ("
            + hex(selected.background) + ")");
    // ... where the stock delegate gives both the identical selection fill.
    require(render(stock, uncovered, Selected::Yes).background
            == render(stock, covered, Selected::Yes).background,
        "the stock delegate no longer flattens the no-data shade under a "
        "selection");

    // The margin the exact-color counts leave, so a future host whose fonts
    // render thinner glyphs shows up as a shrinking number rather than as a
    // sudden failure.
    // The mix itself, where a per-channel slip is visible in a way the
    // rendered cells above cannot show.
    requireBlendsEveryChannel(cellBase, selectionColor, "covered");
    requireBlendsEveryChannel(noDataColor, selectionColor, "uncovered");

    std::cout << "dataset selected cell OK (" << plain.valuePixels
              << " value pixels)\n";
    return 0;
}
