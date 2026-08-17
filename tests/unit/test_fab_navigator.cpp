#include "FabNavigator.hpp"

#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <QApplication>
#include <QTimer>
#include <QWidget>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view realDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create fixture text");
    output << text;
}

void appendFab(std::ofstream& output, std::string_view box,
    const std::vector<double>& values)
{
    output << "FAB " << realDescriptor << box << " 1\n";
    output.write(reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(values.size() * sizeof(double)));
}

// A raw FAB file holding two 2-D records: (0,0)-(1,1) then (0,0)-(3,0).
void writeRawFabFile(const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create fixture FAB");
    appendFab(output, "((0,0) (1,1) (0,0))", {1.0, 2.0, 3.0, 4.0});
    appendFab(output, "((0,0) (3,0) (0,0))", {5.0, 6.0, 7.0, 8.0});
}

// A single-level MultiFab (VisMF header plus two FABs), the shape whose
// selector lists blocks that open from the source metadata.
void writeMultiFab(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);
    writeText(root / "Cell_H",
        "1\n1\n1\n0\n"
        "(2 0\n"
        "((0,0) (3,2) (0,0))\n"
        "((0,3) (2,3) (0,0))\n"
        ")\n"
        "2\n"
        "FabOnDisk: Cell_D_00000 0\n"
        "FabOnDisk: Cell_D_00001 0\n"
        "\n"
        "2,1\n0.0,\n30.0,\n\n2,1\n23.0,\n32.0,\n");
    std::vector<double> gridA;
    for (int j = 0; j <= 2; ++j) {
        for (int i = 0; i <= 3; ++i) {
            gridA.push_back(10.0 * j + i);
        }
    }
    std::vector<double> gridB{30.0, 31.0, 32.0};
    {
        std::ofstream output(root / "Cell_D_00000", std::ios::binary);
        appendFab(output, "((0,0) (3,2) (0,0))", gridA);
    }
    {
        std::ofstream output(root / "Cell_D_00001", std::ios::binary);
        appendFab(output, "((0,3) (2,3) (0,0))", gridB);
    }
}

// What the host would see: every openPrepared call and every signal.
struct Opened {
    std::filesystem::path path;
    std::filesystem::path dataRoot;
    std::string fileVersion;
    std::size_t levelBlocks = 0;
    bool preserveSelector = false;
    bool hadSpec = false;
    int levelSelection = 0;
};

struct Host {
    std::uint64_t generation = 1;
    bool shuttingDown = false;
    bool datasetOpen = false;
    std::vector<Opened> opened;
    int activity = 0;
    int activityEvents = 0;
    int stale = 0;
    QStringList failures;
    int titleChanges = 0;
    // When set, openPrepared throws instead of opening (a host-side failure).
    bool refuseOpens = false;

    amrvis::qt::FabNavigator::Hooks hooks()
    {
        return amrvis::qt::FabNavigator::Hooks{
            [this] { return generation; },
            [this] { return shuttingDown; },
            [this]() -> std::optional<amrvis::FrameSliceSpec> {
                if (!datasetOpen) {
                    return std::nullopt;
                }
                amrvis::FrameSliceSpec spec;
                spec.levelSelection = 2;
                spec.rangeMode = amrvis::RangeMode::User;
                spec.userRange = std::pair{0.0, 1.0};
                return spec;
            },
            [this](const std::filesystem::path& path,
                amrvis::PlotfileMetadataResult metadata,
                std::filesystem::path dataRoot, bool preserve,
                std::optional<amrvis::FrameSliceSpec> spec) {
                if (refuseOpens) {
                    throw std::runtime_error("the host refused the open");
                }
                Opened entry;
                entry.path = path;
                entry.dataRoot = std::move(dataRoot);
                entry.fileVersion = metadata.fileVersion;
                entry.levelBlocks = metadata.metadata->levels.front().blocks.size();
                entry.preserveSelector = preserve;
                entry.hadSpec = spec.has_value();
                entry.levelSelection = spec ? spec->levelSelection : 99;
                opened.push_back(entry);
                datasetOpen = true;
            },
        };
    }

    void observe(amrvis::qt::FabNavigator& navigator)
    {
        QObject::connect(&navigator,
            &amrvis::qt::FabNavigator::loadActivityChanged, &navigator,
            [this](int delta) {
                activity += delta;
                ++activityEvents;
            });
        QObject::connect(&navigator,
            &amrvis::qt::FabNavigator::staleResultDropped, &navigator,
            [this] { ++stale; });
        QObject::connect(&navigator, &amrvis::qt::FabNavigator::openFailed,
            &navigator, [this](const QString& title, const QString& message) {
                failures << title + QStringLiteral(": ") + message;
            });
        QObject::connect(&navigator,
            &amrvis::qt::FabNavigator::windowTitleChanged, &navigator,
            [this] { ++titleChanges; });
    }
};

