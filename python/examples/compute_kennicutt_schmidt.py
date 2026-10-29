#!/usr/bin/env python3
"""Compute a Kennicutt-Schmidt relation from an AMReX plotfile.

A port of incite-2026's ``postprocessing/compute_kennicutt_schmidt.py`` from
yt to amrexplorer (docs/analysis-scripting-interface.md). It computes the
pixel-by-pixel gas surface density -- a 2-D histogram of leaf-cell centers
weighted by leaf-cell mass, exactly as the yt script bins yt's cells -- and
the SFR surface density from young star particles, then writes the relation
to a CSV with the yt script's columns.

The backend returns native numbers only. This script assumes cgs, as Quokka
writes it, and uses unyt's constants so its output matches the yt script's.

Validates: analysis totals.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Optional, Sequence, Tuple

import numpy as np

import amrexplorer

# unyt's values, so the two scripts' conversions agree to the last bit.
MSUN_G = 1.98841586e33
KPC_CM = 3.085677580962325e21
PC_CM = 3.0856775809623245e18
MYR_S = 3.15576e13

AXES = ("x", "y", "z")


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("plotfile", type=Path, help="Path to AMReX plotfile")
    parser.add_argument("--axis", choices=AXES, default="z",
                        help="Line-of-sight projection axis (default: z)")
    parser.add_argument(
        "--width-kpc", type=float,
        help="Square map width in kpc (default: full domain extent in projection plane)")
    parser.add_argument(
        "--thickness-kpc", type=float,
        help="Projection thickness in kpc (default: full domain extent along LOS)")
    parser.add_argument(
        "--resolution", type=int, default=256,
        help="Number of pixels per axis for map generation (default: 256)")
    parser.add_argument(
        "--sfr-window-myr", type=float, default=10.0,
        help="SFR averaging window in Myr for young stars (default: 10)")
    parser.add_argument(
        "--particle-type", default="StochasticStellarPop_particles",
        help="Particle type to use for SFR surface density "
             "(default: StochasticStellarPop_particles)")
    parser.add_argument("--mass-field", default=None,
                        help="Particle mass field name (default: auto-detect)")
    parser.add_argument(
        "--birth-time-field", default=None,
        help="Particle birth/creation time field name (default: auto-detect)")
    parser.add_argument(
        "--age-field", default=None,
        help="Particle age field name (used only if no birth-time field is available)")
    parser.add_argument("--density-field", default="gasDensity",
                        help="Gas density field (default: gasDensity)")
    parser.add_argument("--output-csv", type=Path, default=Path("ks_relation.csv"),
                        help="Output CSV path (default: ks_relation.csv)")
    parser.add_argument("--output-plot", type=Path, default=Path("ks_relation.png"),
                        help="Output scatter plot path (default: ks_relation.png)")
    parser.add_argument("--no-plot", action="store_true",
                        help="Disable writing a scatter plot")
    return parser.parse_args()


def _projection_plane_indices(los_axis: int) -> Tuple[int, int]:
    if los_axis == 0:
        return 1, 2
    if los_axis == 1:
        return 0, 2
    return 0, 1


def _find_particle_field(names: Sequence[str],
                         candidates: Sequence[Optional[str]]) -> Optional[str]:
    """yt names a plotfile component c as particle_c; accept either."""
    for candidate in candidates:
        if candidate is None:
            continue
        for name in (candidate, candidate.removeprefix("particle_")):
            if name in names:
                return name
    return None


def _region(ds, los_axis, width_cm, thickness_cm):
    lower = np.asarray(ds.geometry["domain_lower"], dtype=float)
    upper = np.asarray(ds.geometry["domain_upper"], dtype=float)
    center = 0.5 * lower + 0.5 * upper
    left = center.copy()
    right = center.copy()
    for i in range(3):
        half = 0.5 * width_cm if i != los_axis else 0.5 * thickness_cm
        left[i] = center[i] - half
        right[i] = center[i] + half
    # The yt script keeps its box strictly inside the domain the same way.
    left = np.maximum(left, lower + 1.0e-12)
    right = np.minimum(right, upper - 1.0e-12)
    return center, left, right


def compute_sigma_gas(ds, density_field, los_axis, center, left, right,
                      width_cm, resolution):
    """Msun / pc^2 per pixel, from leaf cells whose centers lie in the box."""
    i0, i1 = _projection_plane_indices(los_axis)
    half_width_kpc = 0.5 * width_cm / KPC_CM
    edges = np.linspace(-half_width_kpc, half_width_kpc, resolution + 1)
    center_kpc = center / KPC_CM
    mass_map = np.zeros((resolution, resolution), dtype=float)
    for chunk in ds.leaf_cells([density_field], region=(left, right),
                               dtype=np.float64):
        mass_msun = chunk[density_field] * chunk.cell_volume / MSUN_G
        coords = chunk.centers()
        c0_rel = coords[i0] / KPC_CM - center_kpc[i0]
        c1_rel = coords[i1] / KPC_CM - center_kpc[i1]
        mask = ((c0_rel >= -half_width_kpc) & (c0_rel < half_width_kpc)
                & (c1_rel >= -half_width_kpc) & (c1_rel < half_width_kpc))
        if not np.any(mask):
            continue
        hist, _, _ = np.histogram2d(c0_rel[mask], c1_rel[mask],
                                    bins=(edges, edges), weights=mass_msun[mask])
        mass_map += hist
    pixel_area_pc2 = (width_cm / PC_CM / resolution) ** 2
    return mass_map / pixel_area_pc2


def compute_sigma_sfr(ds, species, mass_field, birth_time_field, age_field,
                      los_axis, center, left, right, width_cm, resolution,
                      sfr_window_myr):
    """Msun / yr / kpc^2 per pixel from particles younger than the window."""
    i0, i1 = _projection_plane_indices(los_axis)
    half_width_kpc = 0.5 * width_cm / KPC_CM
    edges = np.linspace(-half_width_kpc, half_width_kpc, resolution + 1)
    center_kpc = center / KPC_CM
    window_yr = sfr_window_myr * 1.0e6
    fields = [mass_field, birth_time_field or age_field]
    rate_map = np.zeros((resolution, resolution), dtype=float)
    for chunk in ds.particle_chunks(species, fields):
        positions = [np.asarray(p, dtype=np.float64) for p in chunk.positions()]
        inside = np.ones(chunk.size, dtype=bool)
        for axis in range(3):
            inside &= (positions[axis] >= left[axis]) & (positions[axis] < right[axis])
        if not np.any(inside):
            continue
        mass = np.asarray(chunk[mass_field], dtype=np.float64)[inside] / MSUN_G
        if birth_time_field is not None:
            age_s = ds.time - np.asarray(chunk[birth_time_field],
                                         dtype=np.float64)[inside]
        else:
            age_s = np.asarray(chunk[age_field], dtype=np.float64)[inside]
        age_myr = age_s / MYR_S
        young = (age_myr >= 0.0) & (age_myr <= sfr_window_myr)
        if not np.any(young):
            continue
        weights = mass[young] / window_yr
        c0_rel = positions[i0][inside][young] / KPC_CM - center_kpc[i0]
        c1_rel = positions[i1][inside][young] / KPC_CM - center_kpc[i1]
        mask = ((c0_rel >= -half_width_kpc) & (c0_rel < half_width_kpc)
                & (c1_rel >= -half_width_kpc) & (c1_rel < half_width_kpc))
        if not np.any(mask):
            continue
        hist, _, _ = np.histogram2d(c0_rel[mask], c1_rel[mask],
                                    bins=(edges, edges), weights=weights[mask])
        rate_map += hist
    pixel_area_kpc2 = (width_cm / KPC_CM / resolution) ** 2
    return rate_map / pixel_area_kpc2


def _write_csv(path: Path, sigma_gas: np.ndarray, sigma_sfr: np.ndarray) -> int:
    valid = (sigma_gas > 0.0) & (sigma_sfr > 0.0)
    data = np.column_stack([
        sigma_gas[valid],
        sigma_sfr[valid],
        np.log10(sigma_gas[valid]),
        np.log10(sigma_sfr[valid]),
    ])
    header = ("sigma_gas_msun_pc2,sigma_sfr_msun_yr_kpc2,"
              "log10_sigma_gas,log10_sigma_sfr")
    np.savetxt(path, data, delimiter=",", header=header, comments="")
    return int(np.count_nonzero(valid))


def _fit_power_law(sigma_gas, sigma_sfr) -> Tuple[float, float]:
    valid = (sigma_gas > 0.0) & (sigma_sfr > 0.0)
    x = np.log10(sigma_gas[valid])
    y = np.log10(sigma_sfr[valid])
    if x.size < 2:
        return np.nan, np.nan
    slope, intercept = np.polyfit(x, y, 1)
    return float(slope), float(intercept)


def _plot_relation(path, sigma_gas, sigma_sfr, slope, intercept) -> None:
    import matplotlib.pyplot as plt

    valid = (sigma_gas > 0.0) & (sigma_sfr > 0.0)
    x = sigma_gas[valid]
    y = sigma_sfr[valid]
    fig, ax = plt.subplots(figsize=(6.5, 5.5), constrained_layout=True)
    ax.scatter(x, y, s=8, alpha=0.35, edgecolors="none")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel(r"$\Sigma_{\rm gas}\ [{\rm M_\odot\ pc^{-2}}]$")
    ax.set_ylabel(r"$\Sigma_{\rm SFR}\ [{\rm M_\odot\ yr^{-1}\ kpc^{-2}}]$")
    ax.grid(alpha=0.25)
    if np.isfinite(slope) and np.isfinite(intercept):
        xfit = np.geomspace(np.min(x), np.max(x), 100)
        yfit = 10.0 ** (intercept + slope * np.log10(xfit))
        ax.plot(xfit, yfit, lw=2.0, c="black", label=f"fit slope = {slope:.2f}")
        ax.legend(loc="best")
    fig.savefig(path, dpi=180)
    plt.close(fig)


def main() -> None:
    args = _parse_args()
    if args.resolution <= 0:
        raise ValueError("--resolution must be positive")
    if args.sfr_window_myr <= 0:
        raise ValueError("--sfr-window-myr must be positive")

    with amrexplorer.open_plotfile(args.plotfile) as ds:
        species = {s["name"]: s for s in ds.particle_species}
        target = "StochasticStellarPop_particles"
        if args.particle_type != target:
            raise RuntimeError(
                "This script only computes SFR surface density from "
                "'StochasticStellarPop_particles'.")
        if target not in species:
            available = ", ".join(species) if species else "<none>"
            raise RuntimeError(
                "'StochasticStellarPop_particles' is not present in this dataset. "
                f"Available particle types: {available}")
        names = species[target]["real_components"]
        mass_field = _find_particle_field(
            names, [args.mass_field, "particle_mass_at_birth", "birth_mass",
                    "particle_mass"])
        if mass_field is None:
            raise RuntimeError(
                f"Could not find a particle mass field for type '{target}'. "
                "Try --mass-field.")
        birth_time_field = _find_particle_field(
            names, [args.birth_time_field, "particle_birth_time",
                    "particle_creation_time"])
        age_field = _find_particle_field(names, [args.age_field, "particle_age"])
        if birth_time_field is None and age_field is None:
            raise RuntimeError(
                f"Could not find birth/creation time or age field for type "
                f"'{target}'. Try --birth-time-field or --age-field.")

        los_axis = AXES.index(args.axis)
        i0, i1 = _projection_plane_indices(los_axis)
        domain = (np.asarray(ds.geometry["domain_upper"])
                  - np.asarray(ds.geometry["domain_lower"]))
        width_cm = (min(domain[i0], domain[i1]) if args.width_kpc is None
                    else args.width_kpc * KPC_CM)
        thickness_cm = (domain[los_axis] if args.thickness_kpc is None
                        else args.thickness_kpc * KPC_CM)
        center, left, right = _region(ds, los_axis, width_cm, thickness_cm)

        sigma_gas = compute_sigma_gas(ds, args.density_field, los_axis, center,
                                      left, right, width_cm, args.resolution)
        sigma_sfr = compute_sigma_sfr(ds, target, mass_field, birth_time_field,
                                      age_field, los_axis, center, left, right,
                                      width_cm, args.resolution,
                                      args.sfr_window_myr)

    n_valid = _write_csv(args.output_csv, sigma_gas, sigma_sfr)
    slope, intercept = _fit_power_law(sigma_gas, sigma_sfr)
    if not args.no_plot:
        _plot_relation(args.output_plot, sigma_gas, sigma_sfr, slope, intercept)

    print(f"Loaded plotfile: {args.plotfile}")
    print(f"Gas field: {args.density_field}")
    print(f"Particle type: {target}")
    print(f"Particle mass field: {mass_field}")
    if birth_time_field is not None:
        print(f"Particle birth-time field: {birth_time_field}")
    if age_field is not None:
        print(f"Particle age field: {age_field}")
    print(f"Projection axis: {args.axis}")
    print(f"Map width: {width_cm / KPC_CM:.4g} kpc")
    print(f"Map thickness: {thickness_cm / KPC_CM:.4g} kpc")
    print(f"Resolution: {args.resolution}x{args.resolution}")
    print(f"SFR window: {args.sfr_window_myr:.3g} Myr")
    print(f"Valid KS pixels (Sigma_gas>0 and Sigma_sfr>0): {n_valid}")
    if np.isfinite(slope) and np.isfinite(intercept):
        print(f"Best-fit log10 Sigma_SFR = {intercept:.4f} + {slope:.4f} log10 Sigma_gas")
    else:
        print("Best-fit slope unavailable (fewer than 2 valid pixels)")
    print(f"Wrote CSV: {args.output_csv}")
    if not args.no_plot:
        print(f"Wrote plot: {args.output_plot}")


if __name__ == "__main__":
    main()
