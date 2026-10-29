# Validating the analysis interface

What [analysis-scripting-interface.md](analysis-scripting-interface.md)
promises, and the evidence that it holds. Two layers:

- **Tests** (`tests/python`, run by ctest as `python_analysis`;
  `tests/integration/test_leaf_chunks.cpp` without Python) check every contract
  against fixture plotfiles whose values are known in closed form. Each test
  module says whether it validates raw source values, analysis totals, or
  parallel execution.
- **Comparisons** (`tools/analysis_validation/run_comparisons.sh BUILD WORK`)
  run each ported script in `python/examples` next to the script it replaces --
  the yt scripts in `incite-2026/postprocessing` and Kangaroo's
  `scripts/plotfile_*.py` -- on synthetic plotfiles, and compare outputs.
  `QUOKKA_PLOTFILE=`, `QUOKKA_GALAXY=`, `QUOKKA_DISK=` and `QUOKKA_STARS=`
  add comparisons on real plotfiles.

Passing an import or slice smoke test establishes none of this; the
comparisons below are what does. On 2026-10-28 all 94 comparisons passed.

## Leaf cells against yt on a real Quokka plotfile

`compare_with_yt.py` on `PopIII_runs/plt0012500` (20 levels, 22 GB), a box of
half width 3e16 cm about the origin: amrexplorer and yt select the same
192,975 leaf cells (by center, covered cells excluded), with identical
`gasDensity` and `temperature` values cell for cell, identical cell volumes,
and identical phase masses. Cell centers differ by at most 8 cm at 1e16 cm
(rounding in each code's center arithmetic).

`check_projection_conservation.py` on the same plotfile, a slab of half width
1e17 cm with bounds off the cell faces: the exact projection's total
(sum of image times pixel area, 86.8 million leaf cells) equals an
independent NumPy sum of value times overlap volume to 1.0e-14.

Against Kangaroo on the same real plotfile: the full-domain projection of
`gasDensity` through all 20 levels (256 x 256 pixels) agrees to 1.4e-13; a
cell-average slice agrees to 1e-12 (its plane lies in the first finest cell
above a level-0 face, where Kangaroo's plane index is right, see below); and
the level-0 per-FAB extrema of all 24 fields are byte-identical.

That projection comparison found a bug, since fixed: the planner tested
whether a region was empty by counting its index box's cells, and on level 18
of this plotfile a region spanning the domain in z holds exactly 2^64 cells,
which wrapped to zero and dropped the level -- 1.3% of one pixel's column.
`test_region_over_a_huge_domain_is_not_empty` keeps it fixed.

## Real plotfiles with particles and magnetic fields

Two Quokka test outputs carry what PopIII_runs does not:

- `quokka/tests/plt0000037` (`QUOKKA_GALAXY`): a galaxy, six levels of eight
  grids each, MHD fields, a passive scalar, the stellar birth-mass density,
  and 212,500 CIC particles on all six levels. Against Kangaroo: spherical
  and cylindrical flux surfaces (five cases), all three enclosed-SFR outputs
  (rtol 1e-12), the gas projection (1e-12), the CIC particle mass histogram
  (byte-identical), and the CIC deposit on two axes (1.2e-14 and 2.4e-14 away
  from boundaries; Kangaroo loses 1.2% and 2.1% of the mass, the conservative
  deposit keeps it to 1e-15). Against yt: identical leaf cells and values in
  a box of half width 3e22 cm.
- `quokka/tests/plt0000010` (`QUOKKA_STARS`): 163,961 StochasticStellarPop
  particles. Against Kangaroo: the particle mass histogram (byte-identical)
  and the stellar CIC deposit (8e-15 away from boundaries; Kangaroo loses
  0.4%). Against yt: the Kennicutt-Schmidt CSV at two resolutions (1e-14)
  and the population mass histogram in three modes (byte-identical).

The flux comparisons on real data use `compare_json.py --scale-by-key`: each
value's difference is measured against the largest magnitude of that
quantity in the output. Strictly per value, a handful of entries differ by up
to 4.2e-11 -- positive sign bins 1e-7 the size of the net flux, made of cells
whose radial velocity is nearly zero, and nets of nearly cancelling bins. A
one-ulp shift of the surface's center moves the worst of these bins by up to
9.6e-11 in amrexplorer alone, so the differences are within the bins' own
sensitivity to coordinate rounding (Kangaroo fuses its face arithmetic),
not disagreements about the integral. The CIC comparison likewise allows
1e-15 of the image's maximum per pixel: a particle almost exactly on a cell
center leaves a CIC fraction near 1e-16 that one code rounds to zero.

These comparisons found a second bug, also fixed: the deposit's reference
total of selected weight was a plain running sum, which over 212,500
particles drifted by 7.6e-12 from the exactly rounded total, while the image
itself was exact to 7e-16. Both totals are now compensated sums
(`test_totals_are_compensated`).

- `amrex-visit-plugin/example_data/DiskGalaxy/plt0000020` (`QUOKKA_DISK`):
  a disk galaxy, six levels, with a temperature and a gravitational
  potential. Against yt: identical leaf cells, values and volumes over the
  whole domain (3,158,016 cells), and the density-temperature phase
  histogram in all three weightings. Against Kangaroo: the Toomre-Q moments
  that do not depend on gravity, and spherical and cylindrical fluxes with
  temperature bins. The disk's radial gravity differs from Kangaroo's by up
  to 3.9%, which is Kangaroo's block-face gradient error (above); Quokka's
  stored `circular_velocity` differs from both by up to 16% in the inner
  bins, so it is a different quantity and not an arbiter.

On the disk the phase histogram's weighted bins hold up to ~1e6 cells, and
both scripts sum a bin in histogram2d's plain running sum, in different
orders: against exactly rounded sums amrexplorer is off by at most 1.7e-12
(0.18 n eps) and yt by 3.0e-11 (0.12 n eps). The comparison there allows
2 n eps per bin, n the bin's cell count from the exact count histogram.

## Ported scripts

| Port | Against | Agreement |
|---|---|---|
| `plotfile_enclosed_sfr.py` | Kangaroo | all three JSON outputs, rtol 1e-12 |
| `plotfile_particle_mass_histogram.py` | Kangaroo | byte-identical stdout, CSV, NPZ |
| `plotfile_projection.py` | Kangaroo | rtol 1e-13 (observed 8e-16) |
| `plotfile_slice.py` | Kangaroo | rtol 1e-13, planes in the lower half of a level-0 cell (below) |
| `plotfile_fab_minmax.py` | Kangaroo | byte-identical, NaN cells and an all-NaN FAB included |
| `plotfile_flux_surface.py` | Kangaroo | rtol 1e-12, nine cases (observed 7.4e-14) |
| `plotfile_cylindrical_flux_surface.py` | Kangaroo | rtol 1e-12, twelve cases (h = 2 kpc against Kangaroo one ulp lower, below) |
| `plotfile_projection_cic_stellar.py` | Kangaroo | rtol 1e-12 away from boundaries (below) |
| `plotfile_toomre_q.py` | Kangaroo | every column, rtol 1e-13, on one grid; elsewhere gravity-independent columns (below) |
| `plotfile_toomre_q.py` | exact answer | radial gravity exact (1e-13) on one level; within 5% across levels |
| `compute_kennicutt_schmidt.py` | yt | rtol 1e-14 |
| `stellar_pop_mass_hist.py` | yt | byte-identical, five argument sets |
| `plot_density_temperature_phase.py` | yt | rtol 1e-14, mass, volume and count weights |
| `plot_surface_density_profile.py` | yt | rtol 1e-13 on the full domain (below) |
| `temperature_phase_masses.py` | yt (real plotfile) | identical, see above |

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

**Kangaroo drops the caps of a cylinder whose height lands on a block
face.** At `--height-kpc 2` the height converts to 6.171355162982735e21 cm;
Kangaroo's Python block culling computes the level-2 block's upper z bound
without fusing (…734e21), one ulp below the height, and skips the block, while
its kernel, with fused face arithmetic, would have put the caps there. The
level-1 cells beneath are masked as covered, so the caps over the whole
level-2 footprint vanish (endcap mass flux off by up to 75%). With the
height one ulp lower Kangaroo keeps the block, and the port at 2 kpc agrees
with it to 1e-12 -- a comparison the script runs. The port is not changed to
match the culled result.

**Kangaroo's particle deposit loses mass at boundaries.** By design (see the
interface document), Kangaroo deposits block by block and drops CIC weight
that crosses into a neighboring block or a covered cell. On the particle
fixture it loses 12.8-15.7% of the deposited mass; amrexplorer's deposit
keeps all of it (to 1e-14) and agrees with Kangaroo to 1.1e-14 in every pixel
at least one level-0 cell from any grid face and outside the refined footprint
(`compare_cic.py`).

**Kangaroo's potential gradient is wrong at block faces.** The Toomre-Q
port differentiates the potential by central differences on tiles with one
ghost cell; Kangaroo samples its own AMR patches. The fixture's harmonic
potential has an exact answer (radial gravity Omega^2 R, checked by
`check_toomre_gravity.py`):

- One level, one grid: both are exact, and every column of the two CSVs,
  gravity and the three Q variants included, agrees to 7.9e-15.
- One level, three grids: the port stays exact (to 1e-15); Kangaroo's
  radial gravity is off by up to 381%, in the bins whose cells touch a block
  face -- its stencil does not see the neighboring block.
- Three levels: the port is exact in bins within one level and off by at
  most 3.5% next to coarse-fine boundaries, where linear ghost
  interpolation cannot reproduce a quadratic potential; Kangaroo is off by
  up to 18% in the same bins. Every column not derived from gravity agrees
  to 1e-14.

**yt point-samples projections.** yt's fixed-resolution buffer reads its
projection at pixel centers; `Dataset.project` averages each pixel's column
exactly. They agree wherever a pixel lies within one leaf column -- every
pixel when pixels nest in the finest cells (the full-domain profile case,
3.5e-15). With `--width-kpc 12`, pixel edges are placed in unyt's
kiloparsec (3.085677580962325e21 cm, 1.7e-10 shorter than the fixture's IAU
value), straddle cell faces by about 1e-9 kpc near the image edge, and the two
semantics differ by up to 2e-8.

**Bins on cell centers are ill-conditioned.** At 32 pixels across the
16 kpc Kennicutt-Schmidt map, every coarse cell center sits exactly on a pixel
edge, and ulp differences between codes move cells between pixels. The
comparisons use 24 and 40 pixels, where no center can land on an edge.

**Constants matter.** The yt ports use unyt's constants (its parsec is
3.0856775809623245e18 cm), the Kangaroo ports Kangaroo's (IAU), and the
cylinder port converts kpc as Kangaroo does, `value * 1.0e3 * pc_cm` left to
right: the other association is one ulp different and moved a wall's mass
flux by 3.5% when the height then fell on the other side of a face.

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
