"""Datasets, leaf-chunk plans, and chunks."""

from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Iterable, Iterator, Mapping, Optional, Sequence, Tuple, Union

import numpy as np

from . import _amrexplorer as _native

_AXES = ("x", "y", "z")
_INDEX_NAMES = ("i", "j", "k")
_WIDTH_NAMES = ("dx", "dy", "dz")

Region = Optional[Tuple[Sequence[float], Sequence[float]]]


@dataclass(frozen=True)
class DerivedField:
    """A derived-field definition: a name and an expression in the viewer's
    expression syntax, over the plotfile's stored fields, earlier derived
    fields, and the coordinates x, y, z. Values are native numbers."""

    name: str
    expression: str


def _definitions(derived) -> list:
    result = []
    for entry in derived or ():
        if isinstance(entry, DerivedField):
            result.append((entry.name, entry.expression))
        elif isinstance(entry, Mapping):
            result.append((str(entry["name"]), str(entry["expression"])))
        else:
            name, expression = entry
            result.append((str(name), str(expression)))
    return result


def _dtype_name(dtype) -> Optional[str]:
    if dtype is None:
        return None
    name = np.dtype(dtype).name
    if name not in ("float32", "float64"):
        raise _native.AnalysisError(
            f"dtype must be float32 or float64, not {name}")
    return name


def _region(region, dimension: int):
    if region is None:
        return None
    if isinstance(region, Mapping):
        lower, upper = region["lower"], region["upper"]
    else:
        lower, upper = region
    lower = [float(v) for v in np.atleast_1d(lower)]
    upper = [float(v) for v in np.atleast_1d(upper)]
    if len(lower) != dimension or len(upper) != dimension:
        raise _native.AnalysisError(
            f"region bounds must have {dimension} values each for this "
            f"{dimension}-D plotfile")
    return (lower, upper)


class PlanItem:
    """One work item: exactly one grid's selected leaf cells. Immutable, valid
    only for the dataset and plan it came from."""

    __slots__ = ("_native", "_dimension")

    def __init__(self, native, dimension: int):
        self._native = native
        self._dimension = dimension

    @property
    def index(self) -> int:
        """Stable position in the plan's canonical order."""
        return self._native.index

    @property
    def level(self) -> int:
        return self._native.level

    @property
    def grid(self) -> int:
        """The grid's native FAB index on its level."""
        return self._native.grid

    @property
    def num_cells(self) -> int:
        """Selected leaf cells in this item."""
        return self._native.num_cells

    @property
    def estimated_bytes(self) -> int:
        """Planning-time upper bound on the block-cache bytes a read pins."""
        return self._native.estimated_bytes

    @property
    def dataset_id(self) -> int:
        return self._native.dataset_id

    @property
    def box(self):
        """The grid's valid index box, as (lower, upper) inclusive tuples."""
        lower, upper = self._native._box(self._dimension)
        return tuple(lower), tuple(upper)

    @property
    def selected_box(self):
        """The part of the box the region selects, by cell center."""
        lower, upper = self._native._selected_box(self._dimension)
        return tuple(lower), tuple(upper)

    def __repr__(self) -> str:
        return (f"PlanItem(index={self.index}, level={self.level}, "
                f"grid={self.grid}, num_cells={self.num_cells})")


class LeafChunkPlan(Sequence):
    """A deterministic, ordered partition of the selected leaf cells into
    one item per grid. Planning reads metadata only."""

    def __init__(self, native, dimension: int, region):
        self._native = native
        self._dimension = dimension
        self._region = region

    def __len__(self) -> int:
        return len(self._native)

    def __getitem__(self, index):
        if isinstance(index, slice):
            return [self[i] for i in range(*index.indices(len(self)))]
        return PlanItem(_native._plan_item(self._native, int(index)),
                        self._dimension)

    def __iter__(self) -> Iterator[PlanItem]:
        for index in range(len(self)):
            yield self[index]

    @property
    def fields(self) -> list:
        return list(self._native.fields)

    @property
    def dtype(self) -> np.dtype:
        return np.dtype(self._native.dtype)

    @property
    def region(self):
        return self._region

    @property
    def total_cells(self) -> int:
        """Leaf cells across every item."""
        return self._native.total_cells

    @property
    def dataset_id(self) -> int:
        return self._native.dataset_id

    def partition(self, rank: int, size: int) -> list:
        """The items rank `rank` of `size` workers takes under a static,
        round-robin split. The union over ranks is the whole plan, each item
        exactly once, independent of read order."""
        if size < 1 or not 0 <= rank < size:
            raise _native.AnalysisError(
                "partition needs size >= 1 and 0 <= rank < size")
        return [self[i] for i in range(rank, len(self), size)]

    def __repr__(self) -> str:
        return (f"LeafChunkPlan(items={len(self)}, "
                f"total_cells={self.total_cells}, fields={self.fields}, "
                f"dtype={self.dtype.name})")


