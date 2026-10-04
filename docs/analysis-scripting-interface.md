# Analysis scripting interface

Status: design proposal, 2026-09-27. No interface described here is implemented yet.

## Goal

Let Python scripts use AMReXplorer to analyze AMReX plotfiles, including the
gas-cell, particle, slice, and projection workflows currently written with yt
in `incite-2026/postprocessing/`. The interface must preserve native field
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
`cell_volume` is a geometric measure in native coordinate units cubed for a
Cartesian 3-D plotfile. A projected integral has units of the input field
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
        target_bytes=256 << 20,      # target output size, not a hard I/O cap
    )
    cold_mass_native = 0.0
    for item in plan:                 # serial in milestone 1
        chunk = ds.read_leaf_chunk(item)
        rho = chunk["gasDensity"]
        cold_mass_native += (
            rho[chunk["temperature"] < 200]
            * chunk.cell_volume[chunk["temperature"] < 200]
        ).sum()
```

`ds.leaf_cells(...)` is a convenience iterator equivalent to planning and
reading serially. A chunk exposes arrays of equal logical shape for its
requested fields; integer cell indices; cell-center coordinates; level; cell
widths; and cell volume where that measure is defined. Arrays are read-only by
default. The Python binding must keep their backing storage alive while any
array view exists. Each array is a one-dimensional, equal-length vector of
selected leaf cells in index order. Coverage and region masks are applied in
C++; a covered coarse cell never appears as an ordinary sample. A later API
may offer rectangular tiles with explicit masks for algorithms that need them,
but the default iterator's compact contract stays unchanged.

Field selection uses the plotfile's exact field names and component indices;
the API must expose metadata so a script can resolve names before reading.
Unknown fields, unavailable derived fields, unsupported centering, and invalid
regions are errors. The first milestone supports cell-centered Cartesian
plotfile fields. Nodal, face-centered, mapped, and spherical geometry require
separate semantics and can be added explicitly later.

## Leaf-chunk plan and parallel readiness

Planning reads metadata, not field payloads. Each work item describes a
dataset-session-local, immutable unit of work: level, grid/block, an index-space
tile within that block, requested fields, region selection, and the refinement
coverage needed to identify leaf cells. The plan is ordered deterministically
and exposes a stable item index for reporting and deterministic reductions.
Items do not depend on iteration order or on mutable cursor state. Reading an
item twice gives the same cells and values while the plotfile is unchanged.

For a full-domain plan, every leaf cell appears in exactly one item and no
covered coarse cell appears. Coverage must account for *all* finer levels, not
just the next one, and for partial overlap with a coarse block or requested
region. Region selection is defined by cell centers unless an operation
explicitly requests fractional boundary-cell overlap. Do not infer leaf status
from a display raster's `sourceLevel`: that reports sampled pixels, not a
partition of native cells.

`target_bytes` guides tile size using the selected fields and geometry arrays.
It is not yet a hard peak-memory bound: `PlotfileDataset::requestBlock` currently
decodes a complete FAB component before a tile can be extracted. Document the
actual peak as including decoded source blocks, derived-field inputs, and cache
residency. A later windowed FAB reader is required before promising a hard
per-worker memory ceiling for very large FABs. A plan must reject a request
whose output would exceed its explicit allocation limits rather than allocate
without a bound.

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
   multilevel full and partial refinement, region boundaries, two-component
   requests, missing fields, cancellation, and chunk lifetime. Compare a
   real Quokka plotfile's native values and phase totals with yt using the
   same selection and arithmetic; document any floating-point difference.
2. **Particle attributes and analysis scripts.** Expose complete particle
   attributes. Port one particle histogram and the Kennicutt-Schmidt
   calculation, retaining its Python histogram of cell-center positions
   weighted by leaf-cell mass. These ports use leaf-cell and particle chunks.
3. **Parallel execution.** Benchmark serial reads and cache contention, choose
   a worker ownership model, add bounded scheduling and reductions, then test
   serial/thread/process or MPI parity on multilevel data. Add windowed FAB
   reads if measured peak memory requires a hard per-worker limit.
4. **Numeric projections.** Define pixel normalization, boundary overlap, and
   coverage semantics; add leaf-cell projection and coverage results. Port one
   radial surface-density profile. Check conservation of total projected
   quantity against a direct leaf-cell sum with matching region semantics on
   synthetic and representative plotfiles.
5. **Headless plotting and batch ergonomics.** Expose numeric slices and
   image-oriented queries, sequence helpers, cancellation/progress, and examples
   covering the current yt plotting scripts.

The public Python examples and tests should say whether they validate raw
source values, analysis totals, visual equivalence, or parallel execution.
Passing a slice or import smoke test does not establish exact leaf-cell
correctness.
