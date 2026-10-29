#!/usr/bin/env python3
"""Compute histograms of StochasticStellarPop particle masses from a plotfile.

A port of incite-2026's ``postprocessing/stellar_pop_mass_hist.py`` from yt
to amrexplorer's particle chunks (docs/analysis-scripting-interface.md),
with the same arguments and text/NPZ outputs. Where the yt script loads every
particle at once, this streams one grid at a time in two passes: one for the
mass range and the stages present, one for the histograms.

Outputs bin edges and counts for birth masses and current masses, both in
Msun, split by stellar evolution stage. Masses are native numbers divided by
the yt script's MSUN_CGS, so cgs input is assumed.

    python stellar_pop_mass_hist.py PLOTFILE --bins 50 --min 1e2 --max 1e6

Validates: analysis totals.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

import amrexplorer

MSUN_CGS = 1.99e33
PARTICLE_TYPE = "StochasticStellarPop_particles"
STELLAR_STAGE_LABELS = {
    0: "LowMassStar",
    1: "SNProgenitor",
    2: "SNRemnant",
    3: "LowMassComposite",
    4: "Removed",
}


def _stage_label(stage: int) -> str:
    return STELLAR_STAGE_LABELS.get(stage, f"Stage {stage}")


def _component(info: dict, field: str) -> str:
    """yt names a plotfile component c as particle_c; accept either."""
    names = info["real_components"] + info["int_components"]
    for name in (field, field.removeprefix("particle_")):
        if name in names:
            return name
    raise KeyError(
        f"Field {field!r} not found for {info['name']}. "
        f"Available fields: {', '.join(sorted('particle_' + n for n in names))}")


def _write_text_output(path, edges, stage_labels, birth_counts, current_counts):
    stage_col = np.repeat(stage_labels, edges.size - 1)
    left_edges = np.tile(edges[:-1], stage_labels.size)
    right_edges = np.tile(edges[1:], stage_labels.size)
    header = "stage bin_left_Msun bin_right_Msun birth_count current_count"
    lines = []
    for stage, left, right, birth, current in zip(
            stage_col, left_edges, right_edges, birth_counts.ravel(),
            current_counts.ravel()):
        lines.append(f"{stage} {left:.6e} {right:.6e} {int(birth)} {int(current)}")
    output = "\n".join([f"# {header}", *lines]) + "\n"
    if path is None:
        sys.stdout.write(output)
    else:
        path.write_text(output)


def scan(ds, fields, log_bins):
    """Mass extrema over birth and current masses (positive and finite for
    log bins), per-stage counts, and the particle count."""
    birth, current, stage = fields
    lo, hi = np.inf, -np.inf
    stages = {}
    total = 0
    for chunk in ds.particle_chunks(PARTICLE_TYPE, fields):
        masses = np.concatenate((np.asarray(chunk[birth]) / MSUN_CGS,
                                 np.asarray(chunk[current]) / MSUN_CGS))
        keep = np.isfinite(masses)
        if log_bins:
            keep &= masses > 0
        if np.any(keep):
            lo = min(lo, masses[keep].min())
            hi = max(hi, masses[keep].max())
        values, counts = np.unique(np.asarray(chunk[stage], dtype=int),
                                   return_counts=True)
        for value, count in zip(values.tolist(), counts.tolist()):
            stages[value] = stages.get(value, 0) + count
        total += chunk.size
    return lo, hi, stages, total


def histograms(ds, fields, edges, stage_values):
    birth, current, stage = fields
    birth_counts = np.zeros((stage_values.size, edges.size - 1), dtype=int)
    current_counts = np.zeros_like(birth_counts)
    for chunk in ds.particle_chunks(PARTICLE_TYPE, fields):
        stages = np.asarray(chunk[stage], dtype=int)
        birth_msun = np.asarray(chunk[birth]) / MSUN_CGS
        current_msun = np.asarray(chunk[current]) / MSUN_CGS
        for index, value in enumerate(stage_values):
            mask = stages == value
            birth_counts[index] += np.histogram(birth_msun[mask], bins=edges)[0]
            current_counts[index] += np.histogram(current_msun[mask], bins=edges)[0]
    return birth_counts, current_counts


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Histogram StochasticStellarPop particle birth/current masses")
    parser.add_argument("filename", help="Path to plotfile")
    parser.add_argument("--bins", type=int, default=50, help="Number of bins")
    parser.add_argument("--log", action="store_true", default=True,
                        help="Use logarithmic binning (default)")
    parser.add_argument("--linear", action="store_false", dest="log",
                        help="Use linear binning")
    parser.add_argument("--min", type=float, dest="min_mass",
                        help="Minimum mass (Msun) for bins")
    parser.add_argument("--max", type=float, dest="max_mass",
                        help="Maximum mass (Msun) for bins")
    parser.add_argument("--birth-field", default="particle_mass_at_birth",
                        help="Particle field name for birth mass")
    parser.add_argument("--current-field", default="particle_mass",
                        help="Particle field name for current mass")
    parser.add_argument("--stage-field", default="particle_evolution_stage",
                        help="Particle field name for stellar evolution stage")
    parser.add_argument("--output",
                        help="Output file (.npz for arrays or .txt/.csv for table).")
    parser.add_argument("--png", help=(
        "Write histogram plot to this PNG path "
        "(default: <plotfile>_stellar_pop_mass_hist.png)."))
    parser.add_argument("--print-stage-counts", action="store_true",
                        help="Print counts per stellar evolution stage and exit.")
    args = parser.parse_args()

    with amrexplorer.open_plotfile(args.filename) as ds:
        info = {s["name"]: s for s in ds.particle_species}.get(PARTICLE_TYPE)
        if info is None:
            raise KeyError(f"particle type {PARTICLE_TYPE!r} not found")
        fields = [_component(info, args.birth_field),
                  _component(info, args.current_field),
                  _component(info, args.stage_field)]
        auto_min, auto_max, stage_totals, total = scan(ds, fields, args.log)

        min_mass, max_mass = args.min_mass, args.max_mass
        if min_mass is None or max_mass is None:
            if not np.isfinite(auto_min):
                raise ValueError("No valid particle masses available to set bin range")
            if min_mass is None:
                min_mass = auto_min
            if max_mass is None:
                max_mass = auto_max
            if min_mass <= 0 and args.log:
                raise ValueError("Log bins require positive min mass")
            if min_mass >= max_mass:
                raise ValueError("min mass must be smaller than max mass")
            min_mass, max_mass = float(min_mass), float(max_mass)
        if args.bins <= 0:
            raise ValueError("Number of bins must be positive")
        if args.log:
            edges = np.logspace(np.log10(min_mass), np.log10(max_mass), args.bins + 1)
        else:
            edges = np.linspace(min_mass, max_mass, args.bins + 1)

        stage_values = np.array(sorted(stage_totals), dtype=int)
        stage_labels = np.array([_stage_label(int(s)) for s in stage_values])
        if args.print_stage_counts:
            for stage, label in zip(stage_values, stage_labels):
                print(f"{label} (stage {int(stage)}): {int(stage_totals[stage])}")
            return
        birth_counts, current_counts = histograms(ds, fields, edges, stage_values)

    if args.output:
        output_path = Path(args.output)
        if output_path.suffix == ".npz":
            np.savez(output_path, bin_edges_Msun=edges, stage_labels=stage_labels,
                     stage_values=stage_values, birth_counts=birth_counts,
                     current_counts=current_counts)
        else:
            _write_text_output(output_path, edges, stage_labels, birth_counts,
                               current_counts)
    else:
        _write_text_output(None, edges, stage_labels, birth_counts, current_counts)

    png_path = (Path(args.png) if args.png
                else Path(f"{Path(args.filename).name}_stellar_pop_mass_hist.png"))
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(2, 1, figsize=(7, 8), sharex=True)
    for index, stage in enumerate(stage_values):
        label = _stage_label(int(stage))
        # Histogram counts are already reduced; draw them as steps.
        axes[0].stairs(birth_counts[index], edges, linewidth=1.3, label=label)
        axes[1].stairs(current_counts[index], edges, linewidth=1.3, label=label)
    axes[0].set_ylabel("Birth count")
    axes[1].set_ylabel("Current count")
    axes[1].set_xlabel(r"Particle mass [M$_\odot$]")
    if args.log:
        for ax in axes:
            ax.set_xscale("log")
    for ax in axes:
        ax.set_yscale("log")
    axes[0].legend(frameon=False, ncol=2)
    fig.tight_layout()
    fig.savefig(png_path, dpi=200)
    plt.close(fig)


if __name__ == "__main__":
    main()