class LeafChunk(Mapping):
    """One grid's selected leaf cells, copied out of the block cache. Holds
    no cache pins and stays valid after the dataset closes. Every array is a
    read-only 1-D vector over the same cells in canonical order (x fastest),
    so one boolean mask indexes all of them."""

    def __init__(self, native):
        self._native = native
        self._cache = {}

    # Field access ---------------------------------------------------------
    def __getitem__(self, name: str) -> np.ndarray:
        try:
            return self._cache[name]
        except KeyError:
            pass
        if name not in self._native.fields:
            raise KeyError(name)
        array = self._native._field(name)
        self._cache[name] = array
        return array

    def __iter__(self):
        return iter(self._native.fields)

    def __len__(self) -> int:
        return len(self._native.fields)

    @property
    def fields(self) -> list:
        return list(self._native.fields)

    @property
    def size(self) -> int:
        """Number of leaf cells in the chunk."""
        return len(self._native)

    @property
    def index(self) -> int:
        """The plan item this chunk was read from."""
        return self._native.index

    @property
    def level(self) -> int:
        return self._native.level

    @property
    def grid(self) -> int:
        return self._native.grid

    @property
    def dimension(self) -> int:
        return self._native.dimension

    @property
    def dtype(self) -> np.dtype:
        return np.dtype(self._native.dtype)

    @property
    def cell_size(self) -> tuple:
        return tuple(self._native.cell_size)

    @property
    def box(self):
        lower, upper = self._native.box
        return tuple(lower), tuple(upper)

    # Geometry, computed on first access -----------------------------------
    def _geometry(self, key, compute):
        try:
            return self._cache[key]
        except KeyError:
            value = compute()
            self._cache[key] = value
            return value

    def _axis(self, axis: int, name: str) -> int:
        if axis >= self.dimension:
            raise AttributeError(
                f"a {self.dimension}-D chunk has no '{name}'")
        return axis

    @property
    def cell_volume(self) -> np.ndarray:
        """Product of the cell widths over the plotfile's dimensions, in
        native coordinate units to that power. Always float64."""
        return self._geometry(" volume", self._native._cell_volume)

    def __getattr__(self, name: str):
        if name in _INDEX_NAMES:
            axis = self._axis(_INDEX_NAMES.index(name), name)
            return self._geometry(" i%d" % axis,
                                  lambda: self._native._index(axis))
        if name in _AXES:
            axis = self._axis(_AXES.index(name), name)
            return self._geometry(" x%d" % axis,
                                  lambda: self._native._center(axis))
        if name in _WIDTH_NAMES:
            axis = self._axis(_WIDTH_NAMES.index(name), name)
            return self._geometry(" dx%d" % axis,
                                  lambda: self._native._width(axis))
        raise AttributeError(name)

    def indices(self) -> tuple:
        """The integer cell indices, one array per axis."""
        return tuple(getattr(self, n) for n in _INDEX_NAMES[: self.dimension])

    def centers(self) -> tuple:
        """The cell-center coordinates, one array per axis."""
        return tuple(getattr(self, n) for n in _AXES[: self.dimension])

    def widths(self) -> tuple:
        """The cell widths, one array per axis."""
        return tuple(getattr(self, n) for n in _WIDTH_NAMES[: self.dimension])

    def __repr__(self) -> str:
        return (f"LeafChunk(index={self.index}, level={self.level}, "
                f"grid={self.grid}, size={self.size}, fields={self.fields})")


