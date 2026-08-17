#pragma once

#include "FabSelectorDock.hpp"

#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

class QWidget;

namespace amrvis::qt {

// Everything the FAB selector dock needs for a source, computed off the GUI
// thread (see FabNavigator::buildSelector) so its header scans / per-block
// preads never block the event loop. `matched` distinguishes a recognized FAB
// or single-level-VisMF source (whose mode/source state should be applied)
// from anything else (leave that state untouched, just hide the dock).
struct FabSelectorBuild {
    bool matched = false;
    bool fabMode = false;
    bool hasSourceMetadata = false;
    std::vector<FabSelectorEntry> entries;
    std::filesystem::path root;
};

// Standalone-FAB / MultiFab navigation extracted from MainWindow: the FAB
// mode flag, the source the selector lists (path, data root, metadata), the
// MultiFab-return record, the selector dock, and the asynchronous
// standalone-FAB header reads with their rollback and request token. The host
// opens whatever the navigator resolves (through Hooks::openPrepared, its
// openDatasetImpl), supplies the current frame spec and dataset generation,
// and applies the selector build its own open worker produced.
class FabNavigator final : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        // The host's dataset generation: bumped by every dataset open, so a
        // header read that resolves after one is stale.
        std::function<std::uint64_t()> datasetGeneration;
        // True once application shutdown began; late reads are dropped.
        std::function<bool()> isShuttingDown;
        // The frame spec the host's controls currently describe (its
        // buildFrameSpec), or nullopt when the host cannot say. It is read
        // both for a selected FAB's initial spec and for the MultiFab return
        // record, so it must answer even while a reload has no dataset
        // installed -- the controls still do.
        std::function<std::optional<FrameSliceSpec>()> currentSpec;
        // Opens prepared metadata as the displayed dataset: (path, metadata,
        // dataRoot, preserveSelector, initialSpec) -- the host's
        // openDatasetImpl. preserveSelector is false only for a direct
        // "Open FAB..." (the selector is rebuilt for the new file) and true
        // for every drill-down or return within the current source.
        std::function<void(const std::filesystem::path&, PlotfileMetadataResult,
            std::filesystem::path, bool, std::optional<FrameSliceSpec>)>
            openPrepared;
    };

    FabNavigator(Hooks hooks, QObject* parent = nullptr);

    // The selector dock (hidden until a source has entries), wired to
    // viewEntry/backToMultiFab. Owned by `parent`; the host adds it to a
    // dock area and its View menu.
    FabSelectorDock* createDock(QWidget* parent);

    // Reads FAB/MultiFab record headers and builds the selector entries. Runs
    // on a worker thread; it touches no widgets or navigator state.
    [[nodiscard]] static FabSelectorBuild buildSelector(
        const PlotfileMetadataResult& result, const std::filesystem::path& path);
    // Installs what buildSelector produced for the dataset just opened at
    // `path` (on the GUI thread; only blits, never reads files).
    void applySelectorBuild(FabSelectorBuild build,
        const std::filesystem::path& path,
        const PlotfileMetadataResult& metadata);

    // "Open FAB...": reads the raw file's header off the GUI thread and opens
    // it from the completion. Supplies no rollback: nothing is displayed to
    // fall back to.
    void openStandaloneFab(const std::filesystem::path& path);
    // The dock's view request: a raw record is read asynchronously with the
    // dock moved to it at once (and put back if the read fails); a MultiFab
    // block is opened synchronously from the source metadata, recording the
    // return state the first time.
    void viewEntry(std::size_t index);
    void backToMultiFab();

    // Clears the FAB/MultiFab view state (mode flag, MultiFab-return record,
    // source metadata/paths, pending read) and hides the selector. Any path
    // that replaces the displayed dataset with a non-FAB one -- a plain
    // dataset open, or a plotfile sequence -- must call this or stale FAB
    // state leaks into the new view (see open-sequence-stale-fab-state).
    void reset();

    [[nodiscard]] bool fabMode() const noexcept { return m_fabMode; }
    // For tests: no FAB state at all, dock empty and hidden.
    [[nodiscard]] bool cleared() const;

signals:
    // Background-read bookkeeping for the host's diagnostics (+1 when a
    // header read starts, -1 when its watcher fires) and its stale count.
    void loadActivityChanged(int delta);
    void staleResultDropped();
    // A read failed while it was still the current request; the host reports
    // it non-modally ("<title>: <message>").
    void openFailed(const QString& title, const QString& message);
    // The dock's contents or the mode changed in a way the window title
    // reflects.
    void windowTitleChanged();

private:
    // The selector state a failed standalone-FAB open falls back to: the last
    // one actually committed to the window, not merely highlighted.
    struct SelectorRollback {
        bool fabMode = false;
        bool backAvailable = false;
        std::optional<std::size_t> ordinal;
    };
    // A launched standalone-FAB header read that has not resolved yet,
    // carrying the state to restore if it fails. The pair (generation,
    // requestId) is what makes this safe without any site reaching in to
    // clear it: only the completion holding both may consume the entry, so
    // opening a dataset (which bumps the dataset generation) or tearing the
    // selector down (which bumps the request token) revokes it as a side
    // effect of what it already does. A second click while a read is in
    // flight inherits the pending rollback rather than snapshotting the dock,
    // because what the dock shows then is that pending selection, which was
    // never displayed.
    struct PendingOpen {
        std::uint64_t generation = 0;
        std::uint64_t requestId = 0;
        SelectorRollback rollback;
    };
    // The MultiFab a selected block was drilled into from, and how it was
    // displayed, so Back restores it.
    struct MultiFabReturnState {
        std::filesystem::path path;
        std::filesystem::path dataRoot;
        PlotfileMetadataResult metadata;
        FrameSliceSpec spec;
    };

    void openStandaloneFabAsync(std::filesystem::path path,
        std::optional<std::uint64_t> fileOffset, std::filesystem::path dataRoot,
        bool preserveSelector, std::optional<FrameSliceSpec> initialSpec,
        QString failureTitle, std::optional<SelectorRollback> rollback);
    void applyRollback(const SelectorRollback& rollback);
    [[nodiscard]] std::optional<FrameSliceSpec> selectedEntrySpec() const;
    [[nodiscard]] QWidget* dialogParent() const;

    Hooks m_hooks;
    QPointer<FabSelectorDock> m_dock;
    bool m_fabMode = false;
    std::optional<MultiFabReturnState> m_multifabReturn;
    std::optional<PlotfileMetadataResult> m_sourceMetadata;
    std::filesystem::path m_sourcePath;
    std::filesystem::path m_dataRoot;
    // Newest-wins among overlapping standalone-FAB header reads. The dataset
    // generation alone cannot order them: it is bumped by the open, which only
    // runs once a read has already completed, so two reads in flight together
    // both still match the generation they captured.
    std::uint64_t m_openGeneration = 0;
    std::optional<PendingOpen> m_pendingOpen;
};

} // namespace amrvis::qt
