"""Headless rendering with the viewer's own pipelines.

Images for publication and frame sequences, downstream of the numeric
analysis rather than part of it: a sampled slice coloured exactly as the
viewer colours it, and a ray-cast volume rendering. Both return an
(height, width, 4) uint8 RGBA array with row 0 at the top of the picture;
save_png writes one.

    from amrexplorer import render
    with amrexplorer.open_plotfile("plt00100") as ds:
        render.save_png(render.slice_image(ds, "gasDensity", "z", log=True), "z.png")
        render.save_png(render.volume_image(ds, "gasDensity", log=True), "v.png")

Palettes are the viewer's built-ins (rainbow, turbo, viridis, plasma,
parula, coolwarm, blackbody) or a path to a .pal file.
"""

from __future__ import annotations

from typing import Optional, Sequence, Tuple

import numpy as np

from . import _amrexplorer as _native


def _finite_range(values: np.ndarray, log: bool) -> Tuple[float, float]:
    values = values[np.isfinite(values)]
    if log:
        values = values[values > 0]
    if values.size == 0:
        raise _native.AnalysisError("no finite values to colour")
    lo, hi = float(values.min()), float(values.max())
    if lo == hi:
        lo, hi = (lo / 2, hi * 2) if log else (lo - 0.5, hi + 0.5)
    return lo, hi


def slice_image(ds, field: str, axis="z", coordinate=None, *, rect=None,
                resolution=None, sampling: str = "piecewise", max_level=None,
                palette: str = "viridis", vmin: Optional[float] = None,
                vmax: Optional[float] = None, log: bool = False, zoom: float = 1.0,
                cancel=None) -> np.ndarray:
    """A sampled slice (Dataset.slice) coloured by the viewer's renderer.
    The range defaults to the valid values' extrema."""
    plane = ds.slice(field, axis, coordinate, rect=rect, resolution=resolution,
                     sampling=sampling, max_level=max_level, zoom=zoom,
                     cancel=cancel)
    lo, hi = vmin, vmax
    if lo is None or hi is None:
        auto = _finite_range(plane.values[plane.valid], log)
        lo = auto[0] if lo is None else lo
        hi = auto[1] if hi is None else hi
    return ds._native.render_plane(plane._native, palette, float(lo), float(hi),
                                   bool(log))


def volume_image(ds, field: str, *, region=None, max_level=None,
                 max_voxels: int = 256**3, azimuth: float = 0.6,
                 elevation: float = 0.4, zoom: float = 1.0,
                 size: Sequence[int] = (512, 512), palette: str = "viridis",
                 vmin: Optional[float] = None, vmax: Optional[float] = None,
                 log: bool = False, low_threshold: float = 0.0,
                 high_threshold: float = 1.0, max_opacity: float = 0.05,
                 samples_per_voxel: int = 2, cancel=None) -> np.ndarray:
    """An orthographic ray-cast rendering of `field` sampled onto a uniform
    grid of at most `max_voxels` over `region` (default: the domain). The
    camera turns by `azimuth` about z, then `elevation` (radians). Opacity
    ramps from 0 at `low_threshold` to `max_opacity` at `high_threshold`, in
    the normalized range; the range defaults to the sampled extrema."""
    if region is None:
        lower = list(ds.geometry["domain_lower"])
        upper = list(ds.geometry["domain_upper"])
    else:
        lower, upper = (list(map(float, side)) for side in region)
    if ds.dimension != 3:
        raise _native.AnalysisError("volume rendering needs a 3-D plotfile")
    value_range = None
    if vmin is not None and vmax is not None:
        value_range = (float(vmin), float(vmax))
    elif vmin is not None or vmax is not None:
        raise _native.AnalysisError("give both vmin and vmax, or neither")
    return ds._native.render_volume(
        field, lower, upper, max_level, int(max_voxels), float(azimuth),
        float(elevation), float(zoom), [int(size[0]), int(size[1])], palette,
        value_range, bool(log), float(low_threshold), float(high_threshold),
        float(max_opacity), int(samples_per_voxel), cancel)


def save_png(rgba: np.ndarray, path) -> None:
    """Write an RGBA array as a PNG (needs matplotlib)."""
    import matplotlib.image

    matplotlib.image.imsave(str(path), rgba)


__all__ = ["save_png", "slice_image", "volume_image"]
