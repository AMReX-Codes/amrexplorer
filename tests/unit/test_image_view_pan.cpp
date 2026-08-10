#include "ImageView.hpp"

#include <QApplication>
#include <QImage>
#include <QPoint>
#include <QScrollBar>
#include <QTransform>

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

QImage solidImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::black);
    return image;
}

// A scene larger than the viewport pans by its scroll bars, and the delta says
// how far the *content* moves: content travelling +x backs the bar off by the
// same amount. MainWindow's arrow-key step and its drag handler both hand
// panViewport a delta in that convention, so a sign slip here silently inverts
// the arrow keys in exactly one display mode.
void scrollBarPanFollowsContentDelta()
{
    amrvis::qt::ImageView view;
    view.resize(200, 150);
    view.show();
    QApplication::processEvents();
    view.setImage(solidImage(800, 600));
    view.setFixedScale(2);
    QApplication::processEvents();

    auto* const hBar = view.horizontalScrollBar();
    auto* const vBar = view.verticalScrollBar();
    require(hBar->maximum() > hBar->minimum(),
        "a 2x scale of an 800px raster must overflow a 200px viewport");
    require(vBar->maximum() > vBar->minimum(),
        "a 2x scale of a 600px raster must overflow a 150px viewport");

    // Park both bars mid-range so neither end clamps the step under test.
    hBar->setValue((hBar->minimum() + hBar->maximum()) / 2);
    vBar->setValue((vBar->minimum() + vBar->maximum()) / 2);
    const auto startX = hBar->value();
    const auto startY = vBar->value();

    view.panViewport(QPoint(10, 6));
    require(hBar->value() == startX - 10,
        "content moving +x must decrease the horizontal scroll value by 10");
    require(vBar->value() == startY - 6,
        "content moving +y must decrease the vertical scroll value by 6");
    require(view.transformMode()
            == amrvis::qt::ImageView::TransformMode::FixedScale,
        "scrolling must leave the display mode untouched");
}

// With the whole scene visible there is nowhere to scroll, so a pan is a
// no-op. Translating instead would slide the image off-centre and demote the
// mode to Custom without telling MainWindow, leaving the Scale button stale.
void fullyVisibleSceneIgnoresPan()
{
    amrvis::qt::ImageView view;
    view.resize(400, 300);
    view.show();
    QApplication::processEvents();
    view.setImage(solidImage(50, 40));
    view.fitToWindow();
    QApplication::processEvents();

    auto* const hBar = view.horizontalScrollBar();
    auto* const vBar = view.verticalScrollBar();
    require(hBar->maximum() == hBar->minimum()
            && vBar->maximum() == vBar->minimum(),
        "a fitted raster must leave both scroll bars without range");

    const auto before = view.transform();
    view.panViewport(QPoint(25, 25));
    require(view.transform() == before,
        "a fully visible scene must not be translated by a pan");
    require(view.transformMode() == amrvis::qt::ImageView::TransformMode::Fit,
        "panning must not demote Fit to Custom");
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    scrollBarPanFollowsContentDelta();
    fullyVisibleSceneIgnoresPan();
    return 0;
}
