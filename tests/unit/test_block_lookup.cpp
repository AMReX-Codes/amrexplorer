#include <amrexplorer/query/detail/BlockLookup.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
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
int linearFind(
    const std::vector<LoadedBlock>& blocks, const Int3& p, int dimension = 2)
{
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (amrvis::detail::contains(blocks[i].validBox, p, dimension)) {
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
// `lo`/`hi` bound the probe range on every axis of `dimension`.
void requireAgreesWith(const BlockGrid& grid,
    const std::vector<LoadedBlock>& blocks, int dimension, int lo, int hi,
    const char* context)
{
    std::uint64_t rng = 0xabcd1234ULL;
    const auto span = static_cast<std::uint64_t>(hi - lo + 1);
    for (int trial = 0; trial < 20000; ++trial) {
        Int3 p{};
        for (int axis = 0; axis < dimension; ++axis) {
            p[static_cast<std::size_t>(axis)]
                = lo + static_cast<int>(nextRandom(rng) % span);
        }
        require(grid.find(blocks, p, dimension)
                == linearFind(blocks, p, dimension),
            context);
    }
}

void requireAgrees(const std::vector<LoadedBlock>& blocks,
    const std::array<int, 2>& axes, int lo, int hi, const char* context)
{
    requireAgreesWith(BlockGrid(blocks, axes), blocks, 2, lo, hi, context);
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

    // Overlap fallback: a degenerate catalog where 150 full-domain boxes sit
    // over 250 single-cell ones. The single cells (the majority) set the
    // median extent, so tiles are cells, every big box lands in every tile,
    // and the fill cap trips; the grid must fall back to a linear scan whose
    // result still matches a linear scan exactly (smallest covering index).
    {
        std::vector<LoadedBlock> blocks;
        for (int i = 0; i < 150; ++i) {
            blocks.push_back(block(0, 0, 63, 63));  // identical, overlapping
        }
        for (int i = 0; i < 250; ++i) {
            blocks.push_back(block(i % 64, i / 64, i % 64, i / 64));
        }
        const BlockGrid grid(blocks, axes);
        require(!grid.usesIndex(),
            "overlapping catalog did not trip the fill cap");
        require(grid.find(blocks, point(20, 40), 2) == 0,
            "overlap fallback did not return the smallest covering block");
        require(grid.find(blocks, point(64, 0), 2) == -1,
            "overlap fallback matched a point past the domain");
        requireAgreesWith(grid, blocks, 2, -2, 66,
            "overlap-fallback differential");
    }

    // Identical boxes alone must not trip the fill cap (memory-boundedness is
    // what this pins): with tiles sized to the blocks they all share one tile,
    // and find() there is the bucket scan over all of them -- the linear scan
    // in all but name, which is the right answer for a fully overlapping set.
    {
        std::vector<LoadedBlock> blocks;
        for (int i = 0; i < 400; ++i) {
            blocks.push_back(block(0, 0, 63, 63));
        }
        const BlockGrid grid(blocks, axes);
        require(grid.usesIndex(), "identical boxes tripped the fill cap");
        require(grid.find(blocks, point(20, 40), 2) == 0,
            "identical boxes did not resolve to the smallest index");
    }

    // A pencil decomposition -- 1000 non-overlapping 1x1000 columns -- is an
    // ordinary layout, not overlap: the index must be kept (square tiles would
    // list every column in dozens of buckets and trip the cap) and route each
    // point to its column.
    {
        std::vector<LoadedBlock> blocks;
        for (int i = 0; i < 1000; ++i) {
            blocks.push_back(block(i, 0, i, 999));
        }
        const BlockGrid grid(blocks, axes);
        require(grid.usesIndex(), "pencil layout tripped the fill cap");
        for (int x = 0; x < 1000; x += 7) {
            require(grid.find(blocks, point(x, 500), 2) == x,
                "pencil layout routed a point to the wrong column");
        }
        requireAgrees(blocks, axes, -3, 1002, "pencil differential");
    }

    // A sparse level: a few small blocks scattered over a huge span. The tile
    // count must be capped near the block count (not one tile per block-sized
    // cell of empty space) while lookups stay exact.
    {
        std::vector<LoadedBlock> blocks;
        std::uint64_t rng = 0x5ca7'7e12ULL;
        for (int i = 0; i < 16; ++i) {
            const auto x = static_cast<int>(nextRandom(rng) % 4088);
            const auto y = static_cast<int>(nextRandom(rng) % 4088);
            blocks.push_back(block(x, y, x + 7, y + 7));
        }
        const BlockGrid grid(blocks, axes);
        require(grid.usesIndex(), "sparse layout tripped the fill cap");
        requireAgrees(blocks, axes, -8, 4103, "sparse differential");
    }

    // The line query's single-axis grid: 4096 blocks along x that all straddle
    // the pinned y/z cell. Binning y as well would list every block in every
    // y tile and trip the cap; binning x alone must keep the index and route
    // each sample to its block.
    {
        std::vector<LoadedBlock> blocks;
        for (int i = 0; i < 4096; ++i) {
            IntBox box;
            box.lower = {{16 * i, 0, 0}};
            box.upper = {{16 * i + 15, 15, 15}};
            box.centering = {{0, 0, 0}};
            blocks.push_back(LoadedBlock{box, {}});
        }
        const BlockGrid grid(blocks, 0);
        require(grid.usesIndex(), "single-axis line grid tripped the fill cap");
        for (int x = 0; x < 16 * 4096; x += 1001) {
            require(grid.find(blocks, Int3{{x, 8, 8}}, 3) == x / 16,
                "single-axis grid routed a sample to the wrong block");
        }
        require(grid.find(blocks, Int3{{40, 16, 8}}, 3) == -1,
            "single-axis grid matched a point off the pinned cell");
        require(grid.find(blocks, Int3{{-1, 8, 8}}, 3) == -1,
            "single-axis grid matched a point before the first block");
        // Differential over random y/z as well as x: the unbinned coordinates
        // are checked by contains(), not by the tiles, and must still be.
        {
            std::vector<LoadedBlock> few;
            for (int i = 0; i < 40; ++i) {
                IntBox box;
                box.lower = {{16 * i, i % 3, i % 5}};
                box.upper = {{16 * i + 15, i % 3 + 12, i % 5 + 10}};
                box.centering = {{0, 0, 0}};
                few.push_back(LoadedBlock{box, {}});
            }
            requireAgreesWith(BlockGrid(few, 0), few, 3, -4, 660,
                "single-axis differential");
        }
    }

    // Single-axis overlap fallback: many blocks straddling one cell on the
    // binned axis, with tiles set to one cell by a majority of single-cell
    // blocks, trip the cap; the fallback must still be exact.
    {
        std::vector<LoadedBlock> blocks;
        for (int i = 0; i < 100; ++i) {
            blocks.push_back(block(0, 0, 63, 3));  // 64 cells on x, overlapping
        }
        for (int i = 0; i < 200; ++i) {
            blocks.push_back(block(i % 64, 0, i % 64, 3));  // single cells
        }
        const BlockGrid grid(blocks, 0);
        require(!grid.usesIndex(),
            "single-axis overlapping catalog did not trip the fill cap");
        requireAgreesWith(grid, blocks, 2, -2, 66,
            "single-axis fallback differential");
    }

    // The ordinary AMR shape: two refined regions far apart. The tiles coarsen
    // to fit the bounding box (dozens of blocks per occupied bucket), but the
    // index must be kept and stay exact; averaged over points spread across
    // the bounding box the candidates per lookup are still bounded by
    // blocks/tiles (see the BlockGrid comment).
    {
        std::vector<LoadedBlock> blocks;
        for (const int corner : {0, 4096 - 16 * 16}) {
            for (int gj = 0; gj < 16; ++gj) {
                for (int gi = 0; gi < 16; ++gi) {
                    blocks.push_back(block(corner + 16 * gi, corner + 16 * gj,
                        corner + 16 * gi + 15, corner + 16 * gj + 15));
                }
            }
        }
        const BlockGrid grid(blocks, axes);
        require(grid.usesIndex(), "two-cluster layout tripped the fill cap");
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const auto& box = blocks[i].validBox;
            require(grid.find(blocks, point(box.lower[0] + 3, box.upper[1] - 2), 2)
                    == static_cast<int>(i),
                "two-cluster layout routed a point to the wrong block");
        }
        requireAgreesWith(grid, blocks, 2, -8, 4103,
            "two-cluster differential");
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

    // The axis selection: a slice bins on its plane axes -- {1, 2}, {0, 2},
    // {0, 1} -- so the pairs are not only {0, 1}, and equal axes are the
    // single-axis mode. 3-D blocks laid out along z, indexed on {2, 0} and
    // {1, 2}, and the same-axis case {2, 2}, each against the linear scan.
    {
        std::vector<LoadedBlock> blocks;
        for (int k = 0; k < 6; ++k) {
            IntBox box;
            box.lower = {{0, k % 2 == 0 ? 0 : 4, 8 * k}};
            box.upper = {{7, k % 2 == 0 ? 3 : 7, 8 * k + 7}};
            box.centering = {{0, 0, 0}};
            blocks.push_back(LoadedBlock{box, {}});
        }
        for (const std::array<int, 2> gridAxes : {std::array<int, 2>{2, 0},
                 std::array<int, 2>{1, 2}, std::array<int, 2>{2, 2}}) {
            const BlockGrid grid(blocks, gridAxes);
            std::uint64_t rng = 0x5eed'0000ULL;
            for (int trial = 0; trial < 20000; ++trial) {
                const Int3 p{{-2 + static_cast<int>(nextRandom(rng) % 12),
                    -2 + static_cast<int>(nextRandom(rng) % 12),
                    -4 + static_cast<int>(nextRandom(rng) % 56)}};
                require(grid.find(blocks, p, 3) == linearFind(blocks, p, 3),
                    "grid on non-default axes disagrees with a linear scan");
            }
        }
    }

    // A grid refuses a block set other than the one it indexes.
    {
        std::vector<LoadedBlock> two;
        two.push_back(block(0, 0, 3, 3));
        two.push_back(block(4, 0, 7, 3));
        std::vector<LoadedBlock> one;
        one.push_back(block(0, 0, 3, 3));
        const BlockGrid grid(two, axes);
        bool rejected = false;
        try {
            static_cast<void>(grid.find(one, point(1, 1), 2));
        } catch (const std::logic_error&) {
            rejected = true;
        }
        require(rejected, "grid accepted a block set of the wrong size");
    }

    // Degenerate boxes (upper < lower on a binned axis) contain no point, and
    // must neither be found nor break the build: an all-inverted set yields an
    // empty grid, a mix leaves the inverted ones out (a negative extent would
    // otherwise wrap the CSR entry count and under-allocate the fill), and a
    // box inverted only on an unbinned axis is simply never matched. Each
    // case must agree with the linear scan, which never matches them either.
    {
        std::vector<LoadedBlock> inverted;
        inverted.push_back(block(5, 5, 4, 4));
        const BlockGrid empty(inverted, axes);
        require(empty.find(inverted, point(4, 4), 2) == -1
                && empty.find(inverted, point(5, 5), 2) == -1,
            "an inverted box was matched");
        const BlockGrid emptySingle(inverted, 0);
        require(emptySingle.find(inverted, point(4, 4), 2) == -1,
            "an inverted box was matched by a single-axis grid");

        std::vector<LoadedBlock> mixed;
        mixed.push_back(block(0, 0, 3, 3));
        mixed.push_back(block(4, 0, 7, 3));
        mixed.push_back(block(8, 3, 11, 0));    // inverted on y
        mixed.push_back(block(12, 0, 15, 3));
        mixed.push_back(block(20, 0, 16, 3));   // inverted on x
        for (const std::array<int, 2> gridAxes :
            {std::array<int, 2>{0, 1}, std::array<int, 2>{1, 0},
                std::array<int, 2>{0, 0}}) {
            const BlockGrid grid(mixed, gridAxes);
            require(grid.usesIndex(), "inverted boxes tripped the fill cap");
            for (int y = -1; y <= 4; ++y) {
                for (int x = -1; x <= 21; ++x) {
                    require(grid.find(mixed, point(x, y), 2)
                            == linearFind(mixed, point(x, y)),
                        "grid with inverted boxes disagrees with a linear scan");
                }
            }
        }

        std::vector<LoadedBlock> zInverted;
        IntBox box;
        box.lower = {{0, 0, 3}};
        box.upper = {{3, 3, 0}};  // inverted on z, the unbinned axis
        box.centering = {{0, 0, 0}};
        zInverted.push_back(LoadedBlock{box, {}});
        const BlockGrid grid(zInverted, axes);
        require(grid.find(zInverted, Int3{{1, 1, 0}}, 3) == -1
                && grid.find(zInverted, Int3{{1, 1, 3}}, 3) == -1,
            "a box inverted on the unbinned axis was matched");
    }

    // lookupBlockValue over IndexedBlocks with real cache handles: the value
    // is read at valueOffset within the covering block's FAB and a point
    // outside every block is nullopt. IndexedBlocks validates every block
    // when built -- a FAB whose header box does not cover its catalog box
    // (which would alias into the wrong cell without leaving the payload), a
    // payload shorter than the FAB box (which would read past its end; the
    // reader never produces one, but FabBlock is an aggregate any producer can
    // fill), and a missing payload are each refused there, so the per-hit
    // lookup carries no checks.
    {
        amrvis::PlotfileDataset::BlockCache cache(1 << 20);
        const auto pin = [&cache](int grid, const IntBox& box,
                             std::vector<double> values) {
            auto fab = std::make_shared<amrvis::FabBlock>();
            fab->box = box;
            fab->values = amrvis::FabValues(std::move(values));
            amrvis::BlockKey key;
            key.grid = grid;
            return cache.insertAndPin(key, std::move(fab), 64);
        };
        const auto box2d = [](int x0, int y0, int x1, int y1) {
            IntBox box;
            box.lower = {{x0, y0, 0}};
            box.upper = {{x1, y1, 0}};
            box.centering = {{0, 0, 0}};
            return box;
        };
        std::vector<LoadedBlock> loaded;
        // Block 0: cells x 0..3, y 0..1 -> values 0..7 (x fastest).
        const auto first = box2d(0, 0, 3, 1);
        loaded.push_back({first,
            pin(0, first, {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0})});
        // Block 1: cells x 4..5, y 0..1 -> values 10..13, in a FAB box grown
        // by one cell on every side (ghost cells), as loaded FABs often are.
        const auto second = box2d(4, 0, 5, 1);
        std::vector<double> grown(16, -1.0);
        grown[5] = 10.0;   // (4,0) in the 4x4 grown box (3..6, -1..2)
        grown[6] = 11.0;   // (5,0)
        grown[9] = 12.0;   // (4,1)
        grown[10] = 13.0;  // (5,1)
        loaded.push_back({second, pin(1, box2d(3, -1, 6, 2), grown)});
        const amrvis::detail::IndexedBlocks indexed(2, std::move(loaded), axes);

        const auto at = [&indexed](int x, int y) {
            return amrvis::detail::lookupBlockValue(indexed, point(x, y), 2);
        };
        require(at(0, 0) == 0.0 && at(3, 0) == 3.0 && at(0, 1) == 4.0
                && at(2, 1) == 6.0,
            "lookupBlockValue read the wrong FAB value");
        require(!at(0, 2).has_value() && !at(-1, 0).has_value()
                && !at(6, 0).has_value(),
            "lookupBlockValue returned a value outside every block");
        require(at(4, 0) == 10.0 && at(4, 1) == 12.0 && at(5, 1) == 13.0,
            "lookupBlockValue misread the ghost-grown block");

        // Each rejected shape, at construction, with the right exception:
        // std::runtime_error for a corrupt payload, std::logic_error for the
        // programming error of an unloaded block.
        const auto rejects = [&](const char* what, LoadedBlock block,
                                 bool corrupt) {
            std::vector<LoadedBlock> blocks;
            blocks.push_back(std::move(block));
            bool threw = false;
            try {
                const amrvis::detail::IndexedBlocks bad(2, std::move(blocks), axes);
            } catch (const std::runtime_error&) {
                threw = corrupt;
            } catch (const std::logic_error&) {
                threw = !corrupt;
            }
            require(threw, what);
        };
        // Catalog 4x4, FAB header 2x4: cell (3,0) would be offset 3 < 8, i.e.
        // cell (1,1)'s value, if only the payload size were checked.
        rejects("a FAB header box smaller than its catalog box was accepted",
            {box2d(0, 0, 3, 3),
                pin(2, box2d(0, 0, 1, 3),
                    {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0})},
            true);
        // Header box 2x2 but only three values: cell (5,1) would read past
        // the payload.
        rejects("a payload shorter than its FAB box was accepted",
            {second, pin(3, second, {10.0, 11.0, 12.0})}, true);
        rejects("a block with no payload was accepted", {second, {}}, false);
        // And an inverted FAB header box, which valueOffset refuses.
        rejects("an inverted FAB header box was accepted",
            {second, pin(4, box2d(5, 1, 4, 0), {0.0})}, true);
    }

    std::cout << "block lookup tests passed\n";
    return 0;
}
