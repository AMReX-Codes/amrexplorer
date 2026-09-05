#include "AnimationExporter.hpp"
#include "ColorBarWidget.hpp"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

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
    // This test examines PNGs and the export lifecycle, independent of whether
    // the machine has a real encoder. QProcess must report it unavailable.
    const bool encode = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--with-ffmpeg";
    if (!encode)
        qputenv("PATH", QByteArray());
    using namespace amrvis::qt;
    enum class Mode { Success, AspectChange, MissingPanel, Cancel };
    Mode mode = Mode::Success;
    int frameIndex = 0;
    bool finished = false;
    bool success = false;
    QString message;
    int encodingStarts = 0;
    ExportLayout firstLayout;
    QEventLoop loop;
    AnimationExporter* exporterPointer = nullptr;
    AnimationExporter exporter(
        [&](const ExportOptions& options, qreal, std::map<QString, ExportLayout>& layouts) {
            std::vector<std::pair<QString, QImage>> frames;
            if (mode == Mode::MissingPanel && frameIndex == 1)
                return frames;
            const QSize sourceSize = frameIndex == 0              ? QSize(600, 400)
                                     : mode == Mode::AspectChange ? QSize(1200, 600)
                                                                  : QSize(1200, 800);
            auto& layout = layouts[QString()];
            if (layout.dataRect.isEmpty()) {
                require(frameIndex == 0, "layout was not retained between frames");
                layout = makeExportLayout(sourceSize, options);
                firstLayout = layout;
            } else if (!exportAspectMatches(sourceSize, layout)) {
                throw std::runtime_error("Panel xy aspect ratio changed");
            }
            require(layout.dataRect == firstLayout.dataRect && layout.font == firstLayout.font,
                    "plot rectangle or font moved during export");
            QImage source(sourceSize, QImage::Format_ARGB32_Premultiplied);
            source.fill(QColor(32, 64, 96));
            // A fixed normalized landmark stays in the same output pixels
            // even when the source doubles its resolution.
            QPainter marker(&source);
            marker.fillRect(source.width() / 4, source.height() / 4, source.width() / 20,
                            source.height() / 20, Qt::yellow);
            marker.end();
            const double scale = frameIndex == 0 ? 1.0 : frameIndex == 1 ? 1e200 : 1e-200;
            const std::array<ExportAxis, 2> axes{{{"x", -scale, scale}, {"y", 0.0, scale}}};
            ColorBarWidget bar;
            bar.setFont(layout.font);
            bar.setNumberFormat(options.numberFormat);
            bar.setFieldRange("density", -scale, scale);
            frames.emplace_back(QString(), composeExportImage(source.scaled(layout.dataRect.size()),
                                                              axes, options, layout, &bar));
            return frames;
        },
        [&](int index) {
            QTimer::singleShot(0, &loop, [&, index] {
                frameIndex = index;
                if (mode == Mode::Cancel && index == 1)
                    exporterPointer->cancelForShutdown();
                exporterPointer->onFrameDisplayed(index);
            });
        });
    exporterPointer = &exporter;
    QObject::connect(&exporter, &AnimationExporter::encodingStarted, [&] { ++encodingStarts; });
    QObject::connect(&exporter, &AnimationExporter::finished,
                     [&](bool ok, const QString& detail, int restoreIndex) {
                         finished = true;
                         success = ok;
                         message = detail;
                         require(restoreIndex == 7 && !exporter.active(),
                                 "export state not reset on completion");
                         loop.quit();
                     });
    ExportOptions options;
    options.includeAxes = true;
    options.transparentBackground = true;
    options.font = QFont(QStringLiteral("Sans Serif"));
    options.numberFormat = "%.12f";
    QTemporaryDir directory;
    require(directory.isValid(), "no temporary export directory");
    int runNumber = 0;
    for (const auto nextMode : {Mode::AspectChange, Mode::Success, Mode::MissingPanel,
                                Mode::Success, Mode::Cancel, Mode::Success}) {
        mode = nextMode;
        frameIndex = 0;
        finished = false;
        success = false;
        const auto stem = directory.path() + QStringLiteral("/run%1").arg(runNumber++);
        require(exporter.begin(stem + ".png", options, 3, 7, 1.0, {QString()}, nullptr),
                "could not start export after previous completion");
        require(!exporter.begin(stem + ".png", options, 3, 7, 1.0, {QString()}, nullptr),
                "a second export started while one was active");
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(10000);
        QTimer::singleShot(0, &loop, [&] { exporter.onFrameDisplayed(0); });
        loop.exec();
        require(finished, "export did not finish before timeout");
        require(success == (mode == Mode::Success), "unexpected export outcome");
        const QImage first(stem + "_00000.png");
        require(!first.isNull(), "the first PNG was not retained");
        if (mode == Mode::Success) {
            for (int i = 1; i < 3; ++i) {
                const QImage later(stem + QStringLiteral("_%1.png").arg(i, 5, 10, QChar('0')));
                require(later.size() == first.size(), "written PNG dimensions changed");
                require(later.copy(firstLayout.dataRect) == first.copy(firstLayout.dataRect),
                        "written PNG plot or landmark shifted");
                require(later.dotsPerMeterX() == first.dotsPerMeterX(), "print size changed");
            }
            if (encode) {
                require(QFileInfo::exists(stem + ".mp4"), "MP4 was not written");
                QProcess decoder;
                decoder.start("ffmpeg", {"-v", "error", "-i", stem + ".mp4", "-frames:v", "1", "-f",
                                         "rawvideo", "-pix_fmt", "rgb24", "-"});
                require(decoder.waitForFinished(10000) && decoder.exitCode() == 0,
                        "could not decode the exported MP4");
                const auto pixels = decoder.readAllStandardOutput();
                require(pixels.size() >= 3 && static_cast<unsigned char>(pixels[0]) > 240 &&
                            static_cast<unsigned char>(pixels[1]) > 240 &&
                            static_cast<unsigned char>(pixels[2]) > 240,
                        "MP4 flattened transparent margins onto a non-white background");
            }
        } else {
            require(!QFileInfo::exists(stem + "_00001.png"), "an invalid frame was written");
            if (mode != Mode::Cancel)
                require(message.contains("Frame 1"), "failure omits frame index");
        }
    }
    require(encodingStarts == (encode ? 3 : 0), "unexpected number of MP4 encodes");
    std::cout << "animation exporter tests passed\n";
}
