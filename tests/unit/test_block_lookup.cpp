#include <amrexplorer/query/detail/BlockLookup.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using amrvis::IntBox;
using amrvis::Int3;
using amrvis::detail::BlockGrid;
using amrvis::detail::LoadedBlock;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// A 2-D block with only its valid box set; BlockGrid::find reads nothing else.
LoadedBlock block(int lo0, int lo1, int hi0, int hi1)
{
    IntBox box;
    box.lower = {{lo0, lo1, 0}};
    box.upper = {{hi0, hi1, 0}};
    box.centering = {{0, 0, 0}};
    return LoadedBlock{box, {}};
}

Int3 point(int a, int b)
{
    return Int3{{a, b, 0}};
}

// The behavior the grid must reproduce exactly: the first (smallest-index)
// block whose valid box contains the point, or -1.
int linearFind(const std::vector<LoadedBlock>& blocks, const Int3& p)
{
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (amrvis::detail::contains(blocks[i].validBox, p, 2)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::uint64_t nextRandom(std::uint64_t& state)
{
    state += 0x9e3779b97f4a7c15ULL;
    auto z = state;
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
}

// Grid and linear scan must agree at every probed point, over the block set's
// bounding box widened by a margin (so out-of-range points are exercised too).
void requireAgrees(const std::vector<LoadedBlock>& blocks,
    const std::array<int, 2>& axes, int lo, int hi, const char* context)
{
    const BlockGrid grid(blocks, axes);
    std::uint64_t rng = 0xabcd1234ULL;
    const auto span = static_cast<std::uint64_t>(hi - lo + 1);
    for (int trial = 0; trial < 20000; ++trial) {
        const auto p = point(
            lo + static_cast<int>(nextRandom(rng) % span),
            lo + static_cast<int>(nextRandom(rng) % span));
        require(grid.find(blocks, p, 2) == linearFind(blocks, p), context);
    }
}

} // namespace

int main()
{
    const std::array<int, 2> axes{0, 1};

    // A default-constructed grid and a grid over no blocks reject every point.
    {
        const BlockGrid empty;
        require(empty.find({}, point(0, 0), 2) == -1,
            "default-constructed grid returned a block");
        const std::vector<LoadedBlock> none;
        const BlockGrid overNone(none, axes);
        require(overNone.find(none, point(3, 4), 2) == -1,
            "grid over no blocks returned a block");
    }

    // A real multi-tile grid: a 4x4 arrangement of 4x4-cell blocks over a 16x16
    // domain forces a multi-tile bin layout (not the 1x1 collapse of a handful
    // of blocks). Every in-domain point must route to its block; out-of-domain
    // points must miss.
    {
        std::vector<LoadedBlock> blocks;
        for (int gj = 0; gj < 4; ++gj) {
            for (int gi = 0; gi < 4; ++gi) {
                blocks.push_back(block(
                    gi * 4, gj * 4, gi * 4 + 3, gj * 4 + 3));
            }
        }
        const BlockGrid grid(blocks, axes);
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const auto expected = (x / 4) + 4 * (y / 4);
                require(grid.find(blocks, point(x, y), 2) == expected,
                    "multi-tile grid routed a point to the wrong block");
            }
        }
        require(grid.find(blocks, point(16, 0), 2) == -1,
            "multi-tile grid matched a point past the domain");
        require(grid.find(blocks, point(-1, -1), 2) == -1,
            "multi-tile grid matched a point below the domain");
        requireAgrees(blocks, axes, -4, 19, "multi-tile differential");
    }

    // Regression for the tile-index overflow: blocks at a far-negative base
    // with a tiny span (so the tile size is 1), then query points far to the
    // positive side. The point sits >2^31 cells above the grid's lower bound,
    // where the pre-fix narrowing cast wrapped negative and indexed the bucket
    // array out of range. It must simply miss.
    {
        constexpr int base = -2000000000;
        std::vector<LoadedBlock> blocks;
        blocks.push_back(block(base, base, base + 1, base + 1));
        blocks.push_back(block(base + 2, base, base + 3, base + 1));
        const BlockGrid grid(blocks, axes);
        require(grid.find(blocks, point(base, base), 2) == 0,
            "negative-base grid missed its own first block");
        require(grid.find(blocks, point(base + 2, base + 1), 2) == 1,
            "negative-base grid missed its own second block");
        // Far-out points: no crash / OOB (ASan would catch it), just a miss.
        require(grid.find(blocks, point(1500000000, 1500000000), 2) == -1,
            "far-positive point was not reported as uncovered");
        require(grid.find(blocks, point(2000000000, base), 2) == -1,
            "far-positive x was not reported as uncovered");
        require(grid.find(blocks, point(base, 2000000000), 2) == -1,
            "far-positive y was not reported as uncovered");
    }

    // Overlap fallback: many identical full-domain boxes exceed the grid's fill
    // cap, so it falls back to a linear scan. The result must still match a
    // linear scan exactly (smallest covering index), and out-of-range misses.
    {
        std::vector<LoadedBlock> blocks;
        for (int i = 0; i < 400; ++i) {
            blocks.push_back(block(0, 0, 63, 63));  // all identical, overlapping
        }
        const BlockGrid grid(blocks, axes);
        require(grid.find(blocks, point(20, 40), 2) == 0,
            "overlap fallback did not return the smallest covering block");
        require(grid.find(blocks, point(64, 0), 2) == -1,
            "overlap fallback matched a point past the domain");
        requireAgrees(blocks, axes, -2, 66, "overlap-fallback differential");
    }

    // Irregular non-overlapping layout (varied block sizes) as a differential
    // property check.
    {
        std::vector<LoadedBlock> blocks;
        blocks.push_back(block(0, 0, 9, 3));
        blocks.push_back(block(0, 4, 3, 9));
        blocks.push_back(block(4, 4, 9, 9));
        blocks.push_back(block(10, 0, 15, 15));
        requireAgrees(blocks, axes, -3, 18, "irregular differential");
    }

    std::cout << "block lookup tests passed\n";
    return 0;
}
