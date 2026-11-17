"""Datasets, leaf-chunk plans, and chunks."""

from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Iterable, Iterator, Mapping, Optional, Sequence, Tuple

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
    if derived is None:
        return []
    if isinstance(derived, (str, bytes)):
        raise TypeError(
            "derived takes a sequence of definitions or a {name: expression} "
            "mapping, not a string")
    if isinstance(derived, Mapping):
        # {name: expression}, in the mapping's order.
        return [(str(name), str(expression))
                for name, expression in derived.items()]
    result = []
    for entry in derived:
        if isinstance(entry, DerivedField):
            result.append((entry.name, entry.expression))
        elif isinstance(entry, Mapping):
            result.append((str(entry["name"]), str(entry["expression"])))
        elif isinstance(entry, (str, bytes)):
            # A bare string would otherwise unpack character by character.
            raise TypeError(
                f"derived definition {entry!r} is a string; give a "
                "DerivedField, a (name, expression) pair, or a mapping with "
                "'name' and 'expression'")
        else:
            try:
                name, expression = entry
            except (TypeError, ValueError):
                raise TypeError(
                    f"derived definition {entry!r} is not a DerivedField, a "
                    "(name, expression) pair, or a mapping with 'name' and "
                    "'expression'") from None
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
    only for the dataset and plan it came from. Two items are equal when they
    are the same position of the same plan."""

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

    def _key(self):
        return (self._native._plan_key, self._native.index)

    def __eq__(self, other):
        if not isinstance(other, PlanItem):
            return NotImplemented
        return self._key() == other._key()

    def __hash__(self) -> int:
        return hash(self._key())

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

    # An item knows its plan and position, so these need no scan.
    def __contains__(self, item) -> bool:
        return (isinstance(item, PlanItem)
                and item._native._plan_key == self._native._key)

    def index(self, item, start: int = 0, stop: Optional[int] = None) -> int:
        if item in self:
            position = item.index
            begin, end, _ = slice(start, stop).indices(len(self))
            if begin <= position < end:
                return position
        raise ValueError(f"{item!r} is not in this plan")

    def count(self, item) -> int:
        return 1 if item in self else 0

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
    so one boolean mask indexes all of them. Chunks compare and hash by
    identity: Mapping's equality would compare the arrays elementwise, which
    has no single truth value."""

    __eq__ = object.__eq__
    __ne__ = object.__ne__
    __hash__ = object.__hash__

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


class Dataset:
    """An open plotfile. Use as a context manager, or call close()."""

    def __init__(self, path, derived=(), cache_bytes: Optional[int] = None,
                 *, cancel=None):
        # Kept so another handle can be opened on the same definitions
        # (amrexplorer.parallel's worker-local datasets).
        self._definitions = _definitions(derived)
        # As opened, not the directory the native side reads from: for a
        # standalone FAB or MultiFab that is the file's parent, which does not
        # reopen the same dataset.
        self._path = os.fsdecode(os.fspath(path))
        self._native = _native._Dataset(
            os.fspath(path), self._definitions,
            None if cache_bytes is None else int(cache_bytes), cancel)

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
        """The path the dataset was opened with, so open_plotfile(ds.path)
        opens it again."""
        return self._path

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
        cells, grid count, and stored dtype. The dtype is None when it can
        only be read from a FAB that is missing or unreadable."""
        return [dict(level) for level in self._native.levels]

    @property
    def widest_dtype(self) -> Optional[np.dtype]:
        """The widest stored dtype across levels, or None when a level's
        dtype cannot be read (see levels)."""
        name = self._native.widest_dtype
        return None if name is None else np.dtype(name)

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
                         dtype=None, *, cancel=None) -> LeafChunkPlan:
        """Plan the leaf cells whose centers lie in `region` (lower <= center
        < upper per axis; None is the whole domain), one item per grid that
        has any. `dtype=None` uses the widest stored dtype across levels.
        `cancel` is an optional CancelToken; Ctrl-C raises KeyboardInterrupt
        promptly, as for reads."""
        if isinstance(fields, str):
            fields = [fields]
        native_region = _region(region, self.dimension)
        plan = self._native.plan_leaf_chunks(
            list(fields), native_region, _dtype_name(dtype), cancel)
        return LeafChunkPlan(plan, self.dimension, native_region)

    def read_leaf_chunk(self, item: PlanItem, *, cancel=None) -> LeafChunk:
        """Copy one item's selected cells into arrays the chunk owns.
        `cancel` is an optional CancelToken. Signal handlers run while the
        read is in progress, so Ctrl-C raises KeyboardInterrupt promptly."""
        if not isinstance(item, PlanItem):
            raise TypeError("read_leaf_chunk takes a PlanItem")
        return LeafChunk(self._native.read_leaf_chunk(item._native, cancel))

    def leaf_cells(self, fields: Iterable[str] = (), region: Region = None,
                   dtype=None, *, cancel=None,
                   progress=None) -> Iterator[LeafChunk]:
        """Plan, then read every item serially in canonical order.
        `progress(done, total)` is called after each item is read."""
        plan = self.plan_leaf_chunks(fields, region=region, dtype=dtype,
                                     cancel=cancel)
        for done, item in enumerate(plan, start=1):
            chunk = self.read_leaf_chunk(item, cancel=cancel)
            if progress is not None:
                progress(done, len(plan))
            yield chunk

    def __repr__(self) -> str:
        state = "closed" if self.closed else f"{self.dimension}-D"
        return f"Dataset({self.path if not self.closed else ''!r}, {state})"


def open_plotfile(path, derived=(), cache_bytes: Optional[int] = None, *,
                  cancel=None) -> Dataset:
    """Open a plotfile for analysis.

    derived: derived-field definitions: a {name: expression} mapping, or a
        sequence of entries, each a DerivedField, a (name, expression) pair,
        or a mapping with "name" and "expression". A
        definition that does not resolve raises DerivedFieldError here.
        The viewer's saved definitions are never loaded.
    cache_bytes: the block-cache budget. None uses AMREXPLORER_CACHE_SIZE_MB
        if set (raising if it is not a positive whole number) and 1 GiB
        otherwise.
    cancel: an optional CancelToken that stops reading the metadata. Ctrl-C
        raises KeyboardInterrupt promptly, as for reads.
    """
    return Dataset(path, derived=derived, cache_bytes=cache_bytes,
                   cancel=cancel)
