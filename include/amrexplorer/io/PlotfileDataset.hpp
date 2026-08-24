#pragma once

#include <amrexplorer/cache/BlockKey.hpp>
#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/ParticleReader.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace amrvis {

class PlotfileDataset {
public:
    using BlockCache = ByteLruCache<BlockKey, FabBlock, BlockKeyHash>;

    struct BlockAccess {
        BlockCache::Handle handle;
        bool cacheHit = false;
        BlockReadMetrics io;
    };

    // `derivedFields` are resolved against the plotfile's own field list once,
    // here, and appended to the metadata this dataset presents (see
    // core/DerivedField.hpp); one that does not resolve is left out and
    // reported by skippedDerivedFields() rather than failing the open, so a
    // definition written for another plotfile -- or a sequence frame that
    // happens to lack a field -- still opens. Installing at construction rather than later is what
    // lets the field list be shared without a lock: the block reader and every
    // caller of metadata() see one list that never changes. Editing the
    // definitions therefore means opening a new dataset, which is what the GUI
    // does.
    PlotfileDataset(
        std::filesystem::path plotfile, DatasetId id,
        std::uint64_t cacheBudgetBytes, StopToken cancellation = {},
        std::vector<DerivedFieldDefinition> derivedFields = {});
    PlotfileDataset(std::filesystem::path dataRoot, DatasetId id,
        std::uint64_t cacheBudgetBytes, PlotfileMetadataResult metadata,
        StopToken cancellation = {},
        std::vector<DerivedFieldDefinition> derivedFields = {});

    [[nodiscard]] const DatasetMetadata& metadata() const noexcept;
    [[nodiscard]] const MetadataReadMetrics& metadataReadMetrics() const noexcept;
    // The Header's version *token* -- its first whitespace-delimited word --
    // as the bounded metadata read already parsed it. Exposed so no caller has
    // to open and re-read the Header for it.
    //
    // Narrower than the whole first line, which is what the session used to
    // re-read with its own getline. AMReX writes the version alone on that
    // line ("HyperCLaw-V1.1"), so the two agree on every real plotfile; they
    // differ only where a Header carries something after it, and this is the
    // string the UI's Format field and the wire catalog show.
    [[nodiscard]] const std::string& fileVersion() const noexcept;
    [[nodiscard]] DatasetId id() const noexcept;
    [[nodiscard]] const std::vector<ParticleSpeciesMetadata>& particleSpecies()
        const noexcept;
    [[nodiscard]] ParticleSample requestParticleSample(
        const std::string& species, double fraction, std::uint64_t seed = 0,
        StopToken cancellation = {},
        std::size_t maximumPoints = std::numeric_limits<std::size_t>::max()) const;
    [[nodiscard]] const std::filesystem::path& dataRoot() const noexcept;

    // Whether this field is computed from others rather than read.
    [[nodiscard]] bool isDerivedField(FieldId field) const noexcept;
    // How many of metadata().fields this dataset stores; the derived ones
    // follow them, in the order their definitions were given.
    [[nodiscard]] std::size_t storedFieldCount() const noexcept;
    // The definitions this dataset could not install, with the reason. A
    // definition written for another plotfile does not stop this one from
    // opening (DerivedFieldPolicy::Skip); it lands here instead, for whoever
    // asked for it to say so.
    [[nodiscard]] const std::vector<DerivedFieldSkip>& skippedDerivedFields()
        const noexcept;

    // A block of any field the metadata lists. A derived field's block is
    // evaluated from its inputs' blocks (recursively, so one derived field may
    // read another) and cached like any other, under its own field id.
    [[nodiscard]] BlockAccess requestBlock(
        const BlockRequest& request, StopToken cancellation = {});

    [[nodiscard]] CacheMetrics cacheMetrics() const;
    [[nodiscard]] bool setCacheBudget(std::uint64_t bytes);
    void clearUnpinnedCache();

private:
    // The field list this dataset presents: the stored metadata with one field
    // appended per derived definition, the programs that produce them, and
    // where the stored fields end (so a field id says which it is). Built once,
    // before the block reader is handed the metadata; the plain metadata is
    // shared as-is when there are no derived fields, so the common path copies
    // nothing.
    struct Fields {
        std::shared_ptr<const DatasetMetadata> metadata;
        std::vector<DerivedFieldProgram> programs;
        std::size_t storedCount = 0;
        std::vector<DerivedFieldSkip> skipped;
    };
    [[nodiscard]] static Fields installFields(
        const PlotfileMetadataResult& source,
        std::span<const DerivedFieldDefinition> definitions);
    [[nodiscard]] BlockReadResult readDerivedBlock(const BlockRequest& request,
        const DerivedFieldProgram& program, StopToken cancellation);

    std::filesystem::path m_plotfile;
    DatasetId m_id;
    PlotfileMetadataResult m_metadataResult;
    Fields m_fields;
    std::vector<ParticleSpeciesMetadata> m_particleSpecies;
    PlotfileBlockReader m_blockReader;
    BlockCache m_cache;
    // Serializes this dataset's block reads so concurrent misses of the same
    // block read it once (see the double-checked lookup in requestBlock).
    // Per-dataset, not global: unrelated datasets read in parallel. Acquired
    // with a try_lock/sleep poll loop so a waiter can observe its StopToken
    // instead of blocking a whole block read long. A plain mutex, not
    // std::timed_mutex: libstdc++ implements try_lock_for via
    // pthread_mutex_clocklock, which older ThreadSanitizer runtimes (e.g.
    // GCC 13 on ubuntu-24.04 CI) do not intercept — TSan then misses the
    // lock and reports the destructor's unlock as "unlock of an unlocked
    // mutex". try_lock is intercepted everywhere. (The mutex member makes
    // PlotfileDataset non-movable, which is fine — it is only ever a stack
    // local or held via shared_ptr.)
    std::mutex m_ioMutex;
};

} // namespace amrvis
