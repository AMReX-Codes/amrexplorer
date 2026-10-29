#!/usr/bin/env python3
"""Project stellar particle density with conservative AMR-aware CIC deposition.

A port of Kangaroo's ``scripts/plotfile_projection_cic_stellar.py`` to
amrexplorer's particle deposition (Dataset.deposit_particles;
docs/analysis-scripting-interface.md). Arguments and the image are
Kangaroo's, including --mass-max, which reaches the native deposit as a
per-chunk mask. The deposit differs from Kangaroo's on purpose: Kangaroo
deposits block by block and drops the CIC weight that lands in a neighboring
block or in a covered cell, so it loses mass near block and coarse-fine
boundaries; this deposit keeps every selected particle's weight. The two
agree away from those boundaries. ``--npz`` saves the numbers. cgs assumed.

Validates: analysis totals.
"""

from __future__ import annotations

import argparse
import sys

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


def _parse_resolution(resolution):
    if resolution is None:
        return None
    nx, ny = (int(v.strip()) for v in resolution.split(","))
    if nx <= 0 or ny <= 0:
        raise ValueError("resolution must be positive")
    return nx, ny


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Deposit stellar particles with CIC on the AMR hierarchy, then project.")
    p.add_argument("plotfile")
    p.add_argument("--axis", choices=("x", "y", "z"), default="z")
    p.add_argument("--axis-bounds")
    p.add_argument("--zoom", type=float, default=1.0)
    p.add_argument("--output")
    p.add_argument("--resolution")
    p.add_argument("--mass-max", type=float,
                   help="Only accumulate particles with mass <= this threshold [Msun].")
    p.add_argument("--particle-type", "--particles", dest="particle_type",
                   choices=("CIC_particles", "StochasticStellarPop_particles"),
                   default="StochasticStellarPop_particles",
                   help="Particle species to deposit.")
    p.add_argument("--npz", help="Also save the surface density and weights here.")
    a = p.parse_args(argv)

    mask = None
    if a.mass_max is not None:
        if a.mass_max <= 0.0:
            raise ValueError("--mass-max must be positive when specified")
        limit = a.mass_max * MSUN_G
        mask = lambda chunk: np.asarray(chunk["mass"]) <= limit  # noqa: E731

    with amrexplorer.open_plotfile(a.plotfile) as ds:
        names = [s["name"] for s in ds.particle_species]
        if a.particle_type not in names:
            available = ", ".join(names) if names else "<none>"
            raise RuntimeError(f"particle type '{a.particle_type}' not found in "
                               f"plotfile; available: {available}")
        bounds = _parse_bounds(a.axis_bounds) if a.axis_bounds else None
        deposit = ds.deposit_particles(
            a.particle_type, "mass", a.axis, bounds=bounds, zoom=a.zoom,
            resolution=_parse_resolution(a.resolution), mask=mask)
    print(f"deposited {deposit.selected_particles} particles; "
          f"{deposit.outside_weight / max(deposit.selected_weight, 1e-300):.3e} of "
          "their mass falls outside the image", file=sys.stderr)
    arr = deposit.surface_density * (PC_CM**2) / MSUN_G
    if a.npz:
        np.savez(a.npz, surface_density_msun_pc2=arr, rect=np.asarray(deposit.rect),
                 outside_weight=deposit.outside_weight,
                 selected_weight=deposit.selected_weight)

    import matplotlib.pyplot as plt
    from matplotlib.colors import SymLogNorm

    labels = {"x": ("y", "z"), "y": ("x", "z"), "z": ("x", "y")}[a.axis]
    rect = deposit.rect
    kpc = 1.0e3 * PC_CM
    fig, axp = plt.subplots(figsize=(6, 5))
    vmax = np.nanmax(arr)
    if not np.isfinite(vmax) or vmax <= 0.0:
        vmax = 1.0
    norm = SymLogNorm(linthresh=1.0e-2, vmin=0.0, vmax=vmax)
    im = axp.imshow(np.ma.masked_invalid(arr), origin="lower", cmap="viridis",
                    extent=(rect[0] / kpc, rect[2] / kpc, rect[1] / kpc, rect[3] / kpc),
                    norm=norm)
    axp.set_title(f"CIC stellar projection [{a.particle_type}] ({''.join(labels)} plane)")
    axp.set_xlabel(f"{labels[0]} [kpc]")
    axp.set_ylabel(f"{labels[1]} [kpc]")
    fig.colorbar(im, ax=axp, label=f"{a.particle_type} [Msun pc^-2] (symlog)")
    fig.tight_layout()
    fig.savefig(a.output, dpi=150) if a.output else plt.show()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
