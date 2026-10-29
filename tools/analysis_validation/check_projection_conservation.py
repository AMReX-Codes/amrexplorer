#!/usr/bin/env python3
"""Check that an exact projection conserves the projected quantity on a real
plotfile: the sum over pixels of image * pixel area against a direct sum over
leaf cells of value * |cell intersect slab|, computed independently in NumPy
from leaf chunks with the same fractional-overlap region semantics.

    python check_projection_conservation.py PLOTFILE --half-width 1e17 --field gasDensity
"""

import argparse
import json
import sys
import time

import numpy as np

import amrexplorer


def overlap(lo, hi, blo, bhi):
    return np.maximum(0.0, np.minimum(hi, bhi) - np.maximum(lo, blo))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("plotfile")
    parser.add_argument("--half-width", type=float, required=True)
    parser.add_argument("--field", default="gasDensity")
    parser.add_argument("--resolution", type=int, default=257)
    parser.add_argument("--axis", default="z")
    args = parser.parse_args()
    # Bounds off the cell faces, so boundary cells are genuinely cut.
    lo = -args.half_width * 0.987654321
    hi = args.half_width * 1.0123456789
    report = {"plotfile": args.plotfile, "bounds": [lo, hi]}
    with amrexplorer.open_plotfile(args.plotfile) as ds:
        start = time.perf_counter()
        p = ds.project(args.field, args.axis, rect=(lo, lo, hi, hi),
                       bounds=(lo, hi), resolution=(args.resolution,) * 2)
        report["projection_seconds"] = time.perf_counter() - start
        projected = float(np.sum(p.image) * p.pixel_area)
        dx0 = max(ds.levels[0]["cell_size"])
        grown = ([lo - 0.5 * dx0] * 3, [hi + 0.5 * dx0] * 3)
        direct = 0.0
        cells = 0
        start = time.perf_counter()
        for chunk in ds.leaf_cells([args.field], region=grown, dtype=np.float64):
            fraction = np.ones(chunk.size)
            for c, d in zip(chunk.centers(), chunk.widths()):
                fraction *= overlap(c - 0.5 * d, c + 0.5 * d, lo, hi)
            direct += float(np.sum(chunk[args.field] * fraction))
            cells += int(np.count_nonzero(fraction))
        report["direct_seconds"] = time.perf_counter() - start
    report.update(projected_total=projected, direct_total=direct,
                  native_direct_integral=p.direct_integral,
                  relative_difference=abs(projected - direct) / abs(direct),
                  cells_projected=p.cells, cells_direct=cells,
                  coverage_min=float(p.coverage.min()),
                  coverage_max=float(p.coverage.max()))
    print(json.dumps(report, indent=2))
    ok = report["relative_difference"] < 1e-12 and p.cells == cells
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
