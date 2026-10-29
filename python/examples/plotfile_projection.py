#!/usr/bin/env python3
"""Project a plotfile field along an axis with exact AMR column integration.

A port of Kangaroo's ``scripts/plotfile_projection.py`` to amrexplorer's
exact leaf-cell projection (docs/analysis-scripting-interface.md). The
arguments and the image are Kangaroo's: the column integral of ``--var``
over each pixel, as a surface density in Msun pc^-2 (the plotfile is assumed
to be cgs), drawn as log10. ``--npz`` additionally saves the numbers.

Validates: analysis totals.
"""

from __future__ import annotations

import argparse

import numpy as np

import amrexplorer

MSUN_G = 1.98847e33
PC_CM = 3.0856775814913673e18


def _parse_bounds(bounds: str) -> tuple[float, float]:
    try:
        b0, b1 = (float(v.strip()) for v in bounds.split(","))
    except Exception as exc:  # noqa: BLE001
        raise ValueError("--axis-bounds must be in min,max format") from exc
    if b0 == b1:
        raise ValueError("--axis-bounds must specify two distinct values")
    return b0, b1


def _parse_resolution(resolution: str | None):
    if resolution is None:
        return None
    nx, ny = (int(v.strip()) for v in resolution.split(","))
    if nx <= 0 or ny <= 0:
        raise ValueError("resolution must be positive")
    return nx, ny


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("plotfile")
    p.add_argument("--var")
    p.add_argument("--level", type=int, default=0,
                   help="Accepted for compatibility; the projection is AMR-exact.")
    p.add_argument("--axis", choices=("x", "y", "z"), default="z")
    p.add_argument("--axis-bounds")
    p.add_argument("--zoom", type=float, default=1.0)
    p.add_argument("--output")
    p.add_argument("--resolution")
    p.add_argument("--npz", help="Also save the surface density and coverage here.")
    a = p.parse_args(argv)

    with amrexplorer.open_plotfile(a.plotfile) as ds:
        comp = a.var or ds.fields[0]
        bounds = _parse_bounds(a.axis_bounds) if a.axis_bounds else None
        projection = ds.project(comp, a.axis, bounds=bounds, zoom=a.zoom,
                                resolution=_parse_resolution(a.resolution))

    # The pixel-averaged column integral of a density is a surface density.
    cgs_surface_density_to_msun_pc2 = (PC_CM**2) / MSUN_G
    arr = projection.image * cgs_surface_density_to_msun_pc2
    if a.npz:
        np.savez(a.npz, surface_density_msun_pc2=arr, coverage=projection.coverage,
                 rect=np.asarray(projection.rect),
                 axis_bounds=np.asarray(projection.axis_bounds))

    import matplotlib.pyplot as plt

    labels = projection.labels
    plane = "".join(labels)
    fig, axp = plt.subplots(figsize=(6, 5))
    kpc = 1.0e3 * PC_CM
    rect = projection.rect
    extent = (rect[0] / kpc, rect[2] / kpc, rect[1] / kpc, rect[3] / kpc)
    with np.errstate(divide="ignore", invalid="ignore"):
        logged = np.log10(arr)
    im = axp.imshow(np.ma.masked_invalid(logged), origin="lower", cmap="viridis",
                    extent=extent)
    axp.set_title(f"AMR projection of {comp} ({plane} plane)")
    axp.set_xlabel(f"{labels[0]} [kpc]")
    axp.set_ylabel(f"{labels[1]} [kpc]")
    fig.colorbar(im, ax=axp, label=f"log10({comp} [Msun pc^-2])")
    fig.tight_layout()
    fig.savefig(a.output, dpi=150) if a.output else plt.show()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
