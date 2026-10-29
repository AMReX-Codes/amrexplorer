#!/usr/bin/env python3
"""Gas mass in temperature phases, summed over exact AMR leaf cells.

Validates: analysis totals. Every leaf cell is counted once and no covered
coarse cell is counted; values are the plotfile's native numbers, so the
masses are in the plotfile's native mass unit (density unit times length
unit cubed). For Quokka, which writes cgs, that is grams; --mass-unit converts
the printed totals (default: solar masses, assuming cgs input).

    python temperature_phase_masses.py plt00100 --region -1e21 -1e21 -1e20 1e21 1e21 1e20

Writes the phase totals as JSON (stdout, or --output).
"""

from __future__ import annotations

import argparse
import json
import sys

import numpy as np

import amrexplorer

MSUN_G = 1.98847e33
DEFAULT_PHASES = [
    ("cold", 0.0, 100.0),
    ("cool", 100.0, 8.0e3),
    ("warm", 8.0e3, 2.0e4),
    ("warm-hot", 2.0e4, 5.0e5),
    ("hot", 5.0e5, np.inf),
]


def phase_masses(ds, density, temperature, phases, region=None,
                 dtype=np.float64):
    """Mass per phase [lower, upper) in temperature, native units."""
    totals = np.zeros(len(phases), dtype=np.float64)
    volume = 0.0
    cells = 0
    lowers = np.array([p[1] for p in phases])
    uppers = np.array([p[2] for p in phases])
    for chunk in ds.leaf_cells([density, temperature], region=region,
                               dtype=dtype):
        rho = chunk[density]
        temp = chunk[temperature]
        mass = rho * chunk.cell_volume
        for index in range(len(phases)):
            selected = (temp >= lowers[index]) & (temp < uppers[index])
            totals[index] += mass[selected].sum(dtype=np.float64)
        volume += chunk.cell_volume.sum()
        cells += chunk.size
    return totals, volume, cells


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("plotfile")
    parser.add_argument("--density-field", default="gasDensity")
    parser.add_argument("--temperature-field", default="temperature")
    parser.add_argument("--region", type=float, nargs="+", metavar="BOUND",
                        help="lower bounds then upper bounds, one per axis")
    parser.add_argument("--mass-unit", type=float, default=MSUN_G,
                        help="native mass per reported unit (default: Msun in g)")
    parser.add_argument("--cache-mb", type=int, default=None)
    parser.add_argument("--output", help="write JSON here instead of stdout")
    args = parser.parse_args(argv)

    cache = None if args.cache_mb is None else args.cache_mb * 1024 * 1024
    with amrexplorer.open_plotfile(args.plotfile, cache_bytes=cache) as ds:
        region = None
        if args.region:
            if len(args.region) != 2 * ds.dimension:
                parser.error(f"--region needs {2 * ds.dimension} values")
            region = (args.region[: ds.dimension], args.region[ds.dimension:])
        totals, volume, cells = phase_masses(
            ds, args.density_field, args.temperature_field, DEFAULT_PHASES,
            region=region)
        result = {
            "plotfile": args.plotfile,
            "time": ds.time,
            "region": None if region is None else [list(region[0]),
                                                   list(region[1])],
            "leaf_cells": cells,
            "volume_native": volume,
            "mass_unit_native": args.mass_unit,
            "phases": [
                {"name": name, "temperature_min": lo,
                 "temperature_max": None if np.isinf(hi) else hi,
                 "mass_native": float(total),
                 "mass": float(total / args.mass_unit)}
                for (name, lo, hi), total in zip(DEFAULT_PHASES, totals)
            ],
            "total_mass_native": float(totals.sum()),
        }
    text = json.dumps(result, indent=2)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as out:
            out.write(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
