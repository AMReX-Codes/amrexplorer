"""Exact projections of leaf cells."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Sequence, Tuple

import numpy as np

from . import _amrexplorer as _native

_AXES = ("x", "y", "z")


def _axis_index(axis) -> int:
    if isinstance(axis, str):
        if axis not in _AXES:
            raise _native.AnalysisError(f"axis must be one of x, y, z, not {axis!r}")
        return _AXES.index(axis)
    axis = int(axis)
    if not 0 <= axis <= 2:
        raise _native.AnalysisError("axis must be 0, 1 or 2")
    return axis


def image_axes(axis) -> Tuple[int, int]:
    """The image axes (u, v) for a line-of-sight axis, in increasing order."""
    return {0: (1, 2), 1: (0, 2), 2: (0, 1)}[_axis_index(axis)]


@dataclass(frozen=True)
class Projection:
    """An exact projection: arrays of shape (height, width), indexed
    image[j, i] with j = 0 the row at the lower image bound (origin="lower"
    for Matplotlib's imshow), in native units.

    image: without a weight, the pixel-averaged column integral -- the
        integral of the field over the pixel's column divided by the pixel
        area, in field units times native length. With a weight, the
        weighted average integral(f w dV) / integral(w dV) per column.
    weight_image: the pixel-averaged column integral of the weight, or None.
    coverage: the fraction of each pixel's column (pixel area times path
        length) that leaf cells fill.
    """

    image: np.ndarray
    weight_image: Optional[np.ndarray]
    coverage: np.ndarray
    axis: int
    rect: Tuple[float, float, float, float]
    axis_bounds: Tuple[float, float]
    pixel_area: float
    path_length: float
    cells: int
    direct_integral: float

    @property
    def extent(self) -> Tuple[float, float, float, float]:
        """(u0, u1, v0, v1), for imshow(extent=...)."""
        u0, v0, u1, v1 = self.rect
        return (u0, u1, v0, v1)

    @property
    def labels(self) -> Tuple[str, str]:
        u, v = image_axes(self.axis)
        return _AXES[u], _AXES[v]

    @property
    def pixel_integral(self) -> np.ndarray:
        """Without a weight: the integral of the field over each pixel's
        column (image times pixel area)."""
        if self.weight_image is not None:
            raise _native.AnalysisError(
                "a weighted projection has no pixel integral of its own")
        return self.image * self.pixel_area


def default_plane(ds, axis, zoom: float = 1.0):
    """The default image: the plane of the whole domain, one pixel per
    level-0 cell, zoomed about its center; and the domain along the line of
    sight. Matches Kangaroo's plane_geometry."""
    axis = _axis_index(axis)
    u, v = image_axes(axis)
    lower = ds.geometry["domain_lower"]
    upper = ds.geometry["domain_upper"]
    level0 = ds.levels[0]
    cells = [hi - lo + 1 for lo, hi in zip(*level0["domain"])]
    rect = (lower[u], lower[v], upper[u], upper[v])
    if zoom <= 0:
        raise _native.AnalysisError("zoom must be positive")
    if zoom != 1.0:
        mid_u, mid_v = 0.5 * (rect[0] + rect[2]), 0.5 * (rect[1] + rect[3])
        half_u = 0.5 * (rect[2] - rect[0]) / zoom
        half_v = 0.5 * (rect[3] - rect[1]) / zoom
        rect = (mid_u - half_u, mid_v - half_v, mid_u + half_u, mid_v + half_v)
    return rect, (cells[u], cells[v]), (lower[axis], upper[axis])


def project(ds, field: str, axis="z", *, bounds: Optional[Sequence[float]] = None,
            rect: Optional[Sequence[float]] = None,
            resolution: Optional[Sequence[int]] = None,
            weight: Optional[str] = None, zoom: float = 1.0,
            method: str = "integrate", cancel=None) -> Projection:
    if method != "integrate":
        raise _native.AnalysisError(
            "the only projection method is 'integrate', the exact leaf-cell "
            "integral; sampled raster views are a different operation")
    if ds.dimension != 3:
        raise _native.AnalysisError("projection needs a 3-D plotfile")
    axis = _axis_index(axis)
    default_rect, default_resolution, default_bounds = default_plane(ds, axis, zoom)
    rect = tuple(float(v) for v in (default_rect if rect is None else rect))
    bounds = tuple(float(v) for v in (default_bounds if bounds is None else bounds))
    resolution = tuple(int(v) for v in
                       (default_resolution if resolution is None else resolution))
    if len(rect) != 4 or len(bounds) != 2 or len(resolution) != 2:
        raise _native.AnalysisError(
            "rect is (u0, v0, u1, v1), bounds (lower, upper), resolution (nu, nv)")
    native = ds._native.project(field, weight, axis, list(rect), list(bounds),
                                list(resolution), cancel)
    return Projection(
        image=native._image(),
        weight_image=native._weight_image() if native.weighted else None,
        coverage=native._coverage(),
        axis=axis, rect=rect, axis_bounds=bounds,
        pixel_area=native.pixel_area, path_length=native.path_length,
        cells=native.cells, direct_integral=native.direct_integral)


