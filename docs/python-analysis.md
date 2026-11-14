# Analyzing plotfiles from Python

The `amrexplorer` Python package is a data-access library for scripted
reductions over AMReX plotfiles: integrals, histograms, and time series. It
returns leaf-cell values as NumPy arrays, without resampling. It supports
Cartesian plotfiles without a mapped grid and reads cell-centered fields.
It provides no plotting, slices, line-outs, particle access, or connection
to the GUI. Scripts open files independently of the viewer.

A **leaf cell** is a cell that no finer AMR level covers. Traversing leaf
cells visits each selected cell exactly once, excluding covered coarse
cells and stored ghost cells. A **chunk** contains one grid's selected
leaf cells, with all its arrays aligned to those same cells.

Values and coordinates use the plotfile's native units. The package does
not read, attach, or convert a unit system; scripts must supply any unit
conversions themselves.

## Install

From the repository checkout, use Python 3.9 or newer and a C++20 compiler.
The build needs CMake 3.25 or newer; pip installs the Python build
requirements automatically. Install inside a virtual environment so this
also works with externally managed system Python installations:

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install .
```

On Windows, activate with `.venv\Scripts\Activate.ps1` in PowerShell.
The package builds without Qt or the remote protocol. NumPy is installed
as a runtime dependency.

## First integral

This example integrates density over the whole domain. Replace `plt00100`
with your plotfile directory and `density` with one of its field names.
Print `ds.fields` to discover the exact names available in your file.

```python
import numpy as np
import amrexplorer

mass = 0.0
with amrexplorer.open_plotfile("plt00100") as ds:
    print(ds.fields)
    for chunk in ds.leaf_cells(["density"]):
        mass += np.sum(chunk["density"] * chunk.cell_volume)
print(mass)
```

Omitting `region` selects the entire domain. `cell_volume` is the product
of the cell widths over the file's dimensions: volume in 3-D, area in
2-D, and length in 1-D. Thus a 2-D density integral is not automatically
a 3-D mass. The result above is in density units times coordinate units
raised to the number of dimensions.

The `with` block closes the dataset and releases its cache. Chunks and
arrays already read are independent, read-only NumPy copies that you can
keep using after the dataset closes. Process one chunk at a time to avoid
retaining all the selected data in memory.

## Select cells and fields

Request multiple fields to apply one mask to both the values and geometry:

```python
with amrexplorer.open_plotfile("plt00100") as ds:
    cold_mass = 0.0
    for chunk in ds.leaf_cells(["density", "temperature"]):
        cold = chunk["temperature"] < 200
        cold_mass += np.sum(chunk["density"][cold] * chunk.cell_volume[cold])
print(cold_mass)
```

Here `200` is a threshold in the temperature field's native units.

To restrict the integral to a box, pass `region=(lower, upper)` to
`leaf_cells()`. Both bounds have one coordinate per dimension. For example,
`region=([0.0, 0.0], [1.0, 1.0])` selects a box in a 2-D file's native
coordinates. Selection uses cell centers with `lower <= center < upper`
on each axis. It includes whole cells, without clipping their volumes at
the boundary; adjacent boxes partition the cells without duplication.

Field arrays default to the widest floating-point precision stored on any
level. Use `dtype=np.float32` or `dtype=np.float64` to choose explicitly;
requesting float32 may lose precision. Arrays within a chunk are one-dimensional,
ordered with x varying fastest, then y, then z, skipping cells not selected.
Physical geometry arrays are float64; cell-index arrays are int32.
Geometry is computed on first access.

## Plans and explicit reads

A **plan** is an ordered sequence of work descriptions for the selected
fields, region, and dtype. Each **item** describes one grid's selected leaf
cells; a grid is an AMReX rectangular data block, also called a FAB. An item
becomes a chunk when you read it. Planning does not load field payloads into
chunks, though determining stored precision can read FAB headers.

Use the explicit interface when you need to inspect work sizes or choose
which items to read:

```python
with amrexplorer.open_plotfile("plt00100") as ds:
    plan = ds.plan_leaf_chunks(["density", "temperature"])
    print(len(plan), plan.total_cells)
    for item in plan:
        print(item.level, item.grid, item.num_cells)
        chunk = ds.read_leaf_chunk(item)
        # Analyze chunk here.
