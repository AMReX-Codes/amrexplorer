# Analysis scripting interface

Status: implemented, 2026-10-28 (proposed 2026-09-27). The C++ layer is
`src/analysis` (`include/amrexplorer/analysis`), the binding `python/`, the
user guide [python-analysis.md](python-analysis.md), and the evidence for
each milestone [analysis-validation.md](analysis-validation.md). Decisions
this document left open are recorded under "Implementation notes" at the
end.

## Goal

Let Python scripts use AMReXplorer to analyze AMReX plotfiles, including the
gas-cell, particle, slice, and projection workflows currently written with yt
in `incite-2026/postprocessing/`, and the plotfile analyses currently written
with Kangaroo in `kangaroo/scripts/`. The interface must preserve native field
values and exact AMR coverage. Scripts own their physical-unit conversions,
statistics, plots, and output files. A serial first milestone must leave a
clear path to parallel iteration over leaf chunks.

The compute path already lives below Qt: `PlotfileDataset` reads and caches
blocks, `SliceQuery` and `VolumeQuery` sample them, and `DatasetSession` offers
local and remote visual queries. Analysis adds exact traversal; a screen-sized
slice or uniformly sampled volume cannot stand in for native cell data.

## Boundary and ownership

```text
Python analysis code: NumPy, reductions, physical units, plots, CSV/NPZ
        |
Python extension: lifetime-safe arrays and a small, typed API
        |
Qt-free C++ analysis layer: leaf planning, AMR masks, chunk reads, projection
        |
PlotfileDataset / plotfile readers: metadata, stored/derived FABs, particles
```

The C++ backend exposes plotfile-native numbers only. It does not read a unit
system, attach units to arrays, infer cgs values, or convert fields. Coordinates
and time are in the units represented by the plotfile geometry and header.
For Cartesian plotfiles, `cell_volume` is the product of the cell widths over
the plotfile's dimensions: `dx[0]` in 1-D, `dx[0]*dx[1]` in 2-D, and
`dx[0]*dx[1]*dx[2]` in 3-D, in native coordinate units raised to that power.
Its meaning in lower dimensions (length or area) is the caller's to interpret.
A projected integral has units of the input field
multiplied by native path length; the caller decides its physical meaning.
Geometry and coordinate system remain backend concerns because they determine
which cells exist and how they are located. Unsupported geometry for an
operation must fail explicitly rather than silently use Cartesian formulas.

The Python package may later provide optional, separately configured unit
helpers. They must never change the native dataset or be required to use it.
Derived field expressions can reuse the existing expression engine, but their
values also remain native numbers. Analysis requests fail on missing or skipped
definitions unless the script explicitly opts into a different policy; the
viewer's skip-on-open behavior is inappropriate for an unattended reduction.

## Proposed Python surface

Names and signatures are illustrative; the data contracts below are the
design decisions.

```python
import amrexplorer

with amrexplorer.open_plotfile(
    "plt0100000",
    derived=[],                      # derived-field definitions; none here
    cache_bytes=None,                # None: viewer default (see below)
) as ds:
    print(ds.fields, ds.time, ds.geometry)
    plan = ds.plan_leaf_chunks(
        fields=["gasDensity", "temperature"],
        region=None,                 # full physical domain
        dtype=None,                  # None: widest stored dtype
    )
    cold_mass_native = 0.0
    for item in plan:                 # serial in milestone 1
        chunk = ds.read_leaf_chunk(item)
        rho = chunk["gasDensity"]
        cold = chunk["temperature"] < 200
        cold_mass_native += (rho[cold] * chunk.cell_volume[cold]).sum()
```

`ds.leaf_cells(...)` is a convenience iterator equivalent to planning and
reading serially. A chunk exposes arrays of equal logical shape for its
requested fields; integer cell indices; cell-center coordinates; level; cell
widths; and cell volume, which is always present for supported geometries.

Chunks are copies. Reading an item decodes what it needs through the block
cache, copies the selected leaf cells into arrays the chunk owns, and releases
every cache pin before returning. A chunk therefore holds no cache blocks,
stays valid after the dataset is closed, and any number of chunks can be kept
without affecting the cache budget. Arrays are read-only by default.

