// The two rules in exporting a widget's picture: the file name PNG bytes are
// saved under, and painting a widget without the controls parked over it.
//
// Both shipped a bug that only review caught, because both lived inside a
// private slot nothing could call. The name rule shipped two: a typed "shot"
// overwriting shot.png unasked, and chop-and-re-append rewriting "shot.PNG" to
// "shot.png" -- the second introduced while fixing the first. The painting rule
// shipped one: QWidget::grab() baking the volume view's XY/XZ/YZ preset buttons
// into every exported frame.
//
// The painting test does not use IsoWidget. It builds its own widget with a
// child parked over it, so it pins the rule rather than the volume window's
// current arrangement of buttons.

#include "WidgetImageExport.hpp"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPushButton>
#include <QWidget>

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

void requirePath(const char* chosen, const char* expected)
{
    const auto actual = amrvis::qt::pngExportPath(QString::fromUtf8(chosen));
    if (actual == QString::fromUtf8(expected)) {
        return;
    }
    std::cerr << "FAILED: pngExportPath(\"" << chosen << "\") gave \""
              << actual.toStdString() << "\", expected \"" << expected << "\"\n";
    std::exit(1);
}

// A widget that fills itself solid blue, with a solid red child button parked
// over it -- the volume view's preset buttons, reduced to what matters here.
class HostWidget final : public QWidget {
public:
    static constexpr QColor background() { return QColor(0, 0, 255); }
    static constexpr QColor child() { return QColor(255, 0, 0); }

    HostWidget()
    {
        resize(120, 80);
        m_button = new QPushButton(QStringLiteral("XY"), this);
        // Flat solid red: a styled button paints its own frame and hover, and
        // the count below wants one unambiguous colour to look for.
        m_button->setStyleSheet(
            QStringLiteral("QPushButton { background: rgb(255,0,0);"
                           " border: none; color: rgb(255,0,0); }"));
        m_button->setGeometry(40, 50, 30, 20);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), background());
    }

private:
    QPushButton* m_button = nullptr;
};

// Pixels within a small distance of `wanted`. Exact equality would be brittle
// across the premultiplied conversion and any platform-side rounding.
int countNear(const QImage& image, const QColor& wanted)
{
    int found = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (std::abs(pixel.red() - wanted.red()) < 40
                && std::abs(pixel.green() - wanted.green()) < 40
                && std::abs(pixel.blue() - wanted.blue()) < 40
                && pixel.alpha() > 200) {
                ++found;
            }
        }
    }
    return found;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);

    // --- the file name ---------------------------------------------------
    // A name with no png suffix gets one; the bytes are PNG whatever the name.
    requirePath("shot", "shot.png");
    requirePath("/tmp/pictures/shot", "/tmp/pictures/shot.png");
    // A name that already says png is returned untouched...
    requirePath("shot.png", "shot.png");
    requirePath("a.png.png", "a.png.png");
    // ...case included. Rewriting this to lowercase wrote past the file the
    // dialog had vetted on a case-sensitive filesystem.
    requirePath("shot.PNG", "shot.PNG");
    requirePath("shot.PnG", "shot.PnG");
    // Another format's suffix is not a png suffix, so it gains one rather than
    // being replaced: the caller is about to write PNG bytes there.
    requirePath("shot.jpg", "shot.jpg.png");
    // A dot in a directory but not in the file name.
    requirePath("/tmp/v1.2/shot", "/tmp/v1.2/shot.png");
    // Empty comes back empty; the caller returns early on it.
    requirePath("", "");
    // A dotfile that is only the suffix already says png.
    requirePath(".png", ".png");
    // A trailing dot is not a suffix, so it gains one -- pinned so the
    // behaviour is chosen rather than accidental.
    requirePath("shot.", "shot..png");

    // --- painting the widget without its children ------------------------
    HostWidget host;
    host.show();
    application.processEvents();

    const auto exported = amrvis::qt::renderWidgetWithoutChildren(host, 1.0);
    const auto grabbed = host.grab().toImage();

    // The rule: the child is not in the picture.
    require(countNear(exported, HostWidget::child()) == 0,
        "the exported image contains the child widget's pixels");
    require(countNear(exported, HostWidget::background()) > 0,
        "the exported image did not paint the widget itself");

    // And the contrast that keeps that assertion honest: grab() DOES contain
    // the child, so reverting to it fails the test above rather than passing
    // it silently.
    require(countNear(grabbed, HostWidget::child()) > 0,
        "grab() did not draw the child, so the check above proves nothing");

    // --- the device pixel ratio ------------------------------------------
    // The buffer is scaled, not left at logical size, or the file holds a
    // fraction of the pixels on screen.
    const auto retina = amrvis::qt::renderWidgetWithoutChildren(host, 2.0);
    require(retina.width() == host.width() * 2
            && retina.height() == host.height() * 2,
        "a 2x export was not allocated at twice the logical size");
    require(qFuzzyCompare(retina.devicePixelRatio(), 2.0),
        "a 2x export did not record its device pixel ratio");
    require(exported.width() == host.width()
            && qFuzzyCompare(exported.devicePixelRatio(), 1.0),
        "a 1x export was not at logical size");
    // A nonsense ratio falls back to 1 rather than allocating nothing.
    const auto degenerate = amrvis::qt::renderWidgetWithoutChildren(host, 0.0);
    require(degenerate.size() == host.size(),
        "a non-positive ratio did not fall back to logical size");
    // The child stays out at every ratio.
    require(countNear(retina, HostWidget::child()) == 0,
        "a 2x export contains the child widget's pixels");
    // And the widget was painted ACROSS the larger buffer, not into its
    // top-left quarter: without this, scaling the allocation while leaving the
    // painter at 1x passes every assertion above and exports a picture that is
    // three-quarters empty. Four times the area, less the child's excluded
    // rect and some edge rounding.
    require(countNear(retina, HostWidget::background())
            > 3.5 * countNear(exported, HostWidget::background()),
        "the 2x export did not paint the widget across the larger buffer");
    return 0;
}