```

Items are ordered from coarsest to finest level, then by the grid order in
the level header. Each selected leaf cell belongs to exactly one item.
Items must be read using the dataset that created them, while it is open.
`ds.leaf_cells(...)` combines planning and serial reading in this order.

A plan exposes `fields`, `dtype`, `region`, `total_cells`, and `dataset_id`;
it supports indexing, iteration, and `len(plan)`. Items expose `index`,
`level`, `grid`, `num_cells`, `estimated_bytes`, `dataset_id`, `box`, and
`selected_box`. Index boxes use inclusive lower and upper bounds;
`selected_box` can still contain covered coarse cells that the read excludes.
`plan.partition(rank, size)` selects items in round-robin order for a static
work split; it does not launch workers.

## Derived fields

Define expressions when opening the dataset:

```python
with amrexplorer.open_plotfile(
    "plt00100", derived=[("density_temperature", "density*temperature")]
) as ds:
    for chunk in ds.leaf_cells(["density_temperature"]):
        values = chunk["density_temperature"]
        # Analyze values here.
```

Expressions can use stored fields, earlier derived fields, and coordinates
`x`, `y`, and `z` where available. A name containing punctuation outside
letters, digits, `_`, and `.` needs `${...}` quoting: for example,
`${x-momentum}/density`. See the [expression reference](user-guide.md#derived-fields)
for syntax. `derived=` also accepts a `{name: expression}` dictionary, or a
list of `amrexplorer.DerivedField(name, expression)` entries; later
definitions can use earlier ones. Invalid definitions raise
`DerivedFieldError` when the dataset opens. Definitions saved in the viewer are not loaded by Python.

## Dataset reference

`open_plotfile(path, derived=(), cache_bytes=None)` accepts a string or
path-like object and returns a dataset. Use it as a context manager or call
`ds.close()` explicitly.

| Member | Meaning |
| --- | --- |
| `path`, `id`, `closed` | Path string, process-unique dataset identifier, and closed state. |
| `fields`, `stored_fields`, `derived_fields` | Exact field names, with stored fields before derived fields in `fields`. |
| `field_centering(name)` | The named field's centering; leaf-cell reads require cell-centered fields. |
| `time`, `dimension`, `finest_level` | Native simulation time, spatial dimension, and highest level index (levels start at zero). |
| `geometry` | Dictionary with `dimension`, `coordinate_system`, `coordinate_system_code`, `domain_lower`, `domain_upper`, `mapped_grid`, and `physical_geometry`. |
| `levels` | Per-level dictionaries with `level`, `step`, `domain`, `cell_size`, `index_origin`, `ghost_cells`, `num_grids`, and `dtype` (`None` when it can only be read from a missing or unreadable FAB). |
| `widest_dtype` | Widest stored field precision across levels, as a NumPy dtype, or `None` if any level's `dtype` is `None`. |
| `leaf_cells(fields, region=None, dtype=None)` | Iterate over chunks. Also accepts `cancel=token` and `progress=callback`; the callback receives `(items_read, total_items)`. |
| `plan_leaf_chunks(fields, region=None, dtype=None)` | Create a plan. Also accepts `cancel=token`. |
| `read_leaf_chunk(item, cancel=None)` | Read one item; `cancel` is a keyword argument. |
| `cache_bytes`, `set_cache_bytes(n)`, `cache_metrics()` | Inspect or change the block-cache budget, and inspect cache metrics. |

## Chunk reference

| Member | Meaning |
| --- | --- |
| `chunk[name]` | Read-only field array of shape `(chunk.size,)`; `name` must have been requested. |
| `fields`, `dtype` | Requested field names and field-array dtype. |
| `size` | Number of selected leaf cells. `len(chunk)` counts fields, not cells. |
| `index`, `level`, `grid` | Plan-item index, AMR level, and grid index on that level. |
| `dimension`, `box`, `cell_size` | Spatial dimension, grid's valid inclusive index box, and tuple of cell widths. |
| `x`, `y`, `z`; `centers()` | Cell-center coordinate arrays, individually or as a tuple over available axes. |
| `i`, `j`, `k`; `indices()` | Integer cell-index arrays, individually or as a tuple. |
| `dx`, `dy`, `dz`; `widths()` | Cell-width arrays, individually or as a tuple. |
| `cell_volume` | Per-cell product of widths over available dimensions. |

Unavailable axes raise `AttributeError`: for example, a 2-D chunk has no
`z`, `k`, or `dz`. Every array follows the same cell order, so a boolean
mask applies consistently to fields and geometry.

## Cache limits and errors

`cache_bytes=` sets the block-cache budget in bytes. Otherwise the package
uses `AMREXPLORER_CACHE_SIZE_MB`, if set to a positive whole number of
megabytes, or defaults to 1 GiB. Planning rejects an item whose estimated
pinned blocks exceed that budget, naming the offending grid. Increase the
budget or reduce the requested fields; a smaller region need not reduce
the underlying blocks a read requires. The cache budget does not limit
memory retained by NumPy chunks or your script.

Exceptions are exported from `amrexplorer`:

| Exception | When to handle it | Parent |
| --- | --- | --- |
| `AnalysisError` | Unknown fields, unsupported geometry or centering, invalid selection/dtype, an item from another dataset, or a plan item estimated to exceed the cache budget. | `ValueError` |
| `DerivedFieldError` | A derived definition cannot be resolved. | `AnalysisError` |
| `PlotfileReadError` | Plotfile metadata cannot be read. | `OSError` |
| `BlockReadError` | A data block cannot be read. | `PlotfileReadError` |
| `CacheBudgetExceeded` | A read cannot fit its required blocks, or a budget reduction cannot accommodate pinned blocks. | `MemoryError` |
| `DatasetClosed` | An operation needs a dataset that has been closed. | `RuntimeError` |
| `ReadCancelled` | An open, plan, or read was cancelled using a `CancelToken`. | `RuntimeError` |

Pressing Ctrl-C while `open_plotfile`, `plan_leaf_chunks`, or a read is in
progress raises `KeyboardInterrupt` promptly, and a `signal` handler that
calls `token.cancel()` stops any of them that was given that token (each
takes `cancel=token`).

For example, a batch script can report a failed plotfile and continue:

```python
try:
    with amrexplorer.open_plotfile("plt00100") as ds:
        for chunk in ds.leaf_cells(["density"]):
            pass  # Process each chunk here.
