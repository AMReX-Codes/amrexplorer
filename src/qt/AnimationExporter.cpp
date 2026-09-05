#include "AnimationExporter.hpp"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPair>
#include <QProcess>
#include <QProgressDialog>
#include <QStringList>
#include <QtConcurrent>

#include <algorithm>
#include <exception>
#include <set>
#include <stdexcept>

namespace amrvis::qt {

AnimationExporter::AnimationExporter(
    FrameRenderer renderFrames, AdvanceFrame advanceFrame, QObject* parent)
    : QObject(parent)
    , m_renderFrames(std::move(renderFrames))
    , m_advanceFrame(std::move(advanceFrame))
{
}

bool AnimationExporter::begin(const QString& path, const ExportOptions& options, int totalFrames,
                              int restoreIndex, qreal scale, std::vector<QString> panelSuffixes,
                              QWidget* dialogParent) {
    if (m_active || totalFrames <= 0 || panelSuffixes.empty()) {
        return false;
    }
    const QFileInfo info(path);
    m_active = true;
    m_canceled = false;
    m_framesDone = false;
    m_options = options;
    m_layouts.clear();
    m_nextFrame = 0;
    // Probe ffmpeg off the GUI thread (waitForStarted/Finished can block up to
    // ~4 s); the result is only needed at finalize, after every frame renders.
    m_hasFfmpeg = false;
    m_ffmpegProbe = QtConcurrent::run([] { return probeFfmpeg(); });
    m_scale = scale;
    m_totalFrames = totalFrames;
    m_restoreIndex = restoreIndex;
    m_digitWidth = std::max(5,
        static_cast<int>(QString::number(totalFrames - 1).length()));
    m_directory = info.absolutePath();
    m_stem = info.completeBaseName();
    m_panelSuffixes = std::move(panelSuffixes);
    m_encoderCancel.reset();
    m_encodersRemaining = 0;
    m_encodeFailed = false;
    m_encodeFailureLog.clear();

    m_progress = new QProgressDialog(
        tr("Rendering frame 1 of %1...").arg(totalFrames), tr("Cancel"),
        0, totalFrames, dialogParent);
    m_progress->setWindowTitle(tr("Export Animation"));
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(0);
    m_progress->setValue(0);
    connect(m_progress, &QProgressDialog::canceled, this, [this] {
        m_canceled = true;
        if (m_encoderCancel) {
            m_encoderCancel->store(true);
        }
    });
    return true;
}

void AnimationExporter::cancelForShutdown()
{
    if (m_progress != nullptr) {
        m_canceled = true;
        if (m_encoderCancel) {
            m_encoderCancel->store(true);
        }
        m_progress->cancel();
    }
}

void AnimationExporter::onFrameDisplayed(int index)
{
    if (!m_active || m_framesDone || index != m_nextFrame) {
        return;
    }
    if (m_canceled) {
        endExport(false, tr("Animation export cancelled."));
        return;
    }

    const QString padded
        = QString("%1").arg(index, m_digitWidth, 10, QChar('0'));
    std::vector<std::pair<QString, QImage>> frames;
    try {
        frames = m_renderFrames(m_options, m_scale, m_layouts);
        std::set<QString> seen;
        for (const auto& [suffix, frame] : frames) {
            const auto layout = m_layouts.find(suffix);
            if (frame.isNull() || !seen.insert(suffix).second ||
                std::find(m_panelSuffixes.begin(), m_panelSuffixes.end(), suffix) ==
                    m_panelSuffixes.end() ||
                layout == m_layouts.end() || frame.size() != layout->second.canvasSize) {
                throw std::runtime_error(
                    "An export panel is missing, duplicated, or has inconsistent dimensions.");
            }
        }
        if (seen.size() != m_panelSuffixes.size()) {
            throw std::runtime_error("An expected export panel is missing.");
        }
    } catch (const std::exception& error) {
        endExport(false, tr("Frame %1: %2\nAlready-written PNG frames have been kept.")
                             .arg(index)
                             .arg(QString::fromUtf8(error.what())));
        return;
    }
    for (const auto& [suffix, frame] : frames) {
        const QString filePath = QDir(m_directory).absoluteFilePath(
            m_stem + suffix + "_" + padded + ".png");
        if (!frame.save(filePath, "PNG")) {
            endExport(false, tr("Could not write %1.").arg(filePath));
            return;
        }
    }

    m_progress->setValue(index + 1);
    m_progress->setLabelText(tr("Rendering frame %1 of %2...")
        .arg(index + 2).arg(m_totalFrames));

    if (index + 1 < m_totalFrames) {
        m_nextFrame = index + 1;
        m_advanceFrame(index + 1);
    } else {
        finalizeEncoding();
    }
}

void AnimationExporter::onFrameFailed()
{
    if (!m_active) {
        return;
    }
    endExport(false, tr("A frame failed to load; animation export aborted."));
}

void AnimationExporter::finalizeEncoding()
{
    m_framesDone = true;
    // Continue once the probe launched at begin() resolves. Rendering has
    // usually already let it finish, so the watcher typically fires
    // immediately; either way the GUI thread never blocks on it.
    auto* watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
        m_hasFfmpeg = watcher->result();
        watcher->deleteLater();
        finalizeEncodingWithProbe();
    });
    watcher->setFuture(m_ffmpegProbe);
}