class ParticleItem:
    """One particle work item: exactly one grid's particles of one species."""

    __slots__ = ("_native",)

    def __init__(self, native):
        self._native = native

    @property
    def index(self) -> int:
        return self._native.index

    @property
    def level(self) -> int:
        return self._native.level

    @property
    def grid(self) -> int:
        return self._native.grid

    @property
    def num_particles(self) -> int:
        return self._native.num_particles

    @property
    def dataset_id(self) -> int:
        return self._native.dataset_id

    def __repr__(self) -> str:
        return (f"ParticleItem(index={self.index}, level={self.level}, "
                f"grid={self.grid}, num_particles={self.num_particles})")


class ParticlePlan(Sequence):
    """One species' grids that hold particles, in level and then native grid
    order -- the order the species' Header lists them."""

    def __init__(self, native):
        self._native = native

    def __len__(self) -> int:
        return len(self._native)

    def __getitem__(self, index):
        if isinstance(index, slice):
            return [self[i] for i in range(*index.indices(len(self)))]
        return ParticleItem(_native._particle_item(self._native, int(index)))

    def __iter__(self) -> Iterator[ParticleItem]:
        for index in range(len(self)):
            yield self[index]

    @property
    def species(self) -> str:
        return self._native.species

    @property
    def fields(self) -> list:
        """The requested attributes, positions excluded."""
        dim = self._native.dimension
        return list(self._native.real_fields)[dim:] + list(self._native.int_fields)

    @property
    def total_particles(self) -> int:
        return self._native.total_particles

    @property
    def dataset_id(self) -> int:
        return self._native.dataset_id

    def partition(self, rank: int, size: int) -> list:
        if size < 1 or not 0 <= rank < size:
            raise _native.AnalysisError(
                "partition needs size >= 1 and 0 <= rank < size")
        return [self[i] for i in range(rank, len(self), size)]

    def __repr__(self) -> str:
        return (f"ParticlePlan(species={self.species!r}, items={len(self)}, "
                f"total_particles={self.total_particles})")


class ParticleChunk(Mapping):
    """One grid's particles, copied: complete named attributes, positions
    (chunk.x, chunk.y, chunk.z), and stable ids (chunk.id with chunk.cpu,
    AMReX's persistent identity). Real attributes keep the species' stored
    precision; integer attributes are int32. Arrays are read-only and
    outlive the dataset."""

    def __init__(self, native):
        self._native = native
        self._cache = {}
        dim = native.dimension
        reals = list(native.real_fields)
        self._positions = {name: column for column, name in enumerate(reals[:dim])}
        self._reals = {name: column for column, name in enumerate(reals)
                       if column >= dim}
        self._ints = {name: column
                      for column, name in enumerate(native.int_fields)}

    def __getitem__(self, name: str) -> np.ndarray:
        try:
            return self._cache[name]
        except KeyError:
            pass
        if name in self._reals:
            array = self._native._real(self._reals[name])
        elif name in self._ints:
            array = self._native._int(self._ints[name])
        elif name in self._positions:
            array = self._native._real(self._positions[name])
        else:
            raise KeyError(name)
        self._cache[name] = array
        return array

    def __iter__(self):
        return iter(list(self._reals) + list(self._ints))

    def __len__(self) -> int:
        return len(self._reals) + len(self._ints)

    def __getattr__(self, name: str):
        if name in _AXES:
            if name not in self._positions:
                raise AttributeError(
                    f"a {self.dimension}-D particle chunk has no '{name}'")
            return self[name]
        raise AttributeError(name)

    @property
    def id(self) -> np.ndarray:
        if " id" not in self._cache:
            self._cache[" id"] = self._native._ids()
        return self._cache[" id"]

    @property
    def cpu(self) -> np.ndarray:
        if " cpu" not in self._cache:
            self._cache[" cpu"] = self._native._cpus()
        return self._cache[" cpu"]

    def positions(self) -> tuple:
        return tuple(self[name] for name in _AXES[: self.dimension])

    @property
    def size(self) -> int:
        return len(self._native)

    @property
    def species(self) -> str:
        return self._native.species

    @property
    def index(self) -> int:
        return self._native.index

    @property
    def level(self) -> int:
        return self._native.level

    @property
    def grid(self) -> int:
        return self._native.grid

    @property
    def dimension(self) -> int:
        return self._native.dimension

    @property
    def fields(self) -> list:
        return list(self)

    def __repr__(self) -> str:
        return (f"ParticleChunk(species={self.species!r}, index={self.index}, "
                f"level={self.level}, grid={self.grid}, size={self.size})")


