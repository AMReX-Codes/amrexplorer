#!/usr/bin/env python3
"""Compute a particle-mass histogram from complete particle attributes.

A port of Kangaroo's ``scripts/plotfile_particle_mass_histogram.py`` to
amrexplorer's particle chunks (docs/analysis-scripting-interface.md). The
arguments, printed report, and CSV/NPZ outputs are Kangaroo's. Bins follow
NumPy's convention -- [lower, upper) with the last bin closed -- and
non-finite masses are skipped, as in Kangaroo's ``particle_histogram1d``.
Masses are the plotfile's native numbers; the Msun labels assume cgs.

Validates: analysis totals.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter
from pathlib import Path

import numpy as np

import amrexplorer

MSUN_G = 1.98847e33


def _parse_range(range_arg: str) -> tuple[float, float]:
    try:
        r0, r1 = (float(v.strip()) for v in range_arg.split(","))
    except Exception as exc:  # noqa: BLE001
        raise ValueError("--range must be provided as min,max") from exc
    if r0 == r1:
        raise ValueError("--range must include two distinct values")
    if r0 > r1:
        raise ValueError("--range must be increasing (min,max)")
    if not np.isfinite(r0) or not np.isfinite(r1):
        raise ValueError("--range must be finite")
    return r0, r1


def _write_csv(path: Path, edges: np.ndarray, counts: np.ndarray) -> None:
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["bin_lo", "bin_hi", "count"])
        for lo, hi, count in zip(edges[:-1], edges[1:], counts):
            writer.writerow([f"{lo:.8e}", f"{hi:.8e}", f"{count:.8e}"])


def mass_extrema(ds, species):
    lo, hi = np.inf, -np.inf
    for chunk in ds.particle_chunks(species, ["mass"]):
        mass = np.asarray(chunk["mass"], dtype=np.float64)
        mass = mass[np.isfinite(mass)]
        if mass.size:
            lo, hi = min(lo, mass.min()), max(hi, mass.max())
    return lo, hi


def mass_histogram(ds, species, edges, density=False):
    counts = np.zeros(len(edges) - 1)
    for chunk in ds.particle_chunks(species, ["mass"]):
        mass = np.asarray(chunk["mass"], dtype=np.float64)
        partial, _ = np.histogram(mass[np.isfinite(mass)], bins=edges)
        counts += partial
    if density:
        total = counts.sum()
        if total > 0.0:
            widths = np.diff(edges)
            counts = np.where(widths > 0.0, counts / (total * widths), counts)
    return counts


def mass_modes(ds, species, k):
    """The k most frequent exact mass values: by count, then by value, both
    descending (Kangaroo's particle_topk_modes ordering)."""
    counter = Counter()
    for chunk in ds.particle_chunks(species, ["mass"]):
        mass = np.asarray(chunk["mass"], dtype=np.float64)
        values, counts = np.unique(mass[np.isfinite(mass)], return_counts=True)
        counter.update(dict(zip(values.tolist(), counts.tolist())))
    modes = sorted(counter.items(), key=lambda item: (-item[1], -item[0]))
    return modes[:k]


def _print_modes(modes):
    print("topk_mass_modes (value_msun,count):")
    for value, count in modes:
        if not np.isfinite(value) or count <= 0:
            continue
        print(f"{value / MSUN_G:.8e},{int(count)}")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Compute a histogram of particle masses from a plotfile."
    )
    p.add_argument("plotfile")
    p.add_argument(
        "--particle-type",
        "--particles",
        dest="particle_type",
        default=None,
        help="Particle species name (defaults to the first available).",
    )
    p.add_argument("--bins", type=int, default=64, help="Number of histogram bins.")
    p.add_argument("--range", dest="hist_range", help="Histogram range as min,max.")
    p.add_argument(
        "--log-range",
        action="store_true",
        help="Interpret --range values as log10(mass).",
    )
    p.add_argument(
        "--density",
        action="store_true",
        help="Normalize counts by total and bin width.",
    )
    p.add_argument(
        "--output",
        help="Optional output path (.npz for NumPy, otherwise CSV).",
    )
    p.add_argument(
        "--plot-output",
        default="particle_mass_histogram.png",
        help="PNG path for the histogram plot (default: particle_mass_histogram.png).",
    )
    p.add_argument(
        "--topk",
        type=int,
        default=5,
        help="Number of top modes (exact mass values) to report.",
    )
    p.add_argument(
        "--no-plot",
        action="store_true",
        help="Skip plotting the histogram.",
    )
    argv = sys.argv[1:] if argv is None else list(argv)
    a = p.parse_args(argv)

    with amrexplorer.open_plotfile(a.plotfile) as ds:
        species_info = {s["name"]: s for s in ds.particle_species}
        particle_types = list(species_info)
        if not particle_types:
            raise RuntimeError("No particle species found in plotfile.")
        particle_type = a.particle_type or particle_types[0]
        if particle_type not in particle_types:
            available = ", ".join(particle_types)
            raise RuntimeError(
                f"particle type '{particle_type}' not found in plotfile; available: {available}"
            )
        info = species_info[particle_type]
        particle_fields = info["real_components"] + info["int_components"]
        if "mass" not in particle_fields:
            available = ", ".join(particle_fields) if particle_fields else "<none>"
            raise RuntimeError(
                f"particle field 'mass' not found for '{particle_type}'; available: {available}"
            )

        topk_specified = any(arg == "--topk" or arg.startswith("--topk=") for arg in argv)
        if topk_specified:
            if a.topk > 0:
                _print_modes(mass_modes(ds, particle_type, a.topk))
            return 0

        if a.hist_range:
            r0, r1 = _parse_range(a.hist_range)
            hist_range = (10.0**r0, 10.0**r1) if a.log_range else (r0, r1)
        else:
            rmin, rmax = mass_extrema(ds, particle_type)
            if not np.isfinite(rmin) or not np.isfinite(rmax):
                raise RuntimeError("Particle mass range contains non-finite values.")
            if rmin == rmax:
                raise RuntimeError("Particle mass range is degenerate; provide --range explicitly.")
            hist_range = (min(rmin, rmax), max(rmin, rmax))

        if hist_range[0] <= 0.0:
            raise RuntimeError("Log-mass binning requires a strictly positive minimum mass.")

        log_edges = np.linspace(
            np.log10(hist_range[0]),
            np.log10(hist_range[1]),
            int(a.bins) + 1,
        )
        edges = np.power(10.0, log_edges)
        edges[0] = min(edges[0], hist_range[0])
        edges[-1] = max(edges[-1], hist_range[1])
        edges[0] = np.nextafter(edges[0], 0.0)
        edges[-1] = np.nextafter(edges[-1], np.inf)

        counts = mass_histogram(ds, particle_type, edges, density=a.density)
        if a.topk > 0:
            _print_modes(mass_modes(ds, particle_type, a.topk))

    log_range = (np.log10(hist_range[0]), np.log10(hist_range[1]))
    print(
        "particle_type="
        f"{particle_type} bins={int(a.bins)} "
        f"log_range=({log_range[0]:.6e}, {log_range[1]:.6e}) "
        f"mass_range=({hist_range[0]:.6e}, {hist_range[1]:.6e})"
    )
    print("bin_lo,bin_hi,count")
    for lo, hi, count in zip(edges[:-1], edges[1:], counts):
        print(f"{lo:.8e},{hi:.8e},{count:.8e}")

    if a.output:
        out_path = Path(a.output)
        if out_path.suffix == ".npz":
            np.savez(out_path, edges=edges, counts=counts)
        else:
            _write_csv(out_path, edges, counts)

    if np.sum(counts) == 0.0:
        print(
            "Warning: histogram counts are all zero; check --range/--log-range "
            "and that particle masses fall within the selected range."
        )

    if not a.no_plot:
        import matplotlib.pyplot as plt

        fig, ax = plt.subplots(figsize=(6, 4))
        widths = np.diff(edges)
        ax.bar(edges[:-1], counts, width=widths, align="edge", edgecolor="black")
        ax.set_xlabel("particle mass [Msun]")
        ax.set_ylabel("count" if not a.density else "density")
        ax.set_title(f"Particle mass histogram ({particle_type})")
        if np.any(edges <= 0.0):
            raise RuntimeError("Log-scaled histogram requires positive mass bin edges.")
        ax.set_xscale("log")
        if np.all(counts > 0.0):
            ax.set_yscale("log")
        else:
            ax.set_yscale("symlog", linthresh=1.0)
        ticks = ax.get_xticks()
        if ticks.size:
            ax.set_xticklabels([f"{t / MSUN_G:g}" for t in ticks])
        fig.tight_layout()
        fig.savefig(a.plot_output, dpi=150)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
