#include "DiagnosticsModel.hpp"

#include <QApplication>
#include <QDockWidget>
#include <QPlainTextEdit>
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

bool hasLine(const QString& text, const QString& line)
{
    return text.split(QLatin1Char('\n')).contains(line);
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    using amrvis::qt::DiagnosticsModel;

    std::uint64_t generation = 3;
    qint64 frameSwitch = 42;
    QString remote;
    const auto hooks = [&] {
        return DiagnosticsModel::Hooks{
            [&generation] { return generation; },
            [&frameSwitch] { return frameSwitch; },
            [&remote] { return remote; },
        };
    };

    // The rendered text: every counter on its own line, the host's lines
    // through the hooks, and the remote block only when the host has one.
    {
        DiagnosticsModel model(hooks());
        auto text = model.text();
        require(hasLine(text, QStringLiteral("generation: 3"))
                && hasLine(text, QStringLiteral("active background requests: 0"))
                && hasLine(text, QStringLiteral("stale results discarded: 0"))
                && hasLine(text, QStringLiteral("last frame switch: 42 ms")),
            "the initial text lacks a counter or a hook value");
        require(!text.contains(QStringLiteral("remote")),
            "a remote block appeared with no remote endpoint");
        remote = QStringLiteral("\nremote endpoint: host:7");
        require(hasLine(model.text(), QStringLiteral("remote endpoint: host:7")),
            "the remote hook's lines were not appended");
        remote.clear();
        generation = 4;
        frameSwitch = 7;
        require(hasLine(model.text(), QStringLiteral("generation: 4"))
                && hasLine(model.text(), QStringLiteral("last frame switch: 7 ms")),
            "hook values are not read live");
    }

    // Activity bookkeeping balances and never wraps: an unbalanced decrement
    // clamps to zero (a wrap would defeat every "nothing in flight" check).
    {
        DiagnosticsModel model(hooks());
        model.adjustActivity(1);
        model.adjustActivity(1);
        require(model.activeRequests() == 2, "increments were not counted");
        model.adjustActivity(-1);
        require(model.activeRequests() == 1, "a decrement was not counted");
        model.adjustActivity(-2);
        require(model.activeRequests() == 0,
            "an unbalanced decrement did not clamp to zero");
        model.adjustActivity(-1);
        require(model.activeRequests() == 0, "the count wrapped");
        model.adjustActivity(1);
        require(model.activeRequests() == 1
                && hasLine(model.text(),
                    QStringLiteral("active background requests: 1")),
            "activity after a clamp is not counted from zero");
        model.noteStaleResult();
        model.noteStaleResult();
        require(model.staleResults() == 2
                && hasLine(model.text(),
                    QStringLiteral("stale results discarded: 2")),
            "stale results were not counted");
    }

    // Metrics render, and a dataset reset clears the per-dataset ones (last
    // read, cache, probe history) while the counters and errors survive.
    {
        DiagnosticsModel model(hooks());
        amrvis::CacheMetrics cache;
        cache.budgetBytes = 100;
        cache.residentBytes = 60;
        cache.pinnedBytes = 10;
        cache.evictions = 2;
        model.setCacheMetrics(cache);
        model.setMetadataMetrics(5, 5000);
        model.setSliceMetrics(8, 3, 800);
        model.appendProbeLine(QStringLiteral("probe (1, 2) = 3"));
        model.noteStaleResult();
        model.reportBackgroundError(QStringLiteral("boom"));
        auto text = model.text();
        require(hasLine(text, QStringLiteral("cache budget bytes: 100"))
                && hasLine(text, QStringLiteral("cache resident bytes: 60"))
                && hasLine(text, QStringLiteral("cache pinned bytes: 10"))
                && hasLine(text, QStringLiteral("cache evictions: 2"))
                && hasLine(text, QStringLiteral("metadata files read: 5"))
                && hasLine(text, QStringLiteral("metadata bytes read: 5000"))
                && hasLine(text, QStringLiteral("blocks read: 8"))
                && hasLine(text, QStringLiteral("cache hits: 3"))
                && hasLine(text, QStringLiteral("payload bytes read: 800"))
                && hasLine(text, QStringLiteral("probe (1, 2) = 3"))
                && hasLine(text, QStringLiteral("background error: boom")),
            "a metric, probe line or error is missing from the text");
        model.resetDatasetMetrics();
        text = model.text();
        // Every per-dataset field, so no single clear can go missing.
        require(hasLine(text, QStringLiteral("blocks read: 0"))
                && hasLine(text, QStringLiteral("cache hits: 0"))
                && hasLine(text, QStringLiteral("payload bytes read: 0"))
                && hasLine(text, QStringLiteral("cache budget bytes: 0"))
                && hasLine(text, QStringLiteral("cache resident bytes: 0"))
                && hasLine(text, QStringLiteral("cache pinned bytes: 0"))
                && hasLine(text, QStringLiteral("cache evictions: 0"))
                && !text.contains(QStringLiteral("probe (1, 2)")),
            "the dataset reset left per-dataset state behind");
        require(hasLine(text, QStringLiteral("stale results discarded: 1"))
                && hasLine(text, QStringLiteral("background error: boom"))
                && hasLine(text, QStringLiteral("metadata files read: 5")),
            "the dataset reset took counters, errors or metadata metrics");
    }

    // Bounded histories: the newest 100 probe lines and the newest 50 errors.
    {
        DiagnosticsModel model(hooks());
        for (int i = 0; i < 120; ++i) {
            model.appendProbeLine(QStringLiteral("probe %1").arg(i));
        }
        for (int i = 0; i < 60; ++i) {
            model.reportBackgroundError(QStringLiteral("error %1").arg(i));
        }
        const auto text = model.text();
        require(!text.contains(QStringLiteral("probe 19\n"))
                && hasLine(text, QStringLiteral("probe 20"))
                && hasLine(text, QStringLiteral("probe 119")),
            "the probe history is not the newest 100");
        require(!text.contains(QStringLiteral("background error: error 9\n"))
                && hasLine(text, QStringLiteral("background error: error 10"))
                && hasLine(text, QStringLiteral("background error: error 59"))
                && model.backgroundErrorCount() == 50,
            "the error history is not the newest 50");
    }

    // The dock: hidden until an error is reported, then shown with the text.
    {
        DiagnosticsModel model(hooks());
        QWidget host;
        host.show();
        auto* dock = model.createDock(&host);
        require(dock != nullptr && dock->parent() == &host && dock->isHidden(),
            "the dock did not start hidden, owned by the host");
        model.refresh();
        auto* view = dock->findChild<QPlainTextEdit*>();
        require(view != nullptr && view->isReadOnly()
                && view->toPlainText().contains(QStringLiteral("generation:")),
            "refresh did not render into the dock's text");
        model.reportBackgroundError(QStringLiteral("shown"));
        require(!dock->isHidden()
                && view->toPlainText().contains(
                    QStringLiteral("background error: shown")),
            "an error did not show the dock with the report");
    }

    std::cout << "diagnostics model tests passed\n";
    return 0;
}