Field arrays are filled when the item is read. Geometry arrays (indices,
coordinates, widths, and `cell_volume`) are computed lazily on first access
from the chunk's level, box, and leaf selection, then cached on the chunk, so
a script that never touches them never pays for them. Each array is a
one-dimensional, equal-length vector of
selected leaf cells in the canonical order defined below. Per-axis quantities
are separate 1-D arrays, one per axis the plotfile has, never an `(n, 3)`
array: indices `chunk.i`, `chunk.j`, `chunk.k`; cell-center coordinates
`chunk.x`, `chunk.y`, `chunk.z`; and cell widths `chunk.dx`, `chunk.dy`,
`chunk.dz`. A 2-D plotfile has no `k`, `z`, or `dz`. A boolean mask over the
chunk therefore indexes every array the same way. Coverage and region
masks are applied in C++; a covered coarse cell never appears as an ordinary
sample. Algorithms that need neighboring cells use the separate tile API
described under "Tiles with ghost cells"; the default iterator's compact
contract stays unchanged.

Leaf cells have one canonical global order, and both plan items and the cells
within each chunk follow it. Levels come in ascending order, coarsest first.
Within a level, grids follow their native FAB index, the order in which the
plotfile's level header lists them. Within a grid,
cells follow AMReX's native FAB memory order (Fortran order, x varying
fastest). Masking removes cells without reordering the ones that remain. Each
chunk is exactly one grid (FAB), so a chunk's arrays hold that grid's selected
leaf cells in this order.

