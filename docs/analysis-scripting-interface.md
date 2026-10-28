# Analysis scripting interface

Status: design proposal, 2026-09-27. No interface described here is implemented yet.

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

with amrexplorer.open_plotfile("plt0100000") as ds:
    print(ds.fields, ds.time, ds.geometry)
    plan = ds.plan_leaf_chunks(
        fields=["gasDensity", "temperature"],
        region=None,                 # full physical domain
        dtype=None,                  # None: plotfile's stored dtype
        max_bytes=None,              # None: 1 GiB; hard per-item cap
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
widths; and cell volume where that measure is defined. Arrays are read-only by
default. The Python binding must keep their backing storage alive while any
array view exists. Each array is a one-dimensional, equal-length vector of
selected leaf cells in the canonical order defined below. Coverage and region
masks are applied in C++; a covered coarse cell never appears as an ordinary
sample. Algorithms that need neighboring cells use the separate tile API
described under "Tiles with ghost cells"; the default iterator's compact
contract stays unchanged.

Leaf cells have one canonical global order, and both plan items and the cells
within each chunk follow it. Levels come in ascending order, coarsest first.
Within a level, grids are ordered along a Morton (Z-order) curve keyed on each
grid's lower-corner cell index, measured from the level's domain lower corner.
Grids on one level do not overlap, so these keys are distinct. Within a grid,
cells follow AMReX's native FAB memory order (Fortran order, x varying
fastest). Masking removes cells without reordering the ones that remain. Each
chunk is exactly one grid (FAB), so a chunk's arrays hold that grid's selected
leaf cells in this order.

Field arrays have the plotfile's stored dtype unless the caller passes a
`dtype` keyword, in which case the backend converts while decoding. The plan's
memory estimates use the output dtype. Keeping the stored dtype by default means
a float32 plotfile yields float32 arrays, so scripts that need float64
accumulation must request it or pass a dtype to the reduction. Geometry arrays
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
groups them, and a FAB with no selected leaf cells produces no item. The plan is ordered deterministically
and exposes a stable item index for reporting and deterministic reductions.
Items do not depend on iteration order or on mutable cursor state. Reading an
item twice gives the same cells and values while the plotfile is unchanged.

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
Document the actual peak as including the output arrays, decoded source
components, derived-field inputs, and cache residency.

`max_bytes` is the explicit allocation limit. It caps the estimated peak
allocation for reading any single item: its output arrays plus the decoded
source FAB components and derived-field inputs it needs. It defaults to 1 GiB,
far above typical FABs, and the plan exposes the resolved value as
`plan.max_bytes`. Planning raises an error naming the offending grid and its
estimated size if any item would exceed the limit, rather than allocating
without a bound. Callers with very large FABs can raise `max_bytes`
explicitly.

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

`ds.particle_chunks(species, fields=[...], target_bytes=...)` should eventually
stream complete named integer and real attributes, positions, and stable IDs.
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
all cells, covered or not. Extrema are computed from the decoded data with
NaN ignored, matching Kangaroo's `nanmin`/`nanmax`. The per-block extrema in
the MultiFab header (`src/io/plotfile/PlotfileMetadataReader.cpp`) are not
used here: AMReX computes them before converting to the stored precision,
writes them as text at limited precision, and lets NaN propagate. Block order
follows the canonical grid order above, and each block also reports its
native FAB index within its level so output and selection can match tools
that address FABs by plotfile position.

### Surface-intersection geometry (later milestone)

Flux integrals through spheres and z-aligned cylinders weight each leaf cell
by a per-cell surface area rather than a cell-center test. The reference
geometry is Kangaroo's tangent-plane approximation
(`kangaroo/cpp/src/kernels/flux_geometry_support.cpp`), used by
`plotfile_flux_surface.py` and `plotfile_cylindrical_flux_surface.py`, not the
exact curved-surface area:

- **Sphere:** the surface inside a cell is approximated by the plane
  perpendicular to the unit vector from the center to the cell center, at
  distance `radius`; the area is that plane's section of the cell box, and
  the normal is that unit vector.
- **Cylinder walls:** the same construction in the x-y plane, with the cell's
  z extent clipped to `|z| <= half_height`.
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
chunks, and `max_bytes` covers the ghost-filled allocation. Ghost filling
reads neighboring FABs, so its cost and cache behavior must be measured
before parallel execution relies on it.

### Particle deposition (later milestone)

Kangaroo's `plotfile_projection_cic_stellar.py` deposits particle mass onto a
projection grid with cloud-in-cell weights. Particle attribute chunks alone
leave this to Python, which is correct but slow for large particle counts.
`ds.deposit_particles(species, weight, axis, region, resolution,
method="cic", mask=None)` should return a numeric array with documented
kernel width and normalization, using the same pixel conventions as numeric
projections so the two can be compared or summed. `mask` is an optional
boolean array or callable over particle attributes that selects which
particles deposit, so Python-side filters such as Kangaroo's `mass_max` reach
the native deposit.

The boundary policy follows Kangaroo: CIC weight that lands outside the
particle's grid, in a covered cell, or outside the region is dropped, not
clipped or reflected. The result reports the dropped weight alongside the
array. Conservation is therefore stated as: deposited weight plus dropped
weight equals the direct sum over selected particles, and the two totals
agree exactly when every selected particle's CIC cloud lies inside the
region.

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
   requests, missing fields, `max_bytes` rejection, and chunk lifetime. Compare a
   real Quokka plotfile's native values and phase totals with yt using the
   same selection and arithmetic; document any floating-point difference.
2. **Particle attributes and analysis scripts.** Expose complete particle
   attributes. Port one particle histogram and the Kennicutt-Schmidt
   calculation, retaining its Python histogram of cell-center positions
   weighted by leaf-cell mass. These ports use leaf-cell and particle chunks.
3. **Parallel execution.** Benchmark serial reads and cache contention, choose
   a worker ownership model, add bounded scheduling and reductions, then test
   serial/thread/process or MPI parity on multilevel data.
4. **Numeric projections.** Define pixel normalization, boundary overlap, and
   coverage semantics; add leaf-cell projection and coverage results. Port one
   radial surface-density profile. Check conservation of total projected
   quantity against a direct leaf-cell sum with matching region semantics on
   synthetic and representative plotfiles.
5. **Headless plotting and batch ergonomics.** Expose numeric slices and
   image-oriented queries, sequence helpers, cancellation/progress, and examples
   covering the current yt plotting scripts.
6. **Block metadata and statistics.** Expose grid listings with native FAB
   indices and per-block extrema computed from decoded data. Port `plotfile_fab_minmax.py` and check its output against
   Kangaroo on the same plotfile.
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
9. **Particle deposition.** Add CIC deposition on the projection pixel grid.
   Test that deposited plus dropped weight equals the direct sum, that
   interior-only particles conserve exactly, and agreement with a NumPy
   reference deposit, then port `plotfile_projection_cic_stellar.py` with its
   `mass_max` selection.

The public Python examples and tests should say whether they validate raw
source values, analysis totals, visual equivalence, or parallel execution.
Passing a slice or import smoke test does not establish exact leaf-cell
correctness.
