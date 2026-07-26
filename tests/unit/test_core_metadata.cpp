#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Statistics.hpp>

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool hasIssue(const std::vector<amrvis::MetadataIssue>& issues,
    const char* pathPrefix)
{
    for (const auto& issue : issues) {
        if (issue.path.rfind(pathPrefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

// Exact-path variant for the negative-path battery: a perturbation may
// cascade a second, derived issue (e.g. an out-of-domain box once its level
// domain is corrupted), so matching the precise path keeps each case pinned
// to the check it targets rather than to a prefix a neighbour also shares.
bool hasExactIssue(const std::vector<amrvis::MetadataIssue>& issues,
    const char* path)
{
    for (const auto& issue : issues) {
        if (issue.path == path) {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    amrvis::DatasetMetadata metadata;
    metadata.dimension = 2;
    metadata.finestLevel = 0;
    metadata.physicalDomain = {{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 0.0}}};
    metadata.fields.push_back({"density", amrvis::Centering::Cell, {"density"}});
    amrvis::LevelMetadata level;
    level.domain = {{{0, 0, 0}}, {{3, 3, 0}}, {{0, 0, 0}}};
    level.cellSize = {{0.25, 0.25, 1.0}};
    level.boxes.push_back(level.domain);
    level.storedComponents = 1;
    level.blocks.push_back({level.domain, "Cell_D_00000", 0,
        amrvis::BlockStatistics{{-2.0}, {7.0}}});
    level.dataPath = "Level_0/Cell";
    metadata.levels.push_back(std::move(level));
    require(amrvis::validateMetadata(metadata).empty(), "valid metadata was rejected");
    const auto range = amrvis::metadataValueRange(metadata, amrvis::FieldId{0});
    require(range && range->minimum == -2.0 && range->maximum == 7.0,
        "metadata range did not aggregate block statistics");
    require(!amrvis::metadataValueRange(metadata, amrvis::FieldId{1}),
        "metadata range accepted an unknown field");

    auto noStatistics = metadata;
    noStatistics.levels[0].blocks[0].statistics.reset();
    require(!amrvis::metadataValueRange(
                noStatistics, amrvis::FieldId{0}),
        "metadata range accepted absent block statistics");

    auto partialStatistics = metadata;
    partialStatistics.levels[0].blocks.push_back(
        {metadata.levels[0].domain, "Cell_D_00001", 4096, std::nullopt});
    require(!amrvis::metadataValueRange(
                partialStatistics, amrvis::FieldId{0}),
        "metadata range ignored a block with missing statistics");

    metadata.fields.push_back(metadata.fields.front());
    require(!amrvis::validateMetadata(metadata).empty(), "duplicate field names were accepted");

    amrvis::LevelMetadata mixed;
    mixed.domain = {{{0, 0, 0}}, {{3, 3, 1}}, {{0, 1, 1}}};
    mixed.indexOrigin = {{0.0, 0.0, 0.0}};
    mixed.cellSize = {{1.0, 2.0, 4.0}};
    require(amrvis::samplePosition(mixed, 0, 0) == 0.5,
        "cell-centered x sample position is wrong");
    require(amrvis::samplePosition(mixed, 1, 0) == 0.0,
        "nodal y sample position is wrong");
    require(amrvis::samplePosition(mixed, 2, 0) == 0.0,
        "nodal z sample position is wrong");
    const auto mixedBounds = amrvis::sampleBounds(mixed, mixed.domain, 3);
    require(mixedBounds.lower[0] == 0.0 && mixedBounds.upper[0] == 4.0,
        "cell-centered sample bounds are wrong");
    require(mixedBounds.lower[1] == -1.0 && mixedBounds.upper[1] == 7.0,
        "nodal y sample bounds are wrong");
    require(mixedBounds.lower[2] == -2.0 && mixedBounds.upper[2] == 6.0,
        "nodal z sample bounds are wrong");
    require(amrvis::sampleIndex(mixed, 0, 0.5) == 0
        && amrvis::sampleIndex(mixed, 1, 0.0) == 0,
        "mixed-centering physical-to-index mapping is wrong");
    require(amrvis::centeringFromIndexType({{0, 1, 1}}, 3)
            == amrvis::Centering::EdgeX,
        "mixed index type was not classified as an x edge");

    amrvis::BlockRequest block;
    require(!amrvis::validateBlockRequest(block).empty(), "invalid block request was accepted");
    block.dataset.value = 1;
    require(amrvis::validateBlockRequest(block).empty(), "valid block request was rejected");

    amrvis::SliceRequest slice;
    slice.dataset.value = 1;
    slice.normalDirection = 1;
    slice.visibleRegion = {{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 0.0}}};
    slice.outputSize = {640, 480};
    require(amrvis::validateSliceRequest(slice, 2).empty(), "valid slice request was rejected");

    // Negative path: a dimension outside [1,3] must be reported without the
    // per-axis geometry checks reading past the fixed-size (3-element)
    // arrays. Pre-fix this loops axis up to dimension-1 over Int3/Real3 and
    // reads out of bounds; running it to completion (especially under the
    // sanitizer build) is the real regression assertion.
    {
        amrvis::DatasetMetadata bad;
        bad.dimension = 5;
        bad.finestLevel = 0;
        bad.physicalDomain = {{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 1.0}}};
        bad.fields.push_back({"phi", amrvis::Centering::Cell, {"phi"}});
        amrvis::LevelMetadata badLevel;
        badLevel.domain = {{{0, 0, 0}}, {{3, 3, 3}}, {{0, 0, 0}}};
        badLevel.cellSize = {{0.25, 0.25, 0.25}};
        badLevel.storedComponents = 1;
        badLevel.boxes.push_back(badLevel.domain);
        badLevel.blocks.push_back({badLevel.domain, "Cell_D_00000", 0,
            amrvis::BlockStatistics{{0.0}, {1.0}}});
        bad.levels.push_back(std::move(badLevel));
        require(hasIssue(amrvis::validateMetadata(bad), "dimension"),
            "an out-of-range dimension was not reported");
    }

    // Negative path: more blocks than boxes must be reported without indexing
    // past the level box array. Pre-fix the block loop reads level.boxes[1]
    // when there is only one box.
    {
        amrvis::DatasetMetadata bad;
        bad.dimension = 2;
        bad.finestLevel = 0;
        bad.physicalDomain = {{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 0.0}}};
        bad.fields.push_back({"phi", amrvis::Centering::Cell, {"phi"}});
        amrvis::LevelMetadata badLevel;
        badLevel.domain = {{{0, 0, 0}}, {{3, 3, 0}}, {{0, 0, 0}}};
        badLevel.cellSize = {{0.25, 0.25, 1.0}};
        badLevel.storedComponents = 1;
        badLevel.boxes.push_back(badLevel.domain);
        badLevel.blocks.push_back({badLevel.domain, "Cell_D_00000", 0,
            amrvis::BlockStatistics{{0.0}, {1.0}}});
        // One more block than boxes: blockIndex 1 has no boxes[1].
        badLevel.blocks.push_back({badLevel.domain, "Cell_D_00001", 4096,
            amrvis::BlockStatistics{{0.0}, {1.0}}});
        bad.levels.push_back(std::move(badLevel));
        require(hasIssue(amrvis::validateMetadata(bad), "levels[0].blocks"),
            "a blocks/boxes size mismatch was not reported");
    }

    // Negative-path battery: a known-good baseline perturbed one way per case.
    // Every check in validateMetadata that lacked a negative test above is
    // exercised here. The baseline must validate clean first, or the cases
    // below prove nothing.
    auto makeValid = [] {
        amrvis::DatasetMetadata m;
        m.dimension = 2;
        m.finestLevel = 0;
        m.physicalDomain = {{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 0.0}}};
        m.fields.push_back({"density", amrvis::Centering::Cell, {"density"}});
        amrvis::LevelMetadata baseLevel;
        baseLevel.level = 0;
        baseLevel.domain = {{{0, 0, 0}}, {{3, 3, 0}}, {{0, 0, 0}}};
        baseLevel.cellSize = {{0.25, 0.25, 1.0}};
        baseLevel.storedComponents = 1;
        baseLevel.boxes.push_back(baseLevel.domain);
        baseLevel.blocks.push_back({baseLevel.domain, "Cell_D_00000", 0,
            amrvis::BlockStatistics{{-2.0}, {7.0}}});
        baseLevel.dataPath = "Level_0/Cell";
        m.levels.push_back(std::move(baseLevel));
        return m;
    };
    require(amrvis::validateMetadata(makeValid()).empty(),
        "the negative-path baseline was itself rejected");

    {
        auto m = makeValid();
        m.finestLevel = -1;
        require(hasExactIssue(amrvis::validateMetadata(m), "finestLevel"),
            "a negative finestLevel was not reported");
    }
    {
        auto m = makeValid();
        m.physicalDomain.upper[0] = 0.0;  // zero extent in x
        require(hasExactIssue(amrvis::validateMetadata(m), "physicalDomain"),
            "a degenerate physical domain was not reported");
    }
    {
        auto m = makeValid();
        m.finestLevel = 1;  // now expects two levels, only one present
        require(hasExactIssue(amrvis::validateMetadata(m), "levels"),
            "a levels-size mismatch was not reported");
    }
    {
        auto m = makeValid();
        m.fields[0].name.clear();
        require(hasExactIssue(amrvis::validateMetadata(m), "fields[0].name"),
            "an empty field name was not reported");
    }
    {
        auto m = makeValid();
        m.fields[0].componentNames = {"a", "b"};
        require(hasExactIssue(
                    amrvis::validateMetadata(m), "fields[0].componentNames"),
            "a multi-entry componentNames was not reported");
    }
    {
        auto m = makeValid();
        m.levels[0].level = 5;  // disagrees with its position
        require(hasExactIssue(amrvis::validateMetadata(m), "levels[0].level"),
            "a level index/position mismatch was not reported");
    }
    {
        auto m = makeValid();
        m.levels[0].domain.lower[0] = 5;  // lower > upper
        require(hasExactIssue(amrvis::validateMetadata(m), "levels[0].domain"),
            "an invalid level domain was not reported");
    }
    {
        auto m = makeValid();
        m.levels[0].domain.centering[0] = 2;  // neither cell (0) nor node (1)
        require(hasExactIssue(
                    amrvis::validateMetadata(m), "levels[0].domain.centering"),
            "an invalid domain index type was not reported");
    }
    {
        auto m = makeValid();
        m.levels[0].cellSize[0] = 0.0;
        require(hasExactIssue(amrvis::validateMetadata(m), "levels[0].cellSize"),
            "a non-positive cell size was not reported");
    }
    {
        auto m = makeValid();
        m.levels[0].indexOrigin[0] = std::numeric_limits<double>::quiet_NaN();
        require(hasExactIssue(amrvis::validateMetadata(m), "levels[0].indexOrigin"),
            "a non-finite index origin was not reported");
    }
    {
        auto m = makeValid();
        // Keep the block box in step so only the box itself is faulted.
        m.levels[0].boxes[0].lower[0] = 5;  // lower > upper
        m.levels[0].blocks[0].box = m.levels[0].boxes[0];
        require(hasExactIssue(amrvis::validateMetadata(m), "levels[0].boxes[0]"),
            "an invalid level box was not reported");
    }
    {
        auto m = makeValid();
        m.levels[0].boxes[0].upper[0] = 5;  // reaches outside the domain (0..3)
        m.levels[0].blocks[0].box = m.levels[0].boxes[0];
        require(hasExactIssue(amrvis::validateMetadata(m), "levels[0].boxes[0]"),
            "a box reaching outside the level domain was not reported");
    }
    {
        auto m = makeValid();
        m.levels[0].boxes[0].centering[0] = 1;  // domain is cell-centered (0)
        m.levels[0].blocks[0].box = m.levels[0].boxes[0];
        require(hasExactIssue(
                    amrvis::validateMetadata(m), "levels[0].boxes[0].centering"),
            "a box/domain centering mismatch was not reported");
    }
    {
        auto m = makeValid();
        m.levels[0].blocks[0].box.upper[0] = 2;  // no longer equals boxes[0]
        require(hasExactIssue(
                    amrvis::validateMetadata(m), "levels[0].blocks[0].box"),
            "a block box disagreeing with its level box was not reported");
    }
    {
        auto m = makeValid();
        m.levels[0].blocks[0].filePath.clear();
        require(hasExactIssue(
                    amrvis::validateMetadata(m), "levels[0].blocks[0].filePath"),
            "an empty block file path was not reported");
    }
    {
        auto m = makeValid();
        m.levels[0].blocks[0].statistics->minimum = {0.0, 1.0};  // 2 != 1 stored
        require(hasExactIssue(
                    amrvis::validateMetadata(m), "levels[0].blocks[0].statistics"),
            "a statistics component-count mismatch was not reported");
    }

    return 0;
}