except (amrexplorer.AnalysisError, amrexplorer.PlotfileReadError,
        amrexplorer.CacheBudgetExceeded) as error:
    print(f"Could not analyze plotfile: {error}")
```

Catching `AnalysisError` alone does not catch read errors or cache-budget
errors. Accessing an unrequested field in a chunk raises Python's `KeyError`.

## Parallel reductions

`amrexplorer.parallel.map_reduce` reads each chunk, maps it to a partial
result with your function, and combines the partials with a second
function. It reads several chunks at once:

```python
import numpy as np
from amrexplorer import parallel

mass = parallel.map_reduce(
    "plt00100", parallel.LeafCells(["density"]),
    lambda chunk: float(np.sum(chunk["density"] * chunk.cell_volume)),
    lambda total, partial: total + partial, 0.0,
    executor="threads", workers=8)
print(mass)
```

`LeafCells` takes the same `fields`, `region`, and `dtype` as
`plan_leaf_chunks`. The first argument is a plotfile path or an open
dataset. The third positional argument, here `0.0`, is the starting value;
omit it and the first partial starts the reduction.

| Keyword | Default | Meaning |
| --- | --- | --- |
| `executor` | `"threads"` | `"serial"`, `"threads"`, or `"processes"`. |
| `workers` | `AMREXPLORER_WORKERS`, otherwise the CPUs available to the process, at most 8 | Threads or processes to run. |
| `max_in_flight` | `2 * workers` | Chunks being read, mapped, or waiting to be combined at once. |
| `ordered` | `True` | Combine partials in plan order, so the result is bit-for-bit the serial loop's. `False` combines them as they finish, which can change the last bits of floating-point sums from run to run. |
| `ownership` | `"worker"` | `"worker"` gives each worker its own dataset handle. `"shared"` (threads only) uses one handle for every thread. |
| `items` | every item | Plan positions or plan items to process; see MPI below. |
| `cache_bytes`, `derived` | as `open_plotfile` | How each handle opens the plotfile. |
| `progress`, `cancel` | none | `progress(done, total)` after each item; a `CancelToken` to stop. |

Memory: with `ownership="worker"`, each worker drops its cached blocks after
every chunk, because no chunk is read twice. Peak memory is about
`max_in_flight` chunks and their blocks, plus waiting partials, plus
whatever the dataset you passed in already caches; when you pass a path, the
handle opened for it is cleared the same way. Raise
`workers` or `max_in_flight` and memory grows with them. `ownership="shared"`
keeps one cache of up to `cache_bytes` and never clears it, since the handle
may be yours. It serializes every block read through that one handle, so it
is slower, and can be slower than `executor="serial"`.

### Processes

Threads parallelize the reads, but a mapper doing a lot of pure-Python work
holds the GIL; processes avoid that. Each worker process imports your
script's module, so the mapper and reducer must be module-level functions,
not lambdas, and the call must run under `if __name__ == "__main__":`.
Without the guard, the `spawn` start method (the default on macOS and
Windows) and `forkserver` (the default on Linux from Python 3.14) fail with
an error about the bootstrapping phase. A complete script:

```python
import sys

