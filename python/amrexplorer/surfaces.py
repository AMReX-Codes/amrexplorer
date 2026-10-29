"""Per-cell areas of spheres and z-aligned cylinders.

For flux integrals through a surface: the geometry helpers return, for every
cell of a leaf chunk, the area the surface has in it (Kangaroo's
tangent-plane approximation -- see include/amrexplorer/analysis/Surfaces.hpp),
its normal, and for cylinder caps the footprint's first moments. Field
arithmetic and binning stay in Python:

    region = surfaces.sphere_region(ds, center, radius)
    for chunk in ds.leaf_cells(fields, region=region):
        section = surfaces.sphere_area_in_cells(chunk, center, radius)
        vr = (chunk["xmom"] * section["normal_x"] + ...) / chunk["rho"]
        flux += np.sum(chunk["rho"] * vr * section["area"])

A cell can cross the surface while its center lies outside a tight region,
so plan with the padded region these return: the surface's bounding box
grown by half the diagonal of a level-0 cell, which keeps every leaf cell
that can meet it on any level. Exact curved-surface areas, if added, will
have different names.
"""

from __future__ import annotations

from typing import Sequence

import numpy as np

from . import _amrexplorer as _native
from ._dataset import LeafChunk


def _center(center) -> list:
    center = [float(v) for v in center]
    if len(center) != 3 or not all(np.isfinite(center)):
        raise _native.AnalysisError("center must be three finite coordinates")
    return center


def sphere_area_in_cells(chunk: LeafChunk, center: Sequence[float],
                         radius: float) -> dict:
    """Per cell of the chunk: "area" of the sphere in the cell and the outward
    unit normal ("normal_x", "normal_y", "normal_z"), the direction from
    the center to the cell center. Zero where the sphere misses the cell."""
    return dict(_native._sphere_area_in_cells(chunk._native, _center(center),
                                              float(radius)))


def cylinder_area_in_cells(chunk: LeafChunk, center: Sequence[float],
                           radius: float, half_height: float) -> dict:
    """Per cell: the wall's "wall_area" and outward normal ("wall_normal_x",
    "wall_normal_y"), each end cap's area ("upper_cap_area" with normal +z,
    "lower_cap_area" with normal -z), and the cap footprint's first moments
    about the center ("cap_moment_x" = integral of x dA, "cap_moment_y")."""
    return dict(_native._cylinder_area_in_cells(
        chunk._native, _center(center), float(radius), float(half_height)))


def disk_rectangle_moments(radius: float, x0: float, x1: float, y0: float,
                           y1: float):
    """(area, integral x dA, integral y dA) of a disk about the origin
    intersected with [x0, x1] x [y0, y1], exactly."""
    return _native._disk_rectangle_moments(radius, x0, x1, y0, y1)


def region_padding(ds) -> float:
    """Half the diagonal of a level-0 cell."""
    return _native._surface_region_padding(ds.levels[0]["cell_size"])


def sphere_region(ds, center: Sequence[float], radius: float):
    """The region to plan leaf chunks with for a sphere: its bounding box
    grown by region_padding(ds)."""
    center = np.asarray(_center(center))
    reach = float(radius) + region_padding(ds)
    return (center - reach).tolist(), (center + reach).tolist()


def cylinder_region(ds, center: Sequence[float], radius: float,
                    half_height: float):
    """The padded region for a z-aligned cylinder."""
    center = np.asarray(_center(center))
    pad = region_padding(ds)
    reach = np.array([radius + pad, radius + pad, half_height + pad])
    return (center - reach).tolist(), (center + reach).tolist()


__all__ = ["cylinder_area_in_cells", "cylinder_region", "disk_rectangle_moments",
           "region_padding", "sphere_area_in_cells", "sphere_region"]