@dataclass(frozen=True)
class SampledSlice:
    """A slice sampled at pixel centers through the viewer's query layer.

    values: (height, width) float64, image[j, i] with j = 0 at the lower
        image bound; 0 where `valid` is False.
    valid: whether a cell covers the pixel center.
    source_level: the level each pixel was read from, -1 where invalid.
    u, v: the pixel-center coordinates along the image axes.
    sampling: "nearest" or "piecewise" (the finest covering cell's value)
        or "linear" (interpolated between cell centers of one level).
    """

    values: np.ndarray
    valid: np.ndarray
    source_level: np.ndarray
    axis: int
    coordinate: float
    rect: Tuple[float, float, float, float]
    sampling: str
    u: np.ndarray
    v: np.ndarray
    _native: object = None

    @property
    def extent(self) -> Tuple[float, float, float, float]:
        u0, v0, u1, v1 = self.rect
        return (u0, u1, v0, v1)

    def masked(self) -> np.ma.MaskedArray:
        return np.ma.masked_array(self.values, mask=~self.valid.astype(bool))


@dataclass(frozen=True)
class CellAverageSlice:
    """The exact area-weighted average of the leaf cells a plane passes
    through, over each pixel: (height, width) arrays, j = 0 at the lower
    image bound.

    image: the area-weighted sum over the pixel divided by the pixel area,
        NaN where no cell meets the pixel.
    coverage: the fraction of each pixel's area the plane's cells cover.
    finest_level: the finest level contributing to each pixel, or -1.
    """

    image: np.ndarray
    coverage: np.ndarray
    finest_level: np.ndarray
    axis: int
    coordinate: float
    rect: Tuple[float, float, float, float]
    pixel_area: float

    @property
    def extent(self) -> Tuple[float, float, float, float]:
        u0, v0, u1, v1 = self.rect
        return (u0, u1, v0, v1)


