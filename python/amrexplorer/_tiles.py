"""Tiles: grids grown by ghost cells, for stencils."""

from __future__ import annotations

from typing import Iterator, Mapping, Sequence

import numpy as np

from . import _amrexplorer as _native

_AXES = ("x", "y", "z")
_WIDTHS = ("dx", "dy", "dz")


class TileItem:
    """One tile: a FAB with leaf cells in the region, grown by the ghost
    width. Items match the leaf-chunk plan of the same request one for one."""

    __slots__ = ("_native", "_dimension")

    def __init__(self, native, dimension):
        self._native = native
        self._dimension = dimension

    index = property(lambda self: self._native.index)
    level = property(lambda self: self._native.level)
    grid = property(lambda self: self._native.grid)
    num_cells = property(lambda self: self._native.num_cells,
                         doc="Leaf cells in the tile's valid region.")
    estimated_bytes = property(
        lambda self: self._native.estimated_bytes,
        doc="Planning bound on the block cache one FAB read pins.")

    @property
    def touched(self) -> list:
        """Every (level, grid) the ghost fill reads: its cost."""
        return list(self._native.touched)

    @property
    def grown_box(self):
        lower, upper = self._native._grown_box(self._dimension)
        return tuple(lower), tuple(upper)

    def __repr__(self) -> str:
        return (f"TileItem(index={self.index}, level={self.level}, "
                f"grid={self.grid}, touched={len(self.touched)})")


class TilePlan(Sequence):
    def __init__(self, native, dimension):
        self._native = native
        self._dimension = dimension

    def __len__(self):
        return len(self._native)

    def __getitem__(self, index):
        if isinstance(index, slice):
            return [self[i] for i in range(*index.indices(len(self)))]
        return TileItem(_native._tile_item(self._native, int(index)),
                        self._dimension)

    def __iter__(self) -> Iterator[TileItem]:
        for index in range(len(self)):
            yield self[index]

    fields = property(lambda self: list(self._native.fields))
    dtype = property(lambda self: np.dtype(self._native.dtype))
    ghost = property(lambda self: self._native.ghost)

    def __repr__(self):
        return f"TilePlan(items={len(self)}, ghost={self.ghost}, fields={self.fields})"


class Tile(Mapping):
    """A FAB grown by ghost cells: dense, read-only arrays of shape
    (nx[, ny[, nz]]) in Fortran order, indexed [i, j, k] from the grown
    box's lower corner.

    tile["field"]: values, NaN where `valid` is False.
    valid: False only for ghost cells outside a non-periodic domain.
    source_level: the level each value came from (the tile's own level, or a
        coarser one for interpolated ghost cells), -1 where invalid.
    leaf_mask: the tile's leaf cells in the region -- the cells its
        leaf-chunk counterpart holds; ghosts are never leaves here.
    x, y, z: cell-center coordinates along each axis (1-D, float64).
    interior(array): the part of a tile-shaped array over the valid box.
    """

    def __init__(self, native):
        self._native = native
        self._cache = {}

    def __getitem__(self, name):
        if name not in self._cache:
            if name not in self._native.fields:
                raise KeyError(name)
            self._cache[name] = self._native._field(name)
        return self._cache[name]

    def __iter__(self):
        return iter(self._native.fields)

    def __len__(self):
        return len(self._native.fields)

    def _cached(self, key, compute):
        if key not in self._cache:
            self._cache[key] = compute()
        return self._cache[key]

    @property
    def valid(self) -> np.ndarray:
        return self._cached(" valid", lambda: self._native._valid().astype(bool))

    @property
    def source_level(self) -> np.ndarray:
        return self._cached(" source", self._native._source_level)

    @property
    def leaf_mask(self) -> np.ndarray:
        return self._cached(" leaf", lambda: self._native._leaf_mask().astype(bool))

    index = property(lambda self: self._native.index)
    level = property(lambda self: self._native.level)
    grid = property(lambda self: self._native.grid)
    ghost = property(lambda self: self._native.ghost)
    dimension = property(lambda self: self._native.dimension)
    cell_size = property(lambda self: tuple(self._native.cell_size))

    @property
    def box(self):
        lower, upper = self._native.box
        return tuple(lower), tuple(upper)

    @property
    def grown_box(self):
        lower, upper = self._native.grown_box
        return tuple(lower), tuple(upper)

    @property
    def shape(self):
        lower, upper = self.grown_box
        return tuple(h - l + 1 for l, h in zip(lower, upper))

    def __getattr__(self, name):
        if name in _AXES[: self.dimension]:
            axis = _AXES.index(name)
            lower, upper = self.grown_box
            origin = self._native.index_origin[axis]
            dx = self._native.cell_size[axis]
            return origin + (np.arange(lower[axis], upper[axis] + 1) + 0.5) * dx
        if name in _WIDTHS[: self.dimension]:
            return self._native.cell_size[_WIDTHS.index(name)]
        raise AttributeError(name)

    def interior(self, array: np.ndarray) -> np.ndarray:
        g = self.ghost
        if g == 0:
            return array
        return array[tuple(slice(g, -g) for _ in range(self.dimension))]

    @property
    def cell_volume(self) -> float:
        return float(np.prod(self.cell_size))

    def __repr__(self):
        return (f"Tile(index={self.index}, level={self.level}, grid={self.grid}, "
                f"shape={self.shape}, ghost={self.ghost})")
