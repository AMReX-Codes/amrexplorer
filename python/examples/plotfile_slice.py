#!/usr/bin/env python3
"""Slice a plotfile with an exact AMR cell average and plot the result.

A port of Kangaroo's ``scripts/plotfile_slice.py`` to amrexplorer
(docs/python-analysis.md). The arguments and image are
Kangaroo's: each pixel is the area-weighted average of the leaf cells the
plane passes through (Dataset.slice_cell_average). ``--npz`` additionally
saves the numbers; ``--sampled`` draws the viewer's point-sampled slice
(Dataset.slice) instead.

Validates: raw source values (through the exact average).
"""

from __future__ import annotations

import argparse

import numpy as np

import amrexplorer

KPC_CM = 3.0856775814913673e21


def _parse_resolution(resolution):
    if resolution is None:
        return None
    nx, ny = (int(v.strip()) for v in resolution.split(","))
    if nx <= 0 or ny <= 0:
        raise ValueError("resolution must be positive")
    return nx, ny


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("plotfile_path")
    p.add_argument("--var")
    p.add_argument("--axis", choices=("x", "y", "z"), default="z")
    p.add_argument("--coord", type=float)
    p.add_argument("--zoom", type=float, default=1.0)
    p.add_argument("--output")
    p.add_argument("--resolution")
    p.add_argument("--no-plot", action="store_true")
    p.add_argument("--npz", help="Also save the slice and its coverage here.")
    p.add_argument("--sampled", action="store_true",
                   help="Sample pixel centers instead of averaging cells.")
    a = p.parse_args(argv)

    with amrexplorer.open_plotfile(a.plotfile_path) as ds:
        comp = a.var or ds.fields[0]
        options = dict(coordinate=a.coord, zoom=a.zoom,
                       resolution=_parse_resolution(a.resolution))
        if a.sampled:
            result = ds.slice(comp, a.axis, **options)
            arr = np.where(result.valid, result.values, np.nan)
            coverage = result.valid.astype(float)
        else:
            result = ds.slice_cell_average(comp, a.axis, **options)
            arr, coverage = result.image, result.coverage
    if a.npz:
        np.savez(a.npz, slice=arr, coverage=coverage, rect=np.asarray(result.rect),
                 coord=result.coordinate)

    if not a.no_plot:
        import matplotlib.pyplot as plt

        labels = {"x": ("y", "z"), "y": ("x", "z"), "z": ("x", "y")}[a.axis]
        plane = "".join(labels)
        rect = result.rect
        fig, axp = plt.subplots(figsize=(6, 5))
        with np.errstate(divide="ignore", invalid="ignore"):
            logged = np.log10(arr)
        im = axp.imshow(np.ma.masked_invalid(logged), origin="lower",
                        cmap="viridis",
                        extent=(rect[0] / KPC_CM, rect[2] / KPC_CM,
                                rect[1] / KPC_CM, rect[3] / KPC_CM))
        axp.set_title(f"AMR cell-average of {comp} ({plane} plane)")
        axp.set_xlabel(f"{labels[0]} [kpc]")
        axp.set_ylabel(f"{labels[1]} [kpc]")
        fig.colorbar(im, ax=axp, label=f"log10({comp})")
        fig.tight_layout()
        fig.savefig(a.output, dpi=150) if a.output else plt.show()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