def default_coordinate(ds, axis) -> float:
    """The center of the middle level-0 cell along `axis` (Kangaroo's
    default slice position)."""
    axis = _axis_index(axis)
    level0 = ds.levels[0]
    lo, hi = level0["domain"][0][axis], level0["domain"][1][axis]
    return ds.geometry["domain_lower"][axis] + (((lo + hi) // 2) + 0.5) * \
        level0["cell_size"][axis]


def _plane(ds, axis, coordinate, rect, resolution, zoom):
    default_rect, default_resolution, _ = default_plane(ds, axis, zoom)
    rect = tuple(float(v) for v in (default_rect if rect is None else rect))
    resolution = tuple(int(v) for v in
                       (default_resolution if resolution is None else resolution))
    coordinate = float(default_coordinate(ds, axis) if coordinate is None
                       else coordinate)
    if len(rect) != 4 or len(resolution) != 2:
        raise _native.AnalysisError("rect is (u0, v0, u1, v1), resolution (nu, nv)")
    return rect, resolution, coordinate


def slice_sampled(ds, field, axis="z", coordinate=None, *, rect=None,
                  resolution=None, sampling="piecewise", max_level=None,
                  composition="finest", zoom: float = 1.0,
                  cancel=None) -> SampledSlice:
    if ds.dimension == 2:
        axis = 2
        lower, upper = ds.geometry["domain_lower"], ds.geometry["domain_upper"]
        level0 = ds.levels[0]
        cells = [hi - lo + 1 for lo, hi in zip(*level0["domain"])]
        rect = tuple(float(v) for v in
                     (rect if rect is not None else (lower[0], lower[1], upper[0], upper[1])))
        resolution = tuple(int(v) for v in (resolution or cells))
        coordinate = 0.0
    else:
        axis = _axis_index(axis)
        rect, resolution, coordinate = _plane(ds, axis, coordinate, rect,
                                              resolution, zoom)
    native = ds._native.slice(field, axis, coordinate, list(rect),
                              list(resolution), sampling, max_level, composition,
                              cancel)
    u0, v0, u1, v1 = rect
    nu, nv = resolution
    return SampledSlice(
        values=native._values(), valid=native._valid().astype(bool),
        source_level=native._source_level(), axis=axis, coordinate=coordinate,
        rect=rect, sampling=sampling,
        u=u0 + (np.arange(nu) + 0.5) * (u1 - u0) / nu,
        v=v0 + (np.arange(nv) + 0.5) * (v1 - v0) / nv, _native=native)


def slice_cell_average(ds, field, axis="z", coordinate=None, *, rect=None,
                       resolution=None, zoom: float = 1.0,
                       cancel=None) -> CellAverageSlice:
    if ds.dimension != 3:
        raise _native.AnalysisError("a cell-average slice needs a 3-D plotfile")
    axis = _axis_index(axis)
    rect, resolution, coordinate = _plane(ds, axis, coordinate, rect,
                                          resolution, zoom)
    native = ds._native.slice_cell_average(field, axis, coordinate, list(rect),
                                           list(resolution), cancel)
    return CellAverageSlice(
        image=native._image(), coverage=native._coverage(),
        finest_level=native._finest_level(), axis=axis, coordinate=coordinate,
        rect=rect, pixel_area=native.pixel_area)


@dataclass(frozen=True)
class Deposition:
    """Particles deposited conservatively with AMR-aware CIC and projected
    onto a uniform image, with the numeric projection's pixel conventions
    ((height, width), j = 0 at the lower image bound).

    image: deposited weight per pixel.
    surface_density: image divided by the pixel area, comparable with a
        Projection's image.
    outside_weight: selected weight that fell outside the image rectangle.
    selected_weight: the direct sum over selected particles; image.sum() +
        outside_weight equals it to rounding.
    """

    image: np.ndarray
    outside_weight: float
    selected_weight: float
    selected_particles: int
    axis: int
    rect: Tuple[float, float, float, float]
    axis_bounds: Tuple[float, float]
    pixel_area: float

    @property
    def surface_density(self) -> np.ndarray:
        return self.image / self.pixel_area

    @property
    def extent(self) -> Tuple[float, float, float, float]:
        u0, v0, u1, v1 = self.rect
        return (u0, u1, v0, v1)


def deposit_particles(ds, species, weight=None, axis="z", *, bounds=None,
                      rect=None, resolution=None, method="cic", mask=None,
                      periodic=None, zoom: float = 1.0, cancel=None,
                      progress=None) -> Deposition:
    if method != "cic":
        raise _native.AnalysisError("the only deposition method is 'cic'")
    if ds.dimension != 3:
        raise _native.AnalysisError("particle deposition needs a 3-D plotfile")
    axis = _axis_index(axis)
    default_rect, default_resolution, default_bounds = default_plane(ds, axis, zoom)
    rect = tuple(float(v) for v in (default_rect if rect is None else rect))
    bounds = tuple(float(v) for v in (default_bounds if bounds is None else bounds))
    resolution = tuple(int(v) for v in
                       (default_resolution if resolution is None else resolution))
    periodic = [bool(p) for p in (periodic or [])]
    periodic = (periodic + [False] * 3)[:3]
    accumulator = _native._Deposit(ds._native, axis, list(rect), list(bounds),
                                   list(resolution), periodic)
    fields = [] if weight is None else [weight]
    if mask is not None:
        fields = None  # the mask may read any attribute
    for chunk in ds.particle_chunks(species, fields, cancel=cancel,
                                    progress=progress):
        selected = None
        if mask is not None:
            selected = np.ascontiguousarray(np.asarray(mask(chunk), dtype=bool))
            if selected.shape != (chunk.size,):
                raise _native.AnalysisError(
                    f"mask returned shape {selected.shape} for a chunk of "
                    f"{chunk.size} particles")
        accumulator.add(chunk._native, weight, selected)
    return Deposition(
        image=accumulator._image(), outside_weight=accumulator.outside_weight,
        selected_weight=accumulator.selected_weight,
        selected_particles=accumulator.selected_particles, axis=axis, rect=rect,
        axis_bounds=bounds, pixel_area=accumulator.pixel_area)
