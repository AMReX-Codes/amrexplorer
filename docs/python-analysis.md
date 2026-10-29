# Analyzing plotfiles from Python

The `amrexplorer` Python package reads AMReX plotfiles through the same
Qt-free C++ readers the viewer uses, and gives scripts exact, native-unit
access to them: every leaf cell exactly once, in chunks a script can reduce
serially or in parallel, and per-block statistics.
`tools/analysis_validation/run_comparisons.sh` compares it with yt and
Kangaroo.

Values are always the plotfile's own numbers. Nothing reads a unit system or
converts anything: a script that wants solar masses divides by the gram value
of a solar mass itself.

## Installing

From a checkout, with a C++20 compiler and CMake 3.25 or newer:

```sh
python -m pip install .
```

This builds only the analysis extension (no Qt, no remote protocol). For
development, configure a normal build with the module switched on and point
`PYTHONPATH` at it:

```sh
cmake -S . -B build-python -G Ninja -DAMREXPLORER_ENABLE_QT=OFF \
    -DAMREXPLORER_ENABLE_REMOTE=OFF -DAMREXPLORER_ENABLE_PYTHON=ON \
    -DPython_EXECUTABLE=$(which python3)
cmake --build build-python
PYTHONPATH=build-python/python python3 -c "import amrexplorer"
```

The interpreter must have `nanobind` and `numpy` (`pip install nanobind
numpy`). With `pytest` installed as well, `ctest` runs the Python suite as
the `python_analysis` test.

## Leaf cells

```python
import numpy as np
import amrexplorer

with amrexplorer.open_plotfile("plt00100") as ds:
    print(ds.fields, ds.time, ds.levels)
    plan = ds.plan_leaf_chunks(["gasDensity", "temperature"],
                               region=([-1e21] * 3, [1e21] * 3))
    cold = 0.0
    for item in plan:                       # one item per grid (FAB)
        chunk = ds.read_leaf_chunk(item)
        rho = chunk["gasDensity"]
        mask = chunk["temperature"] < 200
        cold += np.sum(rho[mask] * chunk.cell_volume[mask])
```

- A plan lists the grids that hold leaf cells -- cells no finer level
  covers -- in a fixed order: levels coarsest first, grids in the order the
  level header lists them. Within a chunk, cells are in FAB order, x fastest.
  Every leaf cell of the region appears in exactly one chunk.
- `region` selects cells by center, `lower <= center < upper` on each axis,
  so adjacent regions partition the cells between them.
- Field arrays use the widest precision any level stores, unless `dtype=`
  says otherwise. Geometry (`chunk.x`, `.y`, `.z`, `.dx`, `.i`, ...,
  `.cell_volume`) is float64 and computed on first use. A 2-D chunk has no
  `z`.
- Chunks are copies: they hold no cache memory and outlive the dataset.
  Arrays are read-only.
- `ds.leaf_cells(...)` plans and reads in one loop.

Derived fields are defined when the dataset opens, in the viewer's expression
syntax, and fail at open if they do not resolve:

```python
ds = amrexplorer.open_plotfile("plt00100", derived=[("vx", "${x-GasMomentum}/gasDensity")])
```

`cache_bytes=` sets the block cache, otherwise `AMREXPLORER_CACHE_SIZE_MB` or
1 GiB. A plan refuses, with the grid named, any item that might not fit.

## Parallel reductions

```python
from amrexplorer import parallel

total = parallel.map_reduce("plt00100", parallel.LeafCells(["gasDensity"]),
                            lambda c: float(np.sum(c["gasDensity"] * c.cell_volume)),
                            lambda a, b: a + b, executor="threads", workers=8)
```

Each worker opens its own handle and plans the same request; at most
`max_in_flight` items are in progress at once; partials are combined in plan
order, so the result is bit-for-bit the serial one. `executor="processes"`
needs a picklable mapper. For MPI, plan on every rank and pass
`items=plan.partition(rank, size)`. A `CancelToken` stops reads in progress.

## Grids

`ds.blocks()` lists every grid with its box, bounds and covered fraction;
`ds.block_statistics(fields)` gives per-grid minima and maxima from the
decoded data, NaN ignored.

## Batches

`amrexplorer.sequence.find_plotfiles(directory)` lists a run's plotfiles in
step order, `open_each(paths)` opens them one at a time, and `output_path` /
`needs_output` skip outputs already written. Long operations take
`progress=` callbacks and `cancel=` tokens.

## Examples

`python/examples/plotfile_fab_minmax.py` ports Kangaroo's script of the same
name, keeping its arguments and output, and says in its docstring what it
validates.
