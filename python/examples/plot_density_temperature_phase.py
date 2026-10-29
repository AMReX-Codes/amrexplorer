#!/usr/bin/env python3
"""Compute a density-temperature phase plot from an AMReX plotfile.

A port of incite-2026's ``postprocessing/plot_density_temperature_phase.py``
from yt to amrexplorer's exact leaf-cell traversal
(docs/analysis-scripting-interface.md), with the same arguments, PNG and
NPZ. Every leaf cell is binned once, by its native density and temperature,
weighted by mass (Msun), volume (cm^3) or count; cgs input is assumed.

    python plot_density_temperature_phase.py PLOTFILE --bins 256 --weight mass

Validates: analysis totals.
"""

from __future__ import annotations

import argparse
import warnings
from pathlib import Path

import numpy as np

import amrexplorer

MSUN_CGS = 1.99e33


def _weights(chunk, density, weight):
    if weight == "mass":
        return density * chunk.cell_volume / MSUN_CGS
    if weight == "volume":
        return chunk.cell_volume
    return np.ones_like(density)


def _positive_finite_mask(rho, temp, weight):
    mask = np.isfinite(rho) & np.isfinite(temp) & np.isfinite(weight)
    mask &= (rho > 0) & (temp > 0) & (weight >= 0)
    return mask


def _finalize_minmax(min_val, max_val, user_min, user_max):
    if user_min is None:
        if min_val is None:
            raise ValueError("No finite values available to set min range")
        user_min = min_val
    if user_max is None:
        if max_val is None:
            raise ValueError("No finite values available to set max range")
        user_max = max_val
    if user_min >= user_max:
        raise ValueError("min must be smaller than max")
    return float(user_min), float(user_max)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("plotfile", help="Path to plotfile")
    parser.add_argument("--bins", type=int, default=256, help="Number of bins per axis")
    parser.add_argument("--density-field", default="gasDensity",
                        help="Field name for gas density (default: gasDensity)")
    parser.add_argument("--temperature-field", default="temperature",
                        help="Field name for temperature (default: temperature)")
    parser.add_argument("--weight", choices=("mass", "volume", "count"),
                        default="mass", help="Weighting for the histogram (default: mass)")
    parser.add_argument("--density-min", type=float, dest="density_min", default=1.0e-30,
                        help="Minimum density for bins (cgs, default: 1e-30)")
    parser.add_argument("--density-max", type=float, dest="density_max", default=1.0e-20,
                        help="Maximum density for bins (cgs, default: 1e-20)")
    parser.add_argument("--temperature-min", type=float, dest="temperature_min",
                        default=10.0, help="Minimum temperature for bins (K, default: 10)")
    parser.add_argument("--temperature-max", type=float, dest="temperature_max",
                        default=1.0e9, help="Maximum temperature for bins (K, default: 1e9)")
    parser.add_argument("--linear", action="store_true",
                        help="Use linear bins (default: log-spaced)")
    parser.add_argument("--cmap", default="magma",
                        help="Matplotlib colormap name (default: magma)")
    parser.add_argument("--output", help=(
        "Output PNG filename (default: <plotfile>_rho_T_phase_<weight>.png)"))
    parser.add_argument("--npz", help=(
        "Output .npz with histogram and bin edges (default: alongside PNG)"))
    parser.add_argument("--progress", action="store_true",
                        help="Report chunk progress on stderr")
    args = parser.parse_args()
    if args.bins <= 0:
        raise ValueError("Number of bins must be positive")

    fields = [args.density_field, args.temperature_field]
    progress = None
    if args.progress:
        import sys

        def progress(done, total):
            print(f"\rchunks {done}/{total}", end="" if done < total else "\n",
                  file=sys.stderr, flush=True)

    with amrexplorer.open_plotfile(args.plotfile) as ds:
        bounds = (args.density_min, args.density_max,
                  args.temperature_min, args.temperature_max)
        density_min = density_max = temperature_min = temperature_max = None
        if any(bound is None for bound in bounds):
            for chunk in ds.leaf_cells(fields, dtype=np.float64, progress=progress):
                density = chunk[args.density_field]
                temperature = chunk[args.temperature_field]
                weight = _weights(chunk, density, args.weight)
                mask = _positive_finite_mask(density, temperature, weight)
                if not np.any(mask):
                    continue
                for values, which in ((density[mask], "density"),
                                      (temperature[mask], "temperature")):
                    lo, hi = float(values.min()), float(values.max())
                    if which == "density":
                        density_min = lo if density_min is None else min(density_min, lo)
                        density_max = hi if density_max is None else max(density_max, hi)
                    else:
                        temperature_min = (lo if temperature_min is None
                                           else min(temperature_min, lo))
                        temperature_max = (hi if temperature_max is None
                                           else max(temperature_max, hi))
        density_min, density_max = _finalize_minmax(
            density_min, density_max, args.density_min, args.density_max)
        temperature_min, temperature_max = _finalize_minmax(
            temperature_min, temperature_max, args.temperature_min,
            args.temperature_max)
        if not args.linear:
            if density_min <= 0 or temperature_min <= 0:
                raise ValueError("Log bins require positive density and temperature")
            density_edges = np.logspace(np.log10(density_min),
                                        np.log10(density_max), args.bins + 1)
            temperature_edges = np.logspace(np.log10(temperature_min),
                                            np.log10(temperature_max), args.bins + 1)
        else:
            density_edges = np.linspace(density_min, density_max, args.bins + 1)
            temperature_edges = np.linspace(temperature_min, temperature_max,
                                            args.bins + 1)

        hist = np.zeros((args.bins, args.bins), dtype=np.float64)
        for chunk in ds.leaf_cells(fields, dtype=np.float64, progress=progress):
            density = chunk[args.density_field]
            temperature = chunk[args.temperature_field]
            weight = _weights(chunk, density, args.weight)
            mask = _positive_finite_mask(density, temperature, weight)
            if not np.any(mask):
                continue
            partial, _, _ = np.histogram2d(density[mask], temperature[mask],
                                           bins=[density_edges, temperature_edges],
                                           weights=weight[mask])
            hist += partial
        time = ds.time

    output_path = (Path(args.output) if args.output
                   else Path(f"{args.plotfile}_rho_T_phase_{args.weight}.png"))
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    if args.weight == "mass":
        weight_label = r"$\log_{10}\, M\, [M_\odot]$"
    elif args.weight == "volume":
        weight_label = r"$\log_{10}\, V\, [\mathrm{cm}^3]$"
    else:
        weight_label = r"$\log_{10}$ count"
    fig, ax = plt.subplots(figsize=(7.5, 6.0))
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        mesh = ax.pcolormesh(density_edges, temperature_edges, np.log10(hist.T),
                             shading="auto", cmap=args.cmap)
    if not args.linear:
        ax.set_xscale("log")
        ax.set_yscale("log")
    ax.set_xlabel(r"Density [g cm$^{-3}$]")
    ax.set_ylabel("Temperature [K]")
    cbar = fig.colorbar(mesh, ax=ax)
    cbar.set_label(weight_label)
    ax.set_title(f"t = {time / 3.15576e13:.3f} Myr")
    fig.tight_layout()
    fig.savefig(output_path, dpi=200)
    plt.close(fig)

    npz_path = Path(args.npz) if args.npz else output_path.with_suffix(".npz")
    np.savez(npz_path, hist=hist, density_edges=density_edges,
             temperature_edges=temperature_edges)


if __name__ == "__main__":
    main()
