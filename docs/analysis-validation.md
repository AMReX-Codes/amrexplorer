# Validating the analysis interface

What [analysis-scripting-interface.md](analysis-scripting-interface.md)
promises, and the evidence that it holds. Two layers:

- **Tests** (`tests/python`, run by ctest as `python_analysis`;
  `tests/integration/test_leaf_chunks.cpp` without Python) check every contract
  against fixture plotfiles whose values are known in closed form. Each test
  module says whether it validates raw source values, analysis totals, or
  parallel execution.
- **Comparisons** (`tools/analysis_validation/run_comparisons.sh BUILD WORK`)
  run the ported slice and FAB-extrema scripts in `python/examples` next to
  the Kangaroo `scripts/plotfile_*.py` they replace on synthetic plotfiles,
  and compare outputs. `QUOKKA_PLOTFILE=` and `QUOKKA_GALAXY=` add
  comparisons on real plotfiles, including leaf cells against yt.

Passing an import or slice smoke test establishes none of this; the
comparisons below are what does. On 2026-10-28 all 13 comparisons passed.

## Leaf cells against yt on a real Quokka plotfile

`compare_with_yt.py` on a real Quokka plotfile (20 levels, 22 GB), a box of
half width 3e16 cm about the origin: amrexplorer and yt select the same
192,975 leaf cells (by center, covered cells excluded), with identical
`gasDensity` and `temperature` values cell for cell, identical cell volumes,
and identical phase masses. Cell centers differ by at most 8 cm at 1e16 cm
(rounding in each code's center arithmetic).

`check_projection_conservation.py` on the same plotfile, a slab of half width
1e17 cm with bounds off the cell faces: the exact projection's total
(sum of image times pixel area, 86.8 million leaf cells) equals an
independent NumPy sum of value times overlap volume to 1.0e-14.

Against Kangaroo on the same real plotfile: a cell-average slice agrees to
1e-12 (its plane lies in the first finest cell above a level-0 face, where
Kangaroo's plane index is right, see below), and the level-0 per-FAB extrema
of all 24 fields are byte-identical.

Comparing projections on this plotfile found a bug, since fixed: the planner
tested whether a region was empty by counting its index box's cells, and on level 18
of this plotfile a region spanning the domain in z holds exactly 2^64 cells,
which wrapped to zero and dropped the level -- 1.3% of one pixel's column.
`test_region_over_a_huge_domain_is_not_empty` keeps it fixed.

On `quokka/tests/plt0000037` (`QUOKKA_GALAXY`), a galaxy with six levels, MHD
fields and CIC particles, amrexplorer and yt select identical leaf cells with
identical values in a box of half width 3e22 cm.

Comparing particle deposits on that plotfile found a second bug, also fixed:
the deposit's reference total of selected weight was a plain running sum,
which over 212,500 particles drifted by 7.6e-12 from the exactly rounded
total, while the image itself was exact to 7e-16. Both totals are now
compensated sums (`test_totals_are_compensated`).

## Ported scripts

| Port | Against | Agreement |
|---|---|---|
| `plotfile_slice.py` | Kangaroo | rtol 1e-13, planes in the lower half of a level-0 cell (below) |
| `plotfile_fab_minmax.py` | Kangaroo | byte-identical, NaN cells and an all-NaN FAB included |

`plot_frames.py` draws images and has no numeric reference; its smoke test
checks only that it runs.

## Differences, and what causes them

**Kangaroo misreads stored ghost cells.** On a plotfile with `nGrow > 0`,
Kangaroo's FAB reader returns the ghost-cell payload as valid data (the
fixtures' sentinel -1e30 turned up in its integrals). Quokka writes
`nGrow = 0`, so the comparison fixtures do too; amrexplorer's own tests
use stored ghost cells throughout.

**Kangaroo's slice reads the wrong fine plane.** `plane_indices_by_level`
locates the plane on level 0 and scales that index by the refinement ratio,
so on every finer level it reads the lowest fine cell under the coarse cell
containing the plane -- the right cell only when the plane is in the lower
part of the coarse cell. A plane at -4.1e20 cm in the particle fixture, in
level-1 cell 15, came back from Kangaroo with cell 14's values (up to 114%
off). `tests/python/test_slices.py` checks amrexplorer's choice against the
fixture's own values. Kangaroo's default plane, the middle level-0 cell's
center, also lies exactly on a level-1 face, where which side is taken comes
down to rounding.

**Mixed precision confuses yt.** yt reads the float32 level of a plotfile
whose levels store different precisions as NaN; amrexplorer widens such
levels exactly (tested). The yt comparison fixtures store one precision.

## The rest of the build

The headless build's ctest suite (64 tests, the Python suite among them) and
the Qt build's (222 tests) pass. One GUI smoke test,
`qt_remote_sequence_after_fab_smoke_2d`, once timed out in a run started
alongside other heavy builds; it passed alone and in a full rerun of the 222.

## Parallel execution

`tests/python/test_parallel.py` checks that serial, thread (both ownership
models) and process executors give bit-identical ordered reductions on a
three-level plotfile, that cancellation and errors propagate, and that
partitioned ranks combine to the whole.

`benchmark_parallel.py` on the real plotfile (292 items, 91.9 million leaf
cells, two fields, page cache warm, 10-core Apple M-series):

| Executor | Workers | Ownership | Seconds | Speedup |
|---|---|---|---|---|
| serial | 1 | -- | 1.35 | 1.00 |
| threads | 4 | shared | 2.52 | 0.54 |
| threads | 8 | shared | 3.17 | 0.43 |
| threads | 4 | worker | 0.60 | 2.25 |
| threads | 8 | worker | 0.54 | 2.52 |
| processes | 8 | worker | 1.70 | 0.80 |

A shared handle is slower than serial: `PlotfileDataset` serializes block
reads, and a waiting thread polls the lock every 25 ms. Worker-local handles,
the default, scale; processes pay their start-up and per-process metadata
read on a job this short.