import numpy as np
from amrexplorer import parallel


def mass(chunk):
    return float(np.sum(chunk["density"] * chunk.cell_volume))


def add(total, partial):
    return total + partial


if __name__ == "__main__":
    print(parallel.map_reduce(sys.argv[1], parallel.LeafCells(["density"]),
                              mass, add, 0.0, executor="processes"))
```

### MPI

The package does not depend on MPI; install `mpi4py` yourself. Every rank
plans the same request, processes its share of the plan, and rank 0 combines
the rank results:

```python
from functools import reduce

import numpy as np
from mpi4py import MPI

import amrexplorer
from amrexplorer import parallel


def mass(chunk):
    return float(np.sum(chunk["density"] * chunk.cell_volume))


def add(total, partial):
    return total + partial


comm = MPI.COMM_WORLD
request = parallel.LeafCells(["density"])
with amrexplorer.open_plotfile("plt00100") as ds:
    plan = request.plan(ds)
    mine = parallel.map_reduce(ds, request, mass, add, 0.0, executor="serial",
                               items=plan.partition(comm.rank, comm.size))
partials = comm.gather(mine, root=0)
if comm.rank == 0:
    print(reduce(add, partials))  # in rank order
```

Run it with `mpiexec -n 4 python mass.py`. `partition(rank, size)` deals the
plan's items round-robin, so each rank gets a similar mix of coarse and fine
grids. Combining the gathered results in rank order makes the answer the same
from run to run for a given number of ranks. It groups the partials
differently from a serial loop, though, so floating-point sums can differ
from the serial result in the last bits. Each rank above reads serially; to
use threads within a rank too, set `executor="threads"` and choose `workers`
(or `AMREXPLORER_WORKERS`) so ranks times workers fits the node's cores.

## Batches

`amrexplorer.sequence` handles a run's whole series of plotfiles.
`find_plotfiles(directory)` lists the plotfiles in step order, skipping
non-plotfile entries such as images written beside them. `open_each(paths)`
opens them one at a time, closing each before the next, so only one cache is
alive. `output_path(plotfile, suffix)` names the output for a plotfile, by
default beside it, and `needs_output(path)` is false when that output already
exists, so a rerun after a failure or time limit skips finished plotfiles:

```python
import json

import numpy as np
from amrexplorer import sequence

for ds in sequence.open_each(sequence.find_plotfiles("outputs/")):
    out = sequence.output_path(ds.path, "_mass.json")
    if not sequence.needs_output(out):
        continue
    mass = sum(float(np.sum(c["density"] * c.cell_volume))
               for c in ds.leaf_cells(["density"]))
    out.write_text(json.dumps({"time": ds.time, "mass": mass}))
```

Pass `overwrite=True` to `needs_output` to redo everything, or
`directory=` to `output_path` to write elsewhere. Inside the loop,
`parallel.map_reduce(ds, ...)` parallelizes each plotfile.

## Development build

For a CMake build instead of a pip install, activate a virtual environment
and install the development dependencies:

```sh
python -m pip install nanobind numpy pytest
cmake -S . -B build-python -G Ninja -DAMREXPLORER_ENABLE_QT=OFF \
    -DAMREXPLORER_ENABLE_REMOTE=OFF -DAMREXPLORER_ENABLE_PYTHON=ON \
    -DAMREXPLORER_BUILD_TESTS=ON
cmake --build build-python
PYTHONPATH=build-python/python python -c "import amrexplorer"
ctest --test-dir build-python -R '^python_analysis$' \
    --output-on-failure --no-tests=error
```

These commands use a POSIX shell and require Ninja. Set
`-DPython_EXECUTABLE=/path/to/python` at configuration if you need a specific
interpreter. The module and Python tests use the same interpreter.
With pytest or NumPy missing, CMake
omits the test; `--no-tests=error` makes that visible when running CTest.