Precision is stored per level (each level's `realDescriptor`), so a plotfile
can mix float32 and float64 levels. With `dtype=None`, field arrays use the
widest stored dtype across all levels, so one plan yields one dtype on every
level; narrower levels are widened exactly. A `dtype` keyword overrides this,
and the backend converts while decoding. `dtype` affects only the output
arrays, not cache use. An all-float32 plotfile yields float32 arrays, so
scripts that need float64 accumulation must request it or pass a dtype to the
reduction. Derived fields are evaluated in float64 and, with `dtype=None`,
narrowed to that widest stored dtype, so every field array in a chunk shares
one dtype. Geometry arrays
(cell-center coordinates, cell widths, and `cell_volume`) are always float64,
regardless of the plotfile's stored dtype or the `dtype` keyword.

Field selection uses the plotfile's exact field names. Each plotfile variable
is its own single-component field, so there are no component indices; the API
must expose metadata so a script can resolve names before reading.
Unknown fields, unavailable derived fields, unsupported centering, and invalid
regions are errors. The first milestone supports cell-centered Cartesian
plotfile fields. Nodal, face-centered, mapped, and spherical geometry require
separate semantics and can be added explicitly later.

## Leaf-chunk plan and parallel readiness

Planning reads metadata, not field payloads. Each work item describes a
dataset-session-local, immutable unit of work: level, grid (FAB), requested
fields, region selection, and the refinement coverage needed to identify leaf
cells. Each item is exactly one FAB: the planner neither splits FABs nor
groups them, and a FAB with no selected leaf cells produces no item. The plan
is ordered deterministically and exposes a stable item index for reporting
and deterministic reductions. Items do not depend on iteration order or on
mutable cursor state. Reading an item twice gives the same cells and values
while the plotfile is unchanged.

Derived-field definitions are fixed when the `PlotfileDataset` is
constructed (`include/amrexplorer/io/PlotfileDataset.hpp`), so a plan's
derived fields cannot change for the dataset's lifetime; using different
definitions means opening a new dataset and planning again. Scripts pass
definitions explicitly as `open_plotfile(path, derived=[...])`, each a name and
an expression in the viewer's expression syntax. The binding never loads the
viewer's saved definitions. `PlotfileDataset` currently hardcodes
`DerivedFieldPolicy::Skip`, which drops definitions that do not resolve and
reports them through `skippedDerivedFields`. The constructor gains a policy
parameter, and the binding passes `DerivedFieldPolicy::Strict`, so the
existing strict path in `installDerivedFields` raises at open with its own
messages, per the failure rule above. The GUI keeps passing `Skip`.

Each dataset the binding opens gets a unique `DatasetId`, and every plan item
records it. Reading an item through a different dataset raises an error, even
for the same plotfile path, so an item can never return values computed from
another dataset's definitions.

For a full-domain plan, every leaf cell appears in exactly one item and no
covered coarse cell appears. The backend assumes the plotfile is properly
nested, as AMReX guarantees, and does not validate it: every level-(l+1) grid
lies within the level-l grids. Under that assumption, a level-l cell is a
leaf exactly when no level-(l+1) grid covers it, so checking coverage against
the next finer level alone is sufficient. A plotfile that violates proper
nesting gives undefined leaf partitions. Coverage must still account for
partial overlap with a coarse block or requested region. Region selection is
defined by cell centers unless an operation explicitly requests fractional
boundary-cell overlap. Do not infer leaf status
from a display raster's `sourceLevel`: that reports sampled pixels, not a
partition of native cells.

Because items are whole FABs, per-item memory is set by the plotfile's grid
sizes (AMReX's `max_grid_size`), not by the planner; this matches
`PlotfileDataset::requestBlock`, which decodes complete FAB components.
The decoded box is the on-disk FAB, which includes the plotfile's stored
ghost cells (`nGrow`), so estimates use that grown box, not the valid box;
with `nGrow=2` on 32^3 grids the difference is about 42%. The documented
peak per read is the cache budget plus the chunk's own arrays and evaluation
scratch.

The dataset's block-cache budget is the allocation limit; there is no
separate per-plan cap. While an item is being read, its decoded blocks are
pinned in the block cache, and `ByteLruCache::insertAndPin` refuses a pin
that would exceed the budget. The planner rejects, with an error naming the
grid and its estimate, any item whose conservative estimate exceeds the
budget. The estimate is a deliberate upper bound, not an exact model of
pinning order:

- every block the item could touch, meaning each distinct stored source
  component and each derived field, including derived fields reached only as
  inputs to other derived fields, is counted as if pinned at once;
- each block is sized at 8 bytes per cell over the grown on-disk box,
  whatever its stored dtype or actual box;
- a fixed per-block allowance covers bookkeeping such as `FabBlock`
  overhead.

An item that passes this check cannot exceed the budget through its own pins.
Reads can still raise `CacheBudgetExceeded` if the budget is lowered after
planning or if concurrent reads share the cache; that error names the item.
The check may reject an item that would in fact have fit; raising
`cache_bytes` resolves it. Chunk arrays and evaluation scratch are allocated
outside the cache, are not covered by the budget, and are bounded by one
FAB's cells per field.

`open_plotfile(path, cache_bytes=None)` sets that budget the same way the C++
code does today: the value is passed as `cacheBudgetBytes` when the
`PlotfileDataset` is constructed, and `ds.set_cache_bytes(n)` maps to
`PlotfileDataset::setCacheBudget`. With `cache_bytes=None` the binding uses
the viewer's default, `AMREXPLORER_CACHE_SIZE_MB` if set and otherwise 1 GiB.
That default currently lives in `src/qt/CacheConfig.hpp`; it moves to a
Qt-free header so the GUI and the binding share one definition. The viewer
silently falls back to 1 GiB when the variable is set but unparseable, zero,
or too large; the binding raises instead, naming the variable and its value,
so a batch job never runs with a budget it did not ask for. Callers with very
large FABs raise `cache_bytes`.

`setCacheBudget` returns `false` when pinned blocks already exceed the new
budget, but `ByteLruCache::setBudget` has stored the new budget by then, and
the mapped-grid pool has been resized too. `ds.set_cache_bytes(n)` therefore
restores the previous budget on both pools before raising, and says how many
bytes are pinned. A failed call leaves the dataset exactly as it was, so a
later `CacheBudgetExceeded` is never a silent consequence of it.

A plotfile with a mapped grid gives that grid its own block pool of the full
budget (`PlotfileDataset::mappedGridCacheBudget`), so resident memory can
reach twice `cache_bytes`. That pool fills only while the mapped grid is
displayed, and analysis rejects mapped geometry, so analysis reads never use
it; a future mapped-geometry milestone must account for it.

Future executors can distribute items across threads, processes, or MPI ranks
and combine partial results. They should use bounded in-flight reads and
worker-local partial accumulators. The Python API can offer an executor or
`partition(rank, size)` later without changing the chunk contract. Dynamic
scheduling may be needed when grid sizes or derived-field costs differ. A
fixed item order makes deterministic serial results possible, but parallel
floating-point sums need an explicit ordered or reproducible reduction mode;
identical last bits should not be promised by default.

Current `PlotfileDataset` serializes block reads with a per-dataset mutex.
Parallel execution therefore needs a measured choice between worker-local
dataset handles and narrower cache/read locking. The initial API must not
promise concurrent I/O merely because items are independent. Python compute
parallelism also depends on NumPy operations and binding/GIL behavior; native
reads should release the GIL when they are made concurrent.

Work-item IDs are valid only for the open dataset and plan in milestone 1.
Cross-process serialization, file-change detection, and resumable job manifests
can follow when an MPI or distributed executor is implemented.

## Additional primitives

### Particle attributes

`ds.particle_chunks(species, fields=[...])` should eventually stream complete
named integer and real attributes, positions, and stable IDs. As with leaf
chunks, each particle chunk is exactly one grid's particles as the plotfile
stores them, never split or grouped, in level and then native grid order, and
chunks are copies.
The existing reader discovers species and samples positions for display; it
currently discards component names while parsing the particle header and does
not expose the full attributes. Extend its bounded parsing and chunked I/O
rather than build a second, less-validated parser. Particle filters and
histograms stay in Python. A display sample is never a default analysis input.

### Numeric slices

Expose numeric `slice(...)` results through the current query layer, including
coordinates, validity, source level, and sampling policy.

### Numeric projections (later milestone)

Native projections are deferred until after parallel execution. For analysis,
`project(field, axis, region, resolution, method="integrate")` needs a new
native reduction over leaf cells, with a documented pixel-boundary and
partial-cell policy. It returns a numeric array and coverage, not a colored
image. Its result is distinct from a face-on view made by summing sampled
pixels. Exact integral and approximate raster sampling must have different
names and documented semantics. Matplotlib can render either result in Python.

### Block metadata and statistics (later milestone)

Some analyses need the plotfile's own grid structure rather than a partition
of leaf cells; Kangaroo's `plotfile_fab_minmax.py`, for example, reports
per-FAB extrema including covered cells. `ds.blocks(level=None)` should list
each grid's level, index box, physical bounds, and covered fraction, and
`ds.block_statistics(fields)` should return per-block minima and maxima over
all cells of the on-disk FAB, covered or not and including stored ghost
cells, as Kangaroo's `plotfile_fab_minmax.py` does. Extrema are computed from
the decoded data with NaN ignored, matching Kangaroo's `nanmin`/`nanmax`; a
block whose values are all NaN reports NaN for both, without raising.
Derived fields have no stored ghost cells (`readDerivedBlock` builds them over
the valid box), so their extrema cover the valid box only; each result
reports which box it covers. The per-block extrema in
the MultiFab header (`src/io/plotfile/PlotfileMetadataReader.cpp`) are not
used here: AMReX computes them before converting to the stored precision,
writes them as text at limited precision, and lets NaN propagate. Blocks are
listed in the canonical grid order above, by level and then native FAB index,
and each block reports its FAB index so output and selection match tools that
address FABs by plotfile position.

### Surface-intersection geometry (later milestone)

Flux integrals through spheres and z-aligned cylinders weight each leaf cell
by a per-cell surface area rather than a cell-center test. The reference
geometry is Kangaroo's tangent-plane approximation
(`kangaroo/cpp/src/kernels/flux_geometry_support.cpp`), used by
`plotfile_flux_surface.py` and `plotfile_cylindrical_flux_surface.py`, not the
exact curved-surface area:

- **Intersection test:** a cell gets nonzero area only if the true sphere or
  finite cylinder meets its box (Kangaroo's `sphere_may_intersect_cell` and
  `cylinder_may_intersect_cell`). This test runs first; the tangent-plane
  area below is computed only for cells that pass it, so a plane that clips a
  cell corner the real surface misses contributes nothing.
- **Sphere:** the surface inside a cell is approximated by the plane
  perpendicular to the unit vector from the center to the cell center, at
  distance `radius`; the area is that plane's section of the cell box, and
  the normal is that unit vector.
- **Cylinder walls:** the same construction in the x-y plane, with the cell's
  z extent clipped to `|z - center[2]| <= half_height`. All cylinder
  coordinates, including the end caps and their moments, are measured
  relative to `center`.
- **Cylinder end caps:** these are planar, so the clipped disk-rectangle
  footprint is computed analytically, together with its first moments
  (integrals of x dA and y dA) for lever-arm-weighted fluxes such as
  vertical angular momentum.
- **Faces:** when a cap or wall lies exactly on a cell face, the cell on the
  interior side supplies the state and the face is counted once, as in
  Kangaroo.

Provide vectorized C++ helpers that take a chunk's cell centers and widths and
return these per-cell areas, normals, and cap moments:
`sphere_area_in_cells(chunk, center, radius)` and
`cylinder_area_in_cells(chunk, center, radius, half_height)`, the latter
reporting walls and end caps separately. Field arithmetic and binning stay in
Python. Exact curved-surface areas could be added later under a different
name, not as a silent change to these helpers.

Because region selection is by cell center, a cell can cross the surface
while its center lies outside a tight region. A caller must pad the region
around the surface by half the cell diagonal of the coarsest leaf cell that
can meet the surface; padding by the coarsest level's half-diagonal is always
safe. The helpers document this, and a convenience planner should compute the
padded region.

### Tiles with ghost cells (later milestone)

Stencil operations, such as the potential gradient in Kangaroo's
`plotfile_toomre_q.py`, need neighbors that a compact leaf-cell vector does
not carry. `ds.plan_tiles(fields, ghost=n, ...)` should yield one tile per
FAB, matching leaf chunks, grown by `n` ghost cells per side. Ghosts are
filled from same-level neighbors where they exist. Elsewhere they are
interpolated, by a documented scheme, from the finest coarser level that
covers them, recursing past level l-1 when `n` exceeds the nesting buffer, as
AMReX's `FillPatch` does. Physical-domain ghosts follow a stated boundary
policy or are marked invalid. Each tile carries a leaf mask for its valid
region so a reduction counts each leaf cell once and agrees with the
leaf-chunk iterator. Tiles use Fortran order and the same dtype rules as leaf
chunks, and the planning-time budget check covers every FAB a tile's ghost
fill pins. Ghost filling
reads neighboring FABs, so its cost and cache behavior must be measured
before parallel execution relies on it.

### Particle deposition (later milestone)

Kangaroo's `plotfile_projection_cic_stellar.py` deposits particle mass with
cloud-in-cell weights and then projects it. Particle attribute chunks alone
leave this to Python, which is correct but slow for large particle counts.
`ds.deposit_particles(species, weight, axis, region, resolution,
method="cic", mask=None)` provides it natively. The deposit must be
conservative: every selected particle's full weight reaches the result or is
reported as outside the image.

Kangaroo is not the reference here. Its `particle_cic_projection_accumulate`
(`kangaroo/cpp/src/kernels/particle_kernels.cpp`) deposits block by block and
drops CIC weight that lands in a neighboring block or in a cell covered by a
finer level, so it loses mass near block and coarse-fine boundaries. This
deposit instead works on the AMR hierarchy as a whole:

- **Kernel.** Each selected particle spreads its weight with 2-D CIC weights
  over the four nearest cell centers, in the two image axes, at the cell size
  of the level it is stored on, within the cell layer along `axis` that
  contains it. Region selection along `axis` uses the particle position.
- **Block boundaries.** Weight that crosses into a neighboring grid on the
  same level is deposited there. Blocks are a storage detail and never cause
  loss.
- **Coarse-fine boundaries.** Weight that lands where the particle's level
  has no grid goes to the cell of the next coarser level that contains that
  point, recursing as needed. Weight that lands in a cell covered by a finer
  level is distributed to the covering leaf cells in proportion to their area
  in the image plane. The deposited field therefore lives on leaf cells only.
- **Domain boundaries.** Periodic directions wrap. In non-periodic directions,
  weight that would leave the domain is folded back into the boundary cell,
  so the domain keeps all of it.
- **Projection.** Each leaf cell's deposited weight is distributed to the
  output pixels by the fraction of the cell's area in the image plane that
  overlaps each pixel. The result is deposited weight per pixel, using the
  same pixel conventions as numeric projections so the two can be compared
  or summed.

`mask` is an optional callable that takes a particle chunk's attributes and
returns a boolean array for that chunk, selecting which particles deposit, so
Python-side filters such as Kangaroo's `mass_max` reach the native deposit.
The deposit calls it on each chunk it reads, so the mask always lines up with
the particles it describes. A precomputed boolean array over all particles is
not accepted, because it would depend on a global particle order the API does
not define. Particles with a non-finite position or
weight raise an error rather than being skipped silently.

The result reports the weight that falls outside the image rectangle. Pixel
weight plus outside weight equals the direct sum over selected particles, to
floating-point tolerance. Results agree with Kangaroo away from block,
coarse-fine, and domain boundaries, and differ near them by the mass Kangaroo
drops.

### Headless rendering

Expose the existing slice and volume pipelines as optional Python operations
for publication images and frame sequences. Rendering is downstream of numeric
analysis and should not define the analysis data model.

## Local and remote execution

The first milestone is local, in-process Python. For a remote plotfile, run the
same Python script on the machine holding the data and transfer only results
such as CSV, NPZ, and PNG. The existing remote protocol is designed for bounded
viewport and page responses. It should not acquire an unbounded raw-cell RPC.
A future remote analysis service would need explicit operation, resource, and
result-size limits and a separate security review.

## Milestones and validation

1. **Serial leaf cells.** Add the Qt-free planner and reader, Python binding,
   metadata/field discovery, serial convenience iterator, and one example
   computing temperature-phase masses. Keep native values throughout. Test
   multilevel full and partial refinement, region boundaries, multi-field
   requests, missing fields, planning-time rejection of items over the cache
   budget, and chunk lifetime. Compare a real Quokka plotfile's native values
   and phase totals with yt using the same selection and arithmetic; document
   any floating-point difference. Port Kangaroo's `plotfile_enclosed_sfr.py`
   as a weighted 2-D histogram over leaf cells, keeping its JSON output, and
   compare with Kangaroo.
2. **Particle attributes and analysis scripts.** Expose complete particle
   attributes. Port one particle histogram and the Kennicutt-Schmidt
   calculation, retaining its Python histogram of cell-center positions
   weighted by leaf-cell mass. Port Kangaroo's
   `plotfile_particle_mass_histogram.py` and compare with Kangaroo. These
   ports use leaf-cell and particle chunks.
3. **Parallel execution.** Benchmark serial reads and cache contention, choose
   a worker ownership model, add bounded scheduling and reductions, then test
   serial/thread/process or MPI parity on multilevel data.
4. **Numeric projections.** Define pixel normalization, boundary overlap, and
   coverage semantics; add leaf-cell projection and coverage results. Port one
   radial surface-density profile and Kangaroo's `plotfile_projection.py`,
   comparing the latter with Kangaroo. Check conservation of total projected
   quantity against a direct leaf-cell sum with matching region semantics on
   synthetic and representative plotfiles.
5. **Headless plotting and batch ergonomics.** Expose numeric slices and
   image-oriented queries, sequence helpers, cancellation/progress, and examples
   covering the current yt plotting scripts. Port Kangaroo's
   `plotfile_slice.py` and compare with Kangaroo.
6. **Block metadata and statistics.** Expose grid listings with native FAB
   indices and per-block extrema computed from decoded data. Port
   `plotfile_fab_minmax.py` and check its output against Kangaroo on the same
   plotfile.
7. **Surface-intersection helpers.** Add sphere and cylinder area helpers.
   Port both Kangaroo flux-surface scripts, keeping their JSON output so the
   existing `plot_*` scripts run unchanged. Test that per-cell areas, normals,
   cap moments, and fluxes match Kangaroo to rounding on the same plotfile,
   and that summed areas converge to the analytic surface area as resolution
   increases on uniform and multilevel grids.
8. **Tiles with ghost cells.** Add the tile planner and ghost filling. Test
   that masked tile reductions match leaf-chunk totals, and that gradients of
   linear fields are exact across coarse-fine boundaries. Port
   `plotfile_toomre_q.py` and compare profiles with Kangaroo.
9. **Particle deposition.** Add conservative AMR-aware CIC deposition and
   projection to pixels. Test, to floating-point tolerance, that pixel plus
   outside weight equals the direct sum on single-level, multi-block, and
   multilevel plotfiles, including particles next to block, coarse-fine, and
   domain boundaries; and agreement with a NumPy reference deposit on a
   uniform grid. Then port `plotfile_projection_cic_stellar.py` with its
   `mass_max` selection, and check that it matches Kangaroo away from
   boundaries.

The public Python examples and tests should say whether they validate raw
source values, analysis totals, visual equivalence, or parallel execution.
Passing a slice or import smoke test does not establish exact leaf-cell
correctness.

## Implementation notes

Choices made while implementing, where the text above left room:

- **Region selection** is half-open by cell center, `lower <= center <
  upper` on each axis, decided with the same center arithmetic the chunk's
  coordinate arrays report, so adjacent regions partition the leaf cells. A
  region outside the domain gives an empty plan rather than an error; one
  with non-finite or unordered bounds is an error.
- **Precision per level** comes from the level's VisMF `RealDescriptor`, or,
  for a version-1 header that has none, from the first grid's FAB header
  (one short read per level, no payload).
- **The planning estimate** counts a fixed 4 KiB per block for bookkeeping.
- **Worker ownership** (milestone 3): worker-local dataset handles, measured
  against a shared handle in analysis-validation.md. `amrexplorer.parallel`
  re-plans the request on each worker and checks the plan matches, bounds the
  items in flight, and combines partials in item order by default, which
  reproduces the serial result bit for bit.
- **Projection pixels** (milestone 4): `image[j, i]`, shape (height, width),
  `j = 0` at the lower image bound. Without a weight a pixel holds the
  integral over its column divided by its area; with one, the weighted
  average. Boundary cells contribute the part of them inside the slab
  (fractional overlap). `coverage` is the filled fraction of each column.
  Sampled and exact operations have separate names: `slice` samples through
  the query layer, `slice_cell_average` and `project` are exact.
- **Tiles** (milestone 8): ghost cells outside a non-periodic domain are
  invalid (NaN), since a plotfile records no boundary condition;
  periodicity is the caller's to state. Coarse-to-fine interpolation is
  linear with central slopes by default (exact for linear fields), with the
  MC-limited and piecewise-constant schemes as options. The planning check
  bounds the largest single FAB a ghost fill reads, since the fill reads
  them one at a time; each item lists every FAB it touches.
- **Deposition** (milestone 9): a cell covered by a finer level shares its
  weight equally among the covering image-plane columns, in the fine layer
  along the line of sight that holds the particle. Weight that reaches a
  level with no grid goes to the coarser parent without refining again, so
  grids not aligned to the refinement ratio cannot cycle. The `mask`
  callable receives each `ParticleChunk`.
- **Headless rendering** is `amrexplorer.render`: the viewer's
  `ScalarRenderer` and palettes over a sampled slice, and `VolumeQuery` plus
  `VolumeRaycaster` for volumes, returning RGBA arrays. It calls the
  rendering layers directly rather than `SlicePipeline`, whose request and
  result types carry the GUI's view state.
- **Particle identities** are AMReX's `id` and `cpu`, decoded from either
  identity layout; particles marked invalid are left out, as the display
  sample leaves them out.
- **Kangaroo comparisons** found several behaviors of Kangaroo's own that
  the ports do not reproduce (ghost cells, slice planes, cylinder caps on
  block faces); they are listed in analysis-validation.md.