class Dataset:
    """An open plotfile. Use as a context manager, or call close()."""

    def __init__(self, path, derived=(), cache_bytes: Optional[int] = None):
        # Kept so another handle can be opened on the same definitions
        # (amrexplorer.parallel's worker-local datasets).
        self._definitions = _definitions(derived)
        self._native = _native._Dataset(
            os.fspath(path), self._definitions,
            None if cache_bytes is None else int(cache_bytes))

    # Lifetime ---------------------------------------------------------------
    def close(self) -> None:
        """Release the dataset and its block cache. Chunks already read stay
        valid; plan items can no longer be read."""
        self._native.close()

    @property
    def closed(self) -> bool:
        return self._native.closed

    def __enter__(self) -> "Dataset":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # Metadata ----------------------------------------------------------------
    @property
    def id(self) -> int:
        """Unique within this process; plan items record it."""
        return self._native.id

    @property
    def path(self) -> str:
        return self._native.path

    @property
    def fields(self) -> list:
        """Every field, stored then derived, by exact name."""
        return list(self._native.fields)

    @property
    def stored_fields(self) -> list:
        return list(self._native.stored_fields)

    @property
    def derived_fields(self) -> list:
        return list(self._native.derived_fields)

    def field_centering(self, name: str) -> str:
        return self._native.field_centering(name)

    @property
    def time(self) -> float:
        """Plotfile time, in the plotfile's own units."""
        return self._native.time

    @property
    def dimension(self) -> int:
        return self._native.dimension

    @property
    def finest_level(self) -> int:
        return self._native.finest_level

    @property
    def geometry(self) -> dict:
        """Dimension, coordinate system, and native-unit domain bounds."""
        return dict(self._native.geometry)

    @property
    def levels(self) -> list:
        """Per level: index domain, cell size, index origin, stored ghost
        cells, grid count, and stored dtype."""
        return [dict(level) for level in self._native.levels]

    @property
    def widest_dtype(self) -> np.dtype:
        return np.dtype(self._native.widest_dtype)

    # Cache -----------------------------------------------------------------
    @property
    def cache_bytes(self) -> int:
        return self._native.cache_bytes

    def set_cache_bytes(self, n: int) -> None:
        """Change the block-cache budget. Raises CacheBudgetExceeded, leaving
        the budget unchanged, if pinned blocks exceed the new one."""
        self._native.set_cache_bytes(int(n))

    def cache_metrics(self) -> dict:
        return dict(self._native.cache_metrics())

    # Leaf cells -------------------------------------------------------------
    def plan_leaf_chunks(self, fields: Iterable[str] = (), region: Region = None,
                         dtype=None) -> LeafChunkPlan:
        """Plan the leaf cells whose centers lie in `region` (lower <= center
        < upper per axis; None is the whole domain), one item per grid that
        has any. `dtype=None` uses the widest stored dtype across levels."""
        if isinstance(fields, str):
            fields = [fields]
        native_region = _region(region, self.dimension)
        plan = self._native.plan_leaf_chunks(
            list(fields), native_region, _dtype_name(dtype))
        return LeafChunkPlan(plan, self.dimension, native_region)

    def read_leaf_chunk(self, item: PlanItem, *, cancel=None) -> LeafChunk:
        """Copy one item's selected cells into arrays the chunk owns.
        `cancel` is an optional CancelToken."""
        if not isinstance(item, PlanItem):
            raise TypeError("read_leaf_chunk takes a PlanItem")
        return LeafChunk(self._native.read_leaf_chunk(item._native, cancel))

    def leaf_cells(self, fields: Iterable[str] = (), region: Region = None,
                   dtype=None, *, cancel=None,
                   progress=None) -> Iterator[LeafChunk]:
        """Plan, then read every item serially in canonical order.
        `progress(done, total)` is called after each item is read."""
        plan = self.plan_leaf_chunks(fields, region=region, dtype=dtype)
        for done, item in enumerate(plan, start=1):
            chunk = self.read_leaf_chunk(item, cancel=cancel)
            if progress is not None:
                progress(done, len(plan))
            yield chunk

    # Tiles --------------------------------------------------------------------
    def plan_tiles(self, fields: Iterable[str], *, ghost: int = 1,
                   region: Region = None, dtype=None, periodic=None,
                   interpolation: str = "linear"):
        """Plan one tile per FAB with leaf cells in `region` (matching
        plan_leaf_chunks item for item), each grown by `ghost` cells.

        Ghost cells come from same-level grids where they exist, and are
        interpolated from coarser levels elsewhere ("linear": central
        slopes, exact for linear fields; "limited": AMReX's MC-limited
        slopes; "constant"). `periodic` is one bool per axis (default all
        False): a plotfile does not record periodicity. Ghost cells outside
        a non-periodic domain are invalid (NaN)."""
        from ._tiles import TilePlan

        if isinstance(fields, str):
            fields = [fields]
        periodic = list(periodic) if periodic is not None else []
        periodic = [bool(p) for p in periodic] + [False] * (3 - len(periodic))
        native = self._native.plan_tiles(
            list(fields), _region(region, self.dimension), _dtype_name(dtype),
            int(ghost), periodic[:3], interpolation)
        return TilePlan(native, self.dimension)

    def read_tile(self, item, *, cancel=None):
        from ._tiles import Tile, TileItem

        if not isinstance(item, TileItem):
            raise TypeError("read_tile takes a TileItem")
        return Tile(self._native.read_tile(item._native, cancel))

    def tiles(self, fields: Iterable[str], *, ghost: int = 1, region: Region = None,
              dtype=None, periodic=None, interpolation: str = "linear",
              cancel=None):
        """Plan tiles, then read them serially in canonical order."""
        for item in self.plan_tiles(fields, ghost=ghost, region=region, dtype=dtype,
                                    periodic=periodic, interpolation=interpolation):
            yield self.read_tile(item, cancel=cancel)

    # Blocks -------------------------------------------------------------------
    def blocks(self, level: Optional[int] = None) -> list:
        """The plotfile's grids (FABs), by level and then native FAB index:
        each with its level, FAB index ("grid"), valid index box, physical
        bounds, cell count, the cells the next finer level covers
        ("covered_cells") and "covered_fraction", and its data file and
        offset."""
        blocks = [dict(block) for block in self._native.blocks(level)]
        for block in blocks:
            block["box"] = tuple(tuple(side) for side in block["box"])
            block["covered_fraction"] = block["covered_cells"] / block["cells"]
        return blocks

    def block_statistics(self, fields: Iterable[str], level: Optional[int] = None,
                         grid: Optional[int] = None, *, cancel=None) -> list:
        """Per-block minima and maxima of each field, from the decoded data
        with NaN ignored (a block of all NaN reports NaN). A stored field's
        extrema cover the whole on-disk FAB, covered cells and stored ghost
        cells included; a derived field's cover its valid box. Each entry
        says which box ("box", "includes_ghost_cells", per field)."""
        if isinstance(fields, str):
            fields = [fields]
        entries = [dict(entry) for entry in self._native.block_statistics(
            list(fields), level, grid, cancel)]
        for entry in entries:
            entry["box"] = {name: tuple(tuple(side) for side in box)
                            for name, box in entry["box"].items()}
        return entries

    # Projections --------------------------------------------------------------
    def project(self, field: str, axis="z", *, bounds=None, rect=None,
                resolution=None, weight: Optional[str] = None,
                zoom: float = 1.0, method: str = "integrate", cancel=None):
        """Exactly project leaf cells along `axis` onto a uniform image.

        Every leaf cell contributes its value times the volume it shares
        with each pixel's column -- fractional overlap, not cell centers --
        so the projection conserves the integral. Defaults: the whole
        domain in the image plane, one pixel per level-0 cell (`zoom` about
        the center), and the whole domain along the line of sight. Returns a
        Projection; see its documentation for normalization and coverage.
        """
        from ._projection import project

        return project(self, field, axis, bounds=bounds, rect=rect,
                       resolution=resolution, weight=weight, zoom=zoom,
                       method=method, cancel=cancel)

    def slice(self, field: str, axis="z", coordinate=None, *, rect=None,
              resolution=None, sampling: str = "piecewise", max_level=None,
              composition: str = "finest", zoom: float = 1.0, cancel=None):
        """Sample a plane at pixel centers through the viewer's query layer,
        returning values with validity, source level and pixel coordinates.

        An approximate raster -- each pixel reads one point -- unlike
        slice_cell_average, which averages cells over each pixel exactly.
        Defaults: the middle level-0 cell along `axis`, the whole domain in
        the plane at one pixel per level-0 cell. A 2-D plotfile is its own
        plane, so `axis` and `coordinate` are ignored there.
        """
        from ._projection import slice_sampled

        return slice_sampled(self, field, axis, coordinate, rect=rect,
                             resolution=resolution, sampling=sampling,
                             max_level=max_level, composition=composition,
                             zoom=zoom, cancel=cancel)

    def slice_cell_average(self, field: str, axis="z", coordinate=None, *,
                           rect=None, resolution=None, zoom: float = 1.0,
                           cancel=None):
        """The exact area-weighted average, over each pixel, of the leaf
        cells a plane passes through (Kangaroo's AMR cell-average slice)."""
        from ._projection import slice_cell_average

        return slice_cell_average(self, field, axis, coordinate, rect=rect,
                                  resolution=resolution, zoom=zoom,
                                  cancel=cancel)

    # Particles ----------------------------------------------------------------
    @property
    def particle_species(self) -> list:
        """Each species: name, particle count, precision, and its real and
        integer component names (positions excluded)."""
        return [dict(entry) for entry in self._native.particle_species()]

    def plan_particle_chunks(self, species: str,
                             fields: Optional[Iterable[str]] = None) -> ParticlePlan:
        """Plan one item per grid holding particles of `species`. `fields`
        names the attributes to read (default: all); positions and ids are
        always read."""
        if isinstance(fields, str):
            fields = [fields]
        return ParticlePlan(self._native.plan_particle_chunks(
            species, None if fields is None else list(fields)))

    def deposit_particles(self, species: str, weight: Optional[str] = None,
                          axis="z", *, bounds=None, rect=None, resolution=None,
                          method: str = "cic", mask=None, periodic=None,
                          zoom: float = 1.0, cancel=None, progress=None):
        """Deposit `weight` (a real particle attribute; None counts particles)
        with conservative, AMR-aware cloud-in-cell weights, then project the
        deposited leaf-cell field onto a uniform image. Particles are
        selected by line-of-sight position in `bounds` and, optionally, by
        `mask(chunk) -> bool array`, called on every particle chunk read.
        Returns a Deposition; see include/amrexplorer/analysis/Deposition.hpp
        for the boundary rules."""
        from ._projection import deposit_particles

        return deposit_particles(self, species, weight, axis, bounds=bounds,
                                 rect=rect, resolution=resolution, method=method,
                                 mask=mask, periodic=periodic, zoom=zoom,
                                 cancel=cancel, progress=progress)

    def read_particle_chunk(self, item: ParticleItem, *,
                            cancel=None) -> ParticleChunk:
        if not isinstance(item, ParticleItem):
            raise TypeError("read_particle_chunk takes a ParticleItem")
        return ParticleChunk(self._native.read_particle_chunk(item._native,
                                                              cancel))

    def particle_chunks(self, species: str,
                        fields: Optional[Iterable[str]] = None, *,
                        cancel=None, progress=None) -> Iterator[ParticleChunk]:
        """Plan, then read every grid's particles serially, in order."""
        plan = self.plan_particle_chunks(species, fields)
        for done, item in enumerate(plan, start=1):
            chunk = self.read_particle_chunk(item, cancel=cancel)
            if progress is not None:
                progress(done, len(plan))
            yield chunk

    def __repr__(self) -> str:
        state = "closed" if self.closed else f"{self.dimension}-D"
        return f"Dataset({self._native.path if not self.closed else ''!r}, {state})"


def open_plotfile(path, derived=(), cache_bytes: Optional[int] = None) -> Dataset:
    """Open a plotfile for analysis.

    derived: derived-field definitions, each a DerivedField, a (name,
        expression) pair, or a mapping with "name" and "expression". A
        definition that does not resolve raises DerivedFieldError here.
        The viewer's saved definitions are never loaded.
    cache_bytes: the block-cache budget. None uses AMREXPLORER_CACHE_SIZE_MB
        if set (raising if it is not a positive whole number) and 1 GiB
        otherwise.
    """
    return Dataset(path, derived=derived, cache_bytes=cache_bytes)
