#include "FabNavigator.hpp"

#include "QtErrorText.hpp"

#include <amrexplorer/io/FabCatalog.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>
#include <amrexplorer/io/detail/FabHeaderParsing.hpp>

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QtConcurrent>

#include <exception>
#include <stdexcept>
#include <utility>

namespace amrvis::qt {

FabNavigator::FabNavigator(Hooks hooks, QObject* parent)
    : QObject(parent)
    , m_hooks(std::move(hooks))
{
    // The other hooks default sensibly when unset (see Hooks); this one is
    // what every navigation ends in, so it is called bare at three sites.
    if (!m_hooks.openPrepared) {
        throw std::invalid_argument("FabNavigator needs an openPrepared hook");
    }
}

FabSelectorDock* FabNavigator::createDock(QWidget* parent)
{
    auto* dock = new FabSelectorDock(parent);
    dock->setVisible(false);
    connect(dock, &FabSelectorDock::viewRequested, this,
        [this](std::size_t entry) { viewEntry(entry); });
    connect(dock, &FabSelectorDock::backRequested, this,
        [this] { backToMultiFab(); });
    m_dock = dock;
    return dock;
}

FabSelectorBuild FabNavigator::buildSelector(
    const PlotfileMetadataResult& result, const std::filesystem::path& path)
{
    const auto precisionLabel = [](FabRealPrecision precision) {
        return precision == FabRealPrecision::Single
            ? QCoreApplication::translate("MainWindow", "IEEE-32")
            : QCoreApplication::translate("MainWindow", "IEEE-64");
    };

    FabSelectorBuild build;
    build.root = path.parent_path();
    if (build.root.empty()) {
        build.root = ".";
    }

    if (result.fileVersion == "FAB") {
        const auto records = scanFabFile(path);
        build.entries.reserve(records.size());
        for (const auto& record : records) {
            build.entries.push_back({
                .ordinal = record.ordinal,
                .level = 0,
                .blockIndex = record.ordinal,
                .filePath = path,
                .fileOffset = record.headerOffset,
                .validBox = record.storedBox,
                .storedBox = record.storedBox,
                .dimension = record.dimension,
                .components = record.components,
                .precision = precisionLabel(record.precision),
                .rawRecord = true
            });
        }
        build.matched = true;
        build.fabMode = true;
        build.hasSourceMetadata = false;
    } else if (result.fileVersion.starts_with("VisMF-")
        && result.metadata->levels.size() == 1) {
        const auto& metadata = *result.metadata;
        const auto& level = metadata.levels.front();
        build.entries.reserve(level.blocks.size());
        for (std::size_t index = 0; index < level.blocks.size(); ++index) {
            const auto& block = level.blocks[index];
            // Overflow-guarded shared grow (this copy previously used
            // plain int).
            auto storedBox = amrvis::detail::grownBox<MetadataReadError>(
                block.box, level.ghostWidth, metadata.dimension);
            auto precision = FabRealPrecision::Double;
            if (level.visMfHeaderVersion == 1) {
                const auto record = inspectFabRecord(
                    build.root / block.filePath, block.fileOffset);
                storedBox = record.storedBox;
                precision = record.precision;
            } else {
                precision = fabPrecisionFromDescriptor(level.realDescriptor);
            }
            build.entries.push_back({
                .ordinal = index,
                .level = level.level,
                .blockIndex = index,
                .filePath = build.root / block.filePath,
                .fileOffset = block.fileOffset,
                .validBox = block.box,
                .storedBox = storedBox,
                .dimension = metadata.dimension,
                .components = level.storedComponents,
                .precision = precisionLabel(precision),
                .rawRecord = false
            });
        }
        build.matched = true;
        build.fabMode = false;
        build.hasSourceMetadata = true;
    }
    return build;
}

void FabNavigator::applySelectorBuild(FabSelectorBuild build,
    const std::filesystem::path& path, const PlotfileMetadataResult& metadata)
{
    if (build.matched) {
        m_fabMode = build.fabMode;
        if (build.hasSourceMetadata) {
            m_sourceMetadata = metadata;
        } else {
            m_sourceMetadata.reset();
        }
    }
    if (build.entries.empty()) {
        if (m_dock) {
            m_dock->setVisible(false);
        }
        return;
    }
    m_sourcePath = path;
    m_dataRoot = build.root;
    if (m_dock) {
        m_dock->setEntries(std::move(build.entries));
        m_dock->setBackAvailable(false);
        m_dock->setVisible(true);
        m_dock->raise();
    }
    emit windowTitleChanged();
}

void FabNavigator::openStandaloneFab(const std::filesystem::path& path)
{
    auto root = path.parent_path();
    if (root.empty()) {
        root = ".";
    }
    openStandaloneFabAsync(path, std::nullopt, std::move(root), false,
        std::nullopt, tr("Cannot open FAB"), std::nullopt);
}

void FabNavigator::applyRollback(const SelectorRollback& rollback)
{
    m_fabMode = rollback.fabMode;
    if (!m_dock) {
        return;
    }
    m_dock->setBackAvailable(rollback.backAvailable);
    if (rollback.ordinal) {
        m_dock->selectEntry(*rollback.ordinal);
    } else {
        m_dock->clearSelection();
    }
}

std::optional<FrameSliceSpec> FabNavigator::selectedEntrySpec() const
{
    auto spec = m_hooks.currentSpec ? m_hooks.currentSpec() : std::nullopt;
    if (spec) {
        // The selected FAB shows its own whole level at the file range; the
        // rest of the display state (field, palette, zoom, ...) carries over.
        spec->levelSelection = -1;
        spec->rangeMode = RangeMode::File;
        spec->userRange.reset();
    }
    return spec;
}

void FabNavigator::openStandaloneFabAsync(std::filesystem::path path,
    std::optional<std::uint64_t> fileOffset, std::filesystem::path dataRoot,
    bool preserveSelector, std::optional<FrameSliceSpec> initialSpec,
    QString failureTitle, std::optional<SelectorRollback> rollback)
{
    emit loadActivityChanged(1);
    const auto generation
        = m_hooks.datasetGeneration ? m_hooks.datasetGeneration() : 0;
    // Two of these can be in flight at once -- clicking a second raw record
    // while the first is still reading, which is reachable precisely because
    // the read does not freeze the GUI. Without a per-request token both
    // completions match the generation they captured and the *first* to
    // arrive opens, while the selector already shows the second: oldest-wins,
    // and the window disagrees with the dock.
    //
    // Inheritance is decided here rather than at the call sites, because
    // every request supersedes whatever was live -- including the direct-open
    // entry point, which brings no rollback of its own. The read it supersedes
    // will retire without restoring anything, so if this one fails the state
    // to return to is still the one that was last displayed. Checked before
    // the token moves, since moving it is what retires the other request.
    const bool supersedesLive = m_pendingOpen
        && m_pendingOpen->generation == generation
        && m_pendingOpen->requestId == m_openGeneration;
    if (supersedesLive) {
        rollback = m_pendingOpen->rollback;
    }
    const auto requestId = ++m_openGeneration;
    if (rollback) {
        m_pendingOpen = PendingOpen{generation, requestId, *rollback};
    } else {
        // Nothing live and nothing to fall back to: this request owns the slot
        // and a failure of it has nowhere to return to.
        m_pendingOpen.reset();
    }
    auto* watcher = new QFutureWatcher<PlotfileMetadataResult>(this);
    connect(watcher, &QFutureWatcher<PlotfileMetadataResult>::finished, this,
        [this, watcher, generation, requestId, path,
            dataRoot = std::move(dataRoot), preserveSelector,
            initialSpec = std::move(initialSpec),
            failureTitle = std::move(failureTitle)]() mutable {
            emit loadActivityChanged(-1);
            if (m_hooks.isShuttingDown && m_hooks.isShuttingDown()) {
                watcher->deleteLater();
                return;
            }
            // A dataset opened while this read was in flight owns the window
            // now, and so does a newer FAB read or a selector teardown;
            // publishing over any of them would be a stale result.
            const auto currentGeneration
                = m_hooks.datasetGeneration ? m_hooks.datasetGeneration() : 0;
            const bool current = generation == currentGeneration
                && requestId == m_openGeneration;
            // Only the completion that recorded the pending entry may consume
            // it. Anything that revoked it -- a dataset open, a teardown, a
            // newer click -- has already made `current` false. Taken out of
            // the member up front, not read back later: this completion
            // decides the entry's fate either way, and the open below can
            // throw *after* the success path has given the entry up.
            std::optional<SelectorRollback> owned;
            if (current && m_pendingOpen
                && m_pendingOpen->generation == generation
                && m_pendingOpen->requestId == requestId) {
                owned = m_pendingOpen->rollback;
                m_pendingOpen.reset();
            }
            try {
                auto metadata = watcher->future().takeResult();
                if (current) {
                    m_hooks.openPrepared(path, std::move(metadata),
                        std::move(dataRoot), preserveSelector,
                        std::move(initialSpec));
                } else {
                    emit staleResultDropped();
                }
            } catch (const std::exception& error) {
                if (current) {
                    // The caller may have moved the selector to this FAB
                    // before the read returned; the open did not happen, so
                    // put it back before saying so.
                    if (owned) {
                        applyRollback(*owned);
                    }
                    emit openFailed(failureTitle, exceptionMessage(error));
                } else {
                    emit staleResultDropped();
                }
            }
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([path, fileOffset] {
        return fileOffset
            ? StandaloneMetadataReader{}.readFab(path, *fileOffset)
            : StandaloneMetadataReader{}.readFab(path);
    }));
}

void FabNavigator::viewEntry(std::size_t index)
{
    if (!m_dock) {
        return;
    }
    const auto& entries = m_dock->entries();
    if (index >= entries.size()) {
        return;
    }
    const auto entry = entries[index];
    // Set once the synchronous path below has moved the dock to the entry, so
    // a failure after that puts it back (the asynchronous path carries its
    // own rollback through the pending read).
    std::optional<SelectorRollback> syncRollback;
    try {
        auto selectedSpec = selectedEntrySpec();
        if (entry.rawRecord) {
            // The record's own header has to be read; do it off the GUI thread
            // and let the completion open it. The selector state is moved to
            // the pending record immediately so the dock reflects the choice
            // without waiting for the read -- but only a read that succeeds
            // actually changes what the window displays, so a failure has to
            // put the previous state back rather than leave the dock claiming
            // a FAB that is not on screen.
            //
            // Snapshot what is displayed now; openStandaloneFabAsync prefers a
            // still-live pending rollback over it, because from a displayed
            // FAB X, clicking A then B and having B fail must return to X, not
            // to the A the dock is merely showing -- A lost the request token
            // and will never open.
            const SelectorRollback rollback{m_fabMode, m_dock->backAvailable(),
                m_dock->selectedOrdinal()};
            m_fabMode = true;
            m_dock->setBackAvailable(m_multifabReturn.has_value());
            m_dock->selectEntry(entry.ordinal);
            openStandaloneFabAsync(m_sourcePath, entry.fileOffset, m_dataRoot,
                true, std::move(selectedSpec), tr("Cannot view FAB"), rollback);
            return;
        }
        if (!m_sourceMetadata) {
            throw std::runtime_error(
                "the source MultiFab is no longer available");
        }
        // The first drill-down records where Back returns to. The spec is
        // read now, while the source is still displayed, but the record is
        // kept only once the FAB has opened: a failed drill-down must not
        // leave one behind, or a later drill-down (after the user has moved
        // the slice controls) would find it and skip re-capture, and Back
        // would land on the spec of the failed attempt.
        std::optional<MultiFabReturnState> firstReturn;
        if (!m_multifabReturn) {
            auto returnSpec
                = m_hooks.currentSpec ? m_hooks.currentSpec() : std::nullopt;
            firstReturn = MultiFabReturnState{m_sourcePath, m_dataRoot,
                *m_sourceMetadata, returnSpec.value_or(FrameSliceSpec{})};
        }
        auto selected = makeSelectedFabMetadata(*m_sourceMetadata->metadata,
            entry.level, entry.blockIndex, m_dataRoot);
        syncRollback = SelectorRollback{
            m_fabMode, m_dock->backAvailable(), m_dock->selectedOrdinal()};
        m_fabMode = true;
        m_dock->setBackAvailable(true);  // one of the two is about to hold
        m_dock->selectEntry(entry.ordinal);
        m_hooks.openPrepared(m_sourcePath, std::move(selected), m_dataRoot,
            true, std::move(selectedSpec));
        if (firstReturn) {
            m_multifabReturn = std::move(firstReturn);
        }
    } catch (const std::exception& error) {
        if (syncRollback) {
            applyRollback(*syncRollback);
        }
        // Reported the same way as an asynchronous read failure: non-modally
        // through the host. (A modal box here never returns under the
        // offscreen platform, so a regression would hang the unit test rather
        // than fail it.)
        emit openFailed(tr("Cannot view FAB"), exceptionMessage(error));
    }
}

void FabNavigator::backToMultiFab()
{
    if (!m_multifabReturn) {
        return;
    }
    auto state = std::move(*m_multifabReturn);
    m_multifabReturn.reset();
    m_fabMode = false;
    if (m_dock) {
        m_dock->setBackAvailable(false);
    }
    m_hooks.openPrepared(state.path, std::move(state.metadata),
        std::move(state.dataRoot), true, std::move(state.spec));
}

void FabNavigator::reset()
{
    // Belt-and-braces, not the guarantee. Tearing the selector down has to
    // revoke any read still in flight against it, but the dataset generation
    // already does that at both call sites: openDatasetImpl bumps it further
    // down the same straight-line body, and prepareSequence's callers reach
    // MainWindow's frameSwitchStarted handler -- a direct connection, so the
    // same event-loop slot -- which bumps it too. No completion can be
    // delivered in between, so the generation check is already false for
    // every earlier read and this token bump has never been what retires one.
    // It is kept because it is cheap and because relying on a bump that
    // happens a frame up the stack, in a signal handler, is not a property
    // this function can see.
    ++m_openGeneration;
    m_pendingOpen.reset();
    m_fabMode = false;
    m_multifabReturn.reset();
    m_sourceMetadata.reset();
    m_sourcePath.clear();
    m_dataRoot.clear();
    if (m_dock) {
        m_dock->setEntries({});
        m_dock->setBackAvailable(false);
        m_dock->setVisible(false);
    }
}

bool FabNavigator::cleared() const
{
    return !m_fabMode && !m_multifabReturn && !m_sourceMetadata
        && m_sourcePath.empty() && m_dataRoot.empty()
        && (!m_dock || (m_dock->entries().empty() && !m_dock->isVisible()));
}

} // namespace amrvis::qt
