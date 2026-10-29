#!/usr/bin/env python3
"""Run Kangaroo's projection exactly as scripts/plotfile_projection.py does
and save the surface density it would draw, for comparison with the
amrexplorer port's --npz. Run with Kangaroo's interpreter, from its checkout.

    kangaroo_projection_array.py PLOTFILE OUT.npz [plotfile_projection.py options]
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
    p.add_argument("--axis-bounds")
    p.add_argument("--zoom", type=float, default=1.0)
    p.add_argument("--resolution")
    a, u = p.parse_known_args()
    client = kr.Client.from_parsed_args(a, unknown_args=u)
    ds = client.open_dataset(a.plotfile, level=0)
    comp = a.var or next(iter(ds.fields))
    view = ds.geometry.plane(axis=a.axis, zoom=a.zoom, resolution=a.resolution)
    bounds = view.axis_bounds
    if a.axis_bounds:
        bounds = tuple(float(v) for v in a.axis_bounds.split(","))
    arr = ds[comp].project(axis=a.axis, bounds=bounds, rect=view.rect,
                           resolution=view.resolution,
                           amr_cell_average=True).compute()
    nx, ny = view.resolution
    rect = view.rect
    pixel_area = abs((rect[2] - rect[0]) / nx) * abs((rect[3] - rect[1]) / ny)
    pc_cm = 3.0856775814913673e18
    arr = np.asarray(arr) / pixel_area * (pc_cm**2) / 1.98847e33
    np.savez(a.npz, surface_density_msun_pc2=arr, rect=np.asarray(rect))
    return 0


if __name__ == "__main__":
    sys.exit(main())
