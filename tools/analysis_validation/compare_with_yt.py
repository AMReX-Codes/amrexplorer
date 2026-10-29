#!/usr/bin/env python3
"""Compare amrexplorer leaf-cell values and phase totals with yt on a real
plotfile, using the same selection (cell centers in a box) and the same
arithmetic.

Validates: raw source values and analysis totals. yt's region selects cells by
center and excludes covered coarse cells, like plan_leaf_chunks; the two cell
sets are matched by (level, cell center) and their native values compared
exactly. yt converts nothing for an AMReX plotfile whose length unit is the
default (1 cm), so its "code" values are the plotfile's numbers.

    python compare_with_yt.py PLOTFILE --half-width 1e17
"""

import argparse
import json
import sys
import time

import numpy as np

import amrexplorer

PHASES = [0.0, 100.0, 8.0e3, 2.0e4, 5.0e5, np.inf]


def amrexplorer_cells(path, fields, lower, upper):
    start = time.perf_counter()
    columns = {name: [] for name in fields}
    level, x, y, z, volume = [], [], [], [], []
    with amrexplorer.open_plotfile(path) as ds:
        plan = ds.plan_leaf_chunks(fields, region=(lower, upper),
                                   dtype=np.float64)
        for chunk in (ds.read_leaf_chunk(item) for item in plan):
            for name in fields:
                columns[name].append(chunk[name])
            level.append(np.full(chunk.size, chunk.level))
            x.append(chunk.x)
            y.append(chunk.y)
            z.append(chunk.z)
            volume.append(chunk.cell_volume)
    result = {name: np.concatenate(v) for name, v in columns.items()}
    result.update(level=np.concatenate(level), x=np.concatenate(x),
                  y=np.concatenate(y), z=np.concatenate(z),
                  volume=np.concatenate(volume))
    return result, time.perf_counter() - start


def yt_cells(path, fields, lower, upper):
    import yt

    yt.set_log_level(40)
    start = time.perf_counter()
    ds = yt.load(path)
    region = ds.box(lower, upper)
    result = {name: region["boxlib", name].d.astype(np.float64)
              for name in fields}
    result["level"] = region["index", "grid_level"].d.astype(np.int64)
    for axis in "xyz":
        result[axis] = region["index", axis].d
    result["volume"] = region["index", "cell_volume"].d
    return result, time.perf_counter() - start


def phase_masses(cells, density, temperature):
    mass = cells[density] * cells["volume"]
    out = []
    for lo, hi in zip(PHASES[:-1], PHASES[1:]):
        selected = (cells[temperature] >= lo) & (cells[temperature] < hi)
        out.append(float(np.sum(mass[selected])))
    return out


def canonical(cells):
    order = np.lexsort((cells["z"], cells["y"], cells["x"], cells["level"]))
    return {key: value[order] for key, value in cells.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("plotfile")
    parser.add_argument("--half-width", type=float, required=True,
                        help="half width of a box about the origin, native units")
    parser.add_argument("--density-field", default="gasDensity")
    parser.add_argument("--temperature-field", default="temperature")
    parser.add_argument("--output", help="write the comparison as JSON")
    args = parser.parse_args()

    fields = [args.density_field, args.temperature_field]
    lower = [-args.half_width] * 3
    upper = [args.half_width] * 3
    ours, our_time = amrexplorer_cells(args.plotfile, fields, lower, upper)
    theirs, yt_time = yt_cells(args.plotfile, fields, lower, upper)

    report = {
        "plotfile": args.plotfile,
        "region": [lower, upper],
        "amrexplorer_cells": int(ours["level"].size),
        "yt_cells": int(theirs["level"].size),
        "amrexplorer_seconds": our_time,
        "yt_seconds": yt_time,
        "cells_per_level_amrexplorer": np.bincount(ours["level"]).tolist(),
        "cells_per_level_yt": np.bincount(theirs["level"]).tolist(),
    }
    same_cells = report["amrexplorer_cells"] == report["yt_cells"]
    if same_cells:
        a, b = canonical(ours), canonical(theirs)
        report["levels_match"] = bool(np.array_equal(a["level"], b["level"]))
        report["max_center_difference"] = float(max(
            np.max(np.abs(a[axis] - b[axis])) for axis in "xyz"))
        report["volume_max_rel_difference"] = float(
            np.max(np.abs(a["volume"] - b["volume"]) / b["volume"]))
        for name in fields:
            report[f"{name}_values_identical"] = bool(
                np.array_equal(a[name], b[name]))
            report[f"{name}_max_rel_difference"] = float(np.max(
                np.abs(a[name] - b[name]) / np.maximum(np.abs(b[name]), 1e-300)))
    ours_phases = phase_masses(ours, *fields)
    yt_phases = phase_masses(theirs, *fields)
    report["phase_mass_amrexplorer"] = ours_phases
    report["phase_mass_yt"] = yt_phases
    report["phase_mass_rel_difference"] = [
        abs(a - b) / max(abs(b), 1e-300) for a, b in zip(ours_phases, yt_phases)]
    report["total_volume_amrexplorer"] = float(ours["volume"].sum())
    report["total_volume_yt"] = float(theirs["volume"].sum())
    text = json.dumps(report, indent=2)
    print(text)
    if args.output:
        with open(args.output, "w") as out:
            out.write(text + "\n")
    return 0 if same_cells else 1


if __name__ == "__main__":
    sys.exit(main())