template <typename Predicate>
void waitFor(QCoreApplication& application, Predicate done, const char* what)
{
    QTimer poll;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timeout, &QTimer::timeout, &application,
        [&application, &timedOut] {
            timedOut = true;
            application.quit();
        });
    QObject::connect(&poll, &QTimer::timeout, &application,
        [&application, &done] {
            if (done()) {
                application.quit();
            }
        });
    poll.start(5);
    timeout.start(5000);
    application.exec();
    require(!timedOut, what);
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    using amrvis::qt::FabNavigator;

    const auto unique
        = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("amrexplorer-fab-navigator-" + std::to_string(unique));
    std::filesystem::create_directories(root);
    const auto fabPath = root / "records.fab";
    writeRawFabFile(fabPath);
    const auto multifabRoot = root / "mf";
    writeMultiFab(multifabRoot);
    const auto multifabPath = multifabRoot / "Cell_H";

    // openPrepared is the one hook the navigator cannot do without.
    {
        bool refused = false;
        try {
            FabNavigator navigator(FabNavigator::Hooks{});
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        require(refused, "a navigator without openPrepared was constructed");
    }

    // A raw FAB source: the selector lists its records as raw records, the
    // navigator is in FAB mode with no source metadata, and reset() clears it.
    {
        Host host;
        FabNavigator navigator(host.hooks());
        host.observe(navigator);
        QWidget window;
        auto* dock = navigator.createDock(&window);
        window.show();
        require(navigator.cleared() && !dock->isVisible(),
            "a fresh navigator is not cleared");
        const auto metadata = amrvis::readDatasetMetadata(fabPath);
        auto build = FabNavigator::buildSelector(metadata, fabPath);
        require(build.matched && build.fabMode && !build.hasSourceMetadata
                && build.entries.size() == 2 && build.entries[0].rawRecord
                && build.entries[1].fileOffset > 0,
            "the raw FAB selector build is wrong");
        navigator.applySelectorBuild(build, fabPath, metadata);
        require(navigator.fabMode() && dock->isVisible()
                && dock->entries().size() == 2 && !dock->backAvailable()
                && host.titleChanges == 1 && !navigator.cleared(),
            "applying the raw FAB build did not install it");
        navigator.reset();
        require(navigator.cleared() && !navigator.fabMode()
                && dock->entries().empty() && !dock->isVisible(),
            "reset left FAB state behind");
    }

    // A MultiFab source: blocks open synchronously from the source metadata
    // (one block per opened level, the selector kept, the current spec with
    // level/range reset), the first drill-down records the return state, and
    // Back reopens the source with the spec it was displayed with.
    {
        Host host;
        host.datasetOpen = true;
        FabNavigator navigator(host.hooks());
        host.observe(navigator);
        QWidget window;
        auto* dock = navigator.createDock(&window);
        window.show();
        const auto metadata = amrvis::readDatasetMetadata(multifabPath);
        auto build = FabNavigator::buildSelector(metadata, multifabPath);
        require(build.matched && !build.fabMode && build.hasSourceMetadata
                && build.entries.size() == 2 && !build.entries[0].rawRecord,
            "the MultiFab selector build is wrong");
        navigator.applySelectorBuild(build, multifabPath, metadata);
        require(!navigator.fabMode() && dock->entries().size() == 2,
            "applying the MultiFab build did not install it");
        navigator.viewEntry(1);
        require(host.opened.size() == 1, "viewing a block did not open it");
        const auto& block = host.opened.back();
        require(block.path == multifabPath
                && block.dataRoot == multifabPath.parent_path()
                && block.preserveSelector
                && block.levelBlocks == 1 && block.hadSpec
                && block.levelSelection == -1,
            "the drilled-into block was opened wrongly");
        require(navigator.fabMode() && dock->backAvailable()
                && dock->selectedOrdinal() == std::optional<std::size_t>{1},
            "the selector does not reflect the drilled-into block");
        navigator.viewEntry(5);
        require(host.opened.size() == 1, "an out-of-range entry opened");
        navigator.backToMultiFab();
        require(host.opened.size() == 2, "Back did not reopen the source");
        const auto& source = host.opened.back();
        require(source.path == multifabPath
                && source.dataRoot == multifabPath.parent_path()
                && source.preserveSelector
                && source.levelBlocks == 2 && source.hadSpec
                && source.levelSelection == 2,
            "Back did not restore the source with its display spec");
        require(!navigator.fabMode() && !dock->backAvailable(),
            "Back left FAB mode or the Back button on");
        navigator.backToMultiFab();
        require(host.opened.size() == 2, "a second Back reopened again");
        // A drill-down whose open fails synchronously (here: the host
        // refuses) is reported through openFailed, not a modal box, and the
        // dock goes back to what was displayed -- the source, no Back.
        const auto displayed = dock->selectedOrdinal();
        host.refuseOpens = true;
        navigator.viewEntry(0);
        require(host.opened.size() == 2 && host.failures.size() == 1
                && host.failures.front().startsWith(
                    QStringLiteral("Cannot view FAB: "))
                && !navigator.fabMode() && !dock->backAvailable()
                && dock->selectedOrdinal() == displayed,
            "a synchronously failed drill-down was not reported and rolled back");
        host.refuseOpens = false;
        navigator.viewEntry(0);
        require(host.opened.size() == 3 && navigator.fabMode()
                && dock->selectedOrdinal() == std::optional<std::size_t>{0},
            "the navigator did not recover after a refused open");
    }

    // The direct "Open FAB...": an asynchronous header read that opens with
    // the selector rebuilt (preserve = false), balancing the activity count;
    // a missing file fails once and opens nothing.
    {
        Host host;
        FabNavigator navigator(host.hooks());
        host.observe(navigator);
        navigator.openStandaloneFab(fabPath);
        require(host.activity == 1, "the read did not announce itself");
        waitFor(application, [&] { return host.activityEvents == 2; },
            "the FAB read did not finish");
        require(host.activity == 0 && host.opened.size() == 1
                && host.opened.back().fileVersion == "FAB"
                && host.opened.back().dataRoot == fabPath.parent_path()
                && !host.opened.back().preserveSelector
                && !host.opened.back().hadSpec && host.failures.isEmpty(),
            "the raw FAB was not opened as a fresh dataset");
        navigator.openStandaloneFab(root / "missing.fab");
        waitFor(application, [&] { return host.activityEvents == 4; },
            "the failing FAB read did not finish");
        require(host.activity == 0 && host.opened.size() == 1
                && host.failures.size() == 1
                && host.failures.front().startsWith(
                    QStringLiteral("Cannot open FAB: ")),
            "a missing FAB was not reported once");
    }

    // A read that resolves after the dataset generation moved is stale: it
    // neither opens nor reports.
    {
        Host host;
        FabNavigator navigator(host.hooks());
        host.observe(navigator);
        navigator.openStandaloneFab(fabPath);
        ++host.generation;
        waitFor(application, [&] { return host.activityEvents == 2; },
            "the superseded FAB read did not finish");
        require(host.opened.empty() && host.stale == 1
                && host.failures.isEmpty(),
            "a read from a previous dataset generation was not dropped");
    }

    // Raw-record drill-down and rollback: the dock moves to the clicked
    // record at once; a read that fails puts it back and reports; two clicks
    // in flight together return to what was displayed before either.
    {
        Host host;
        host.datasetOpen = true;
        FabNavigator navigator(host.hooks());
        host.observe(navigator);
        QWidget window;
        auto* dock = navigator.createDock(&window);
        window.show();
        const auto metadata = amrvis::readDatasetMetadata(fabPath);
        navigator.applySelectorBuild(
            FabNavigator::buildSelector(metadata, fabPath), fabPath, metadata);
        navigator.viewEntry(0);
        require(dock->selectedOrdinal() == std::optional<std::size_t>{0}
                && navigator.fabMode(),
            "the dock did not move to the clicked record");
        waitFor(application, [&] { return host.activityEvents == 2; },
            "the record read did not finish");
        require(host.opened.size() == 1 && host.opened.back().preserveSelector
                && host.opened.back().hadSpec
                && host.opened.back().levelSelection == -1,
            "the raw record was not opened keeping the selector");
        // Break the source, then click 1: it must fail and roll back to 0.
        std::filesystem::remove(fabPath);
        navigator.viewEntry(1);
        require(dock->selectedOrdinal() == std::optional<std::size_t>{1},
            "the dock did not move to the second record");
        waitFor(application, [&] { return host.activityEvents == 4; },
            "the failing record read did not finish");
        require(host.failures.size() == 1
                && host.failures.front().startsWith(
                    QStringLiteral("Cannot view FAB: "))
                && dock->selectedOrdinal() == std::optional<std::size_t>{0}
                && host.opened.size() == 1,
            "a failed record read did not roll the dock back");
        // Three clicks on record 1 in flight: the first two are superseded
        // (stale, restore nothing), the last fails and returns to what was
        // displayed before any of them -- record 0. Identical clicks matter:
        // the later ones must inherit the pending rollback rather than
        // snapshot the dock, which by then already highlights record 1.
        navigator.viewEntry(1);
        navigator.viewEntry(1);
        navigator.viewEntry(1);
        waitFor(application, [&] { return host.activityEvents == 10; },
            "the overlapping record reads did not finish");
        require(host.stale == 2 && host.failures.size() == 2
                && dock->selectedOrdinal() == std::optional<std::size_t>{0},
            "overlapping failed reads did not return to the displayed record");
        // Shutdown: a late result touches nothing.
        navigator.viewEntry(1);
        host.shuttingDown = true;
        waitFor(application, [&] { return host.activity == 0; },
            "the shutdown-time read did not release its activity");
        require(host.failures.size() == 2 && host.stale == 2,
            "a shutdown-time result was reported");
    }

    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    std::cout << "fab navigator tests passed\n";
    return 0;
}
