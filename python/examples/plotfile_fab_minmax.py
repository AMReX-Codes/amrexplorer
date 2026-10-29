#!/usr/bin/env python3
"""Print per-component min/max for each FAB in a plotfile.

A port of Kangaroo's ``scripts/plotfile_fab_minmax.py`` to amrexplorer's
block statistics (docs/analysis-scripting-interface.md), printing the same
report. Extrema are computed from the decoded data over each whole on-disk
FAB -- covered cells and stored ghost cells included -- with NaN ignored; a
FAB whose values are all NaN reports nan.

Validates: raw source values.
"""

from __future__ import annotations

import argparse
import os

import amrexplorer


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Print per-component min/max for each FAB.")
    parser.add_argument("plotfile", help="Path to the plotfile directory.")
    parser.add_argument("--level", type=int, default=None, help="Level to inspect (default: all).")
    parser.add_argument("--fab", type=int, default=None, help="FAB index to inspect (default: all).")
    args = parser.parse_args(argv)

    if not os.path.isdir(args.plotfile):
        print(f"Plotfile path does not exist: {args.plotfile}")
        return 1

    with amrexplorer.open_plotfile(args.plotfile) as ds:
        names = ds.stored_fields
        levels = [args.level] if args.level is not None else range(ds.finest_level + 1)
        for level in levels:
            nfabs = ds.levels[level]["num_grids"]
            print(f"Level {level}: {nfabs} FABs")
            for entry in ds.block_statistics(names, level=level, grid=args.fab):
                print(f"  FAB {entry['grid']}:")
                for c, name in enumerate(names):
                    lo = entry["minimum"][name]
                    hi = entry["maximum"][name]
                    print(f"    {c:02d} {name}: min={lo:.6e} max={hi:.6e}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
