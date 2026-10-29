#!/usr/bin/env python3
"""Run Kangaroo's slice exactly as scripts/plotfile_slice.py does and save
the array it would draw, for comparison with the amrexplorer port's --npz.
Run with Kangaroo's interpreter, from its checkout.

    kangaroo_slice_array.py PLOTFILE OUT.npz [plotfile_slice.py options]
"""

import argparse
import sys

import numpy as np

import kangaroo as kr


def main():
    p = argparse.ArgumentParser()
    p.add_argument("plotfile")
    p.add_argument("npz")
    p.add_argument("--var")
    p.add_argument("--axis", choices=("x", "y", "z"), default="z")
    p.add_argument("--coord", type=float)
    p.add_argument("--zoom", type=float, default=1.0)
    p.add_argument("--resolution")
    a, u = p.parse_known_args()
    client = kr.Client.from_parsed_args(a, unknown_args=u)
    ds = client.open_dataset(a.plotfile)
    comp = a.var or next(iter(ds.fields))
    view = ds.geometry.plane(axis=a.axis, coord=a.coord, zoom=a.zoom,
                             resolution=a.resolution)
    arr = ds[comp].slice(axis=a.axis, coord=view.coord, rect=view.rect,
                         resolution=view.resolution).compute()
    np.savez(a.npz, slice=np.asarray(arr, dtype=np.float64),
             rect=np.asarray(view.rect), coord=view.coord)
    return 0


if __name__ == "__main__":
    sys.exit(main())
