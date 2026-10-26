#pragma once

#include <amrexplorer/cache/BlockKey.hpp>
#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/BlockSource.hpp>
#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/ParticleReader.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace amrvis {

// One open dataset, whatever its format: the BlockSource that reads it, a
// byte-budgeted cache of its blocks, and the derived fields computed from
// them. Everything above src/io (the queries, the local session) works on
// this, so a new format is a new BlockSource and nothing more.
class AmrDataset {
public:
    using BlockCache = ByteLruCache<BlockKey, FabBlock, BlockKeyHash>;

    struct BlockAccess {
        BlockCache::Handle handle;
        bool cacheHit = false;
        BlockReadMetrics io;
    };

    // `derivedFields` are resolved against the source's own field list once,
    // here, and appended to the metadata this dataset presents (see
    // core/DerivedField.hpp); one that does not resolve is left out and
    // reported by skippedDerivedFields() rather than failing the open, so a
    // definition written for another dataset -- or a sequence frame that
    // happens to lack a field -- still opens. Installing at construction rather
    // than later is what lets the field list be shared without a lock: every
    // caller of metadata() sees one list that never changes. Editing the
    // definitions therefore means opening a new dataset, which is what the GUI
    // does.
    AmrDataset(std::shared_ptr<const BlockSource> source, DatasetId id,
        std::uint64_t cacheBudgetBytes,
        std::vector<DerivedFieldDefinition> derivedFields = {});
    virtual ~AmrDataset() = default;

    // The mutex makes a dataset non-movable, which is fine -- it is only ever
    // a stack local or held via shared_ptr.
    AmrDataset(const AmrDataset&) = delete;
    AmrDataset& operator=(const AmrDataset&) = delete;

    [[nodiscard]] const DatasetMetadata& metadata() const noexcept;
    [[nodiscard]] const MetadataReadMetrics& metadataReadMetrics() const noexcept;
    [[nodiscard]] const std::string& fileVersion() const noexcept;
    [[nodiscard]] DatasetId id() const noexcept;
    [[nodiscard]] const std::vector<ParticleSpeciesMetadata>& particleSpecies()
        const noexcept;
    [[nodiscard]] ParticleSample requestParticleSample(
        const std::string& species, double fraction, std::uint64_t seed = 0,
        StopToken cancellation = {},
        std::size_t maximumPoints = std::numeric_limits<std::size_t>::max(),
        std::optional<ParticleAttribute> attribute = std::nullopt) const;

    // Whether this field is computed from others rather than read.
    [[nodiscard]] bool isDerivedField(FieldId field) const noexcept;
    // How many of metadata().fields this dataset stores; the derived ones
    // follow them, in the order their definitions were given.
    [[nodiscard]] std::size_t storedFieldCount() const noexcept;
    // The definitions this dataset could not install, with the reason. A
    // definition written for another dataset does not stop this one from
    // opening (DerivedFieldPolicy::Skip); it lands here instead, for whoever
    // asked for it to say so.
    [[nodiscard]] const std::vector<DerivedFieldSkip>& skippedDerivedFields()
        const noexcept;
    // The definitions this dataset was opened with, installed or not. What a
    // caller holding a newer list compares against to find out whether this
    // session is still the one that list describes -- which cannot be
    // inferred from the field names, since an expression can change under a
    // name that does not.
    [[nodiscard]] const std::vector<DerivedFieldDefinition>&
    derivedFieldDefinitions() const noexcept;

    // A block of any field the metadata lists. A derived field's block is
    // evaluated from its inputs' blocks (recursively, so one derived field may
    // read another) and cached like any other, under its own field id.
    [[nodiscard]] BlockAccess requestBlock(
        const BlockRequest& request, StopToken cancellation = {});

    // The mapped-grid hierarchy as a dataset of its own, or null when the
    // source carries none. It shares this dataset's id, so a request built for
    // one is valid for the other, and reads through its own block cache (a
    // fixed slice of this dataset's budget) so node positions do not evict
    // field blocks. It carries no particles, no derived fields and no nested
    // grid.
    [[nodiscard]] std::shared_ptr<AmrDataset> mappedGrid() const noexcept;

    // The block pool this dataset reports; the mapped grid's pool follows the
    // budget (setCacheBudget) and the clear, but is not part of the metrics.
    [[nodiscard]] CacheMetrics cacheMetrics() const;
    [[nodiscard]] bool setCacheBudget(std::uint64_t bytes);
    void clearUnpinnedCache();

private:
    [[nodiscard]] static std::shared_ptr<AmrDataset> makeMappedGrid(
        const BlockSource& source, DatasetId id,
        std::uint64_t cacheBudgetBytes);
    [[nodiscard]] static std::uint64_t mappedGridCacheBudget(
        std::uint64_t cacheBudgetBytes) noexcept;

    // The field list this dataset presents: the stored metadata with one field
    // appended per derived definition, the programs that produce them, and
    // where the stored fields end (so a field id says which it is). Built once,
    // at construction; the plain metadata is shared as-is when there are no
    // derived fields, so the common path copies nothing.
    struct Fields {
        std::shared_ptr<const DatasetMetadata> metadata;
        std::vector<DerivedFieldProgram> programs;
        std::size_t storedCount = 0;
        std::vector<DerivedFieldSkip> skipped;
        // As given, not as installed: a caller comparing against its own list
        // is asking what this session was built from.
        std::vector<DerivedFieldDefinition> definitions;
    };
    [[nodiscard]] static Fields installFields(
        const std::shared_ptr<const DatasetMetadata>& stored,
        std::span<const DerivedFieldDefinition> definitions);
    [[nodiscard]] BlockReadResult readDerivedBlock(const BlockRequest& request,
        const DerivedFieldProgram& program, StopToken cancellation);

    std::shared_ptr<const BlockSource> m_source;
    DatasetId m_id;
    Fields m_fields;
    BlockCache m_cache;
    std::shared_ptr<AmrDataset> m_mappedGrid;
    // Serializes this dataset's block reads so concurrent misses of the same
    // block read it once (see the double-checked lookup in requestBlock).
    // Per-dataset, not global: unrelated datasets read in parallel. Acquired
    // with a try_lock/sleep poll loop so a waiter can observe its StopToken
    // instead of blocking a whole block read long. A plain mutex, not
    // std::timed_mutex: libstdc++ implements try_lock_for via
    // pthread_mutex_clocklock, which older ThreadSanitizer runtimes (e.g.
    // GCC 13 on ubuntu-24.04 CI) do not intercept — TSan then misses the
    // lock and reports the destructor's unlock as "unlock of an unlocked
    // mutex". try_lock is intercepted everywhere.
    std::mutex m_ioMutex;
};

} // namespace amrvis