void AnimationExporter::finalizeEncodingWithProbe()
{
    if (!m_active) {
        return;
    }
    if (m_canceled) {
        endExport(false, tr("Animation export cancelled."));
        return;
    }
    if (!m_hasFfmpeg) {
        endExport(true, tr("Exported %1 PNG frames "
            "(FFmpeg not found; skipped MP4).").arg(m_totalFrames));
        return;
    }

    m_progress->setLabelText(tr("Encoding MP4..."));
    m_progress->setRange(0, 0);
    // Cancellation flag shared with the encoder workers (captured by value,
    // so it outlives this object). Set by the progress Cancel and by
    // cancelForShutdown.
    m_encoderCancel = std::make_shared<std::atomic<bool>>(false);
    emit encodingStarted();

    const auto encode = [this](const QString& stem) {
        const QString inputPattern = m_directory + "/"
            + stem + "_%0" + QString::number(m_digitWidth) + "d.png";
        const QString outputPath
            = QDir(m_directory).absoluteFilePath(stem + ".mp4");
        // PNG retains alpha, while H.264/yuv420p does not. Composite onto an
        // explicit white background rather than letting alpha become black.
        const auto suffix = stem.mid(m_stem.size());
        const auto size = m_layouts.at(suffix).canvasSize;
        const QStringList args{
            "-y",
            "-framerate",
            "24",
            "-i",
            inputPattern,
            "-f",
            "lavfi",
            "-i",
            QStringLiteral("color=c=white:s=%1x%2:r=24").arg(size.width()).arg(size.height()),
            "-filter_complex",
            "[1:v][0:v]overlay=shortest=1,scale=trunc(iw/2)*2:trunc(ih/2)*2",
            "-pix_fmt",
            "yuv420p",
            "-crf",
            "14",
            outputPath,
        };
        return QtConcurrent::run(
            [args, cancel = m_encoderCancel]() -> QPair<int, QString> {
            QProcess proc;
            proc.setProcessChannelMode(QProcess::MergedChannels);
            proc.start("ffmpeg", args);
            if (!proc.waitForStarted(3000)) {
                return { -2,
                    QString::fromLocal8Bit(proc.readAllStandardOutput()) };
            }
            // Bounded wait: poll so Cancel/close can interrupt instead of
            // blocking the global pool forever. This worker owns proc, so it
            // terminates -- and kills, if needed -- the encoder itself.
            while (proc.state() != QProcess::NotRunning) {
                if (proc.waitForFinished(200)) {
                    break;
                }
                if (cancel && cancel->load()) {
                    proc.terminate();
                    if (!proc.waitForFinished(3000)) {
                        proc.kill();
                        proc.waitForFinished(1000);
                    }
                    break;
                }
            }
            const int code = proc.exitStatus() == QProcess::NormalExit
                ? proc.exitCode() : -1;
            QString log = QString::fromLocal8Bit(
                proc.readAllStandardOutput());
            if (log.length() > 800) {
                log = QStringLiteral("...") + log.right(800);
            }
            return { code, log.trimmed() };
        });
    };

    if (m_panelSuffixes.size() > 1) {
        QStringList stems;
        for (const auto& suffix : m_panelSuffixes) {
            stems.append(m_stem + suffix);
        }
        m_encodersRemaining = static_cast<int>(stems.size());
        m_encodeFailed = false;
        m_encodeFailureLog.clear();
        for (const auto& stem : stems) {
            auto* watcher = new QFutureWatcher<QPair<int, QString>>(this);
            connect(watcher, &QFutureWatcher<QPair<int, QString>>::finished,
                this, [this, watcher, stems] {
                    const auto result = watcher->result();
                    watcher->deleteLater();
                    if (result.first != 0) {
                        m_encodeFailed = true;
                        m_encodeFailureLog = result.second;
                    }
                    if (--m_encodersRemaining == 0) {
                        if (m_canceled) {
                            endExport(false, tr("Export cancelled."));
                        } else if (m_encodeFailed) {
                            endExport(false,
                                tr("FFmpeg failed. PNG frames were "
                                "still written.\n\n%1")
                                .arg(m_encodeFailureLog));
                        } else {
                            QStringList names;
                            for (const auto& encoded : stems) {
                                names.append(encoded + ".mp4");
                            }
                            endExport(true,
                                tr("Exported %1 frames and %2.")
                                .arg(m_totalFrames)
                                .arg(names.join(QStringLiteral(", "))));
                        }
                    }
                });
            watcher->setFuture(encode(stem));
        }
    } else {
        const QString stem = m_stem + m_panelSuffixes.front();
        auto* watcher = new QFutureWatcher<QPair<int, QString>>(this);
        connect(watcher, &QFutureWatcher<QPair<int, QString>>::finished,
            this, [this, watcher, stem] {
                const auto result = watcher->result();
                watcher->deleteLater();
                if (m_canceled) {
                    endExport(false, tr("Export cancelled."));
                    return;
                }
                const QString outputPath
                    = QDir(m_directory).absoluteFilePath(stem + ".mp4");
                if (result.first == 0) {
                    endExport(true, tr("Exported %1 frames and %2.")
                        .arg(m_totalFrames).arg(outputPath));
                } else {
                    endExport(false,
                        tr("FFmpeg failed (exit %1). "
                        "PNG frames were still written.\n\n%2")
                        .arg(result.first).arg(result.second));
                }
            });
        watcher->setFuture(encode(stem));
    }
}

void AnimationExporter::endExport(bool success, const QString& message)
{
    if (!m_active) {
        return;
    }
    const int restoreIndex = m_restoreIndex;

    if (m_progress != nullptr) {
        m_progress->hide();
        m_progress->deleteLater();
        m_progress = nullptr;
    }
    m_active = false;
    m_canceled = false;
    m_framesDone = false;
    m_encoderCancel.reset();
    m_encodersRemaining = 0;
    m_encodeFailed = false;
    m_encodeFailureLog.clear();
    m_panelSuffixes.clear();
    m_layouts.clear();
    m_options = {};
    m_restoreIndex = -1;

    emit finished(success, message, restoreIndex);
}

bool AnimationExporter::probeFfmpeg()
{
    QProcess proc;
    proc.start("ffmpeg", {"-version"});
    if (!proc.waitForStarted(2000) || !proc.waitForFinished(2000)) {
        return false;
    }
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

} // namespace amrvis::qt
