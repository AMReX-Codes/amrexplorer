#!/usr/bin/env python3
"""Run Kangaroo's stellar CIC projection as scripts/plotfile_projection_cic_stellar.py
does and save the surface density it would draw (Msun pc^-2). Run with
Kangaroo's interpreter, from its checkout.

    kangaroo_cic_array.py PLOTFILE OUT.npz [script options]
"""

import argparse
import sys

import numpy as np

from analysis import Runtime
from analysis.dataset import open_dataset
from analysis.pipeline import pipeline


def main():
    p = argparse.ArgumentParser()
    p.add_argument("plotfile")
    p.add_argument("npz")
    p.add_argument("--axis", choices=("x", "y", "z"), default="z")
    p.add_argument("--axis-bounds")
    p.add_argument("--zoom", type=float, default=1.0)
    p.add_argument("--resolution")
    p.add_argument("--mass-max", type=float)
    p.add_argument("--particle-type", default="StochasticStellarPop_particles")
    a, u = p.parse_known_args()
    rt = Runtime.from_parsed_args(a, unknown_args=u)
    ds = open_dataset(a.plotfile, level=0, runtime=rt)
    view = ds.plane_geometry(axis=a.axis, level=0, zoom=a.zoom, resolution=a.resolution)
    bounds = view["axis_bounds"]
    if a.axis_bounds:
        bounds = tuple(float(v) for v in a.axis_bounds.split(","))
    mass_max = None if a.mass_max is None else a.mass_max * 1.98847e33
    pipe = pipeline(runtime=rt, runmeta=ds.metadata_bundle().runmeta, dataset=ds)
    out = pipe.particle_cic_projection(particle_type=a.particle_type, axis=a.axis,
                                       axis_bounds=bounds, rect=view["rect"],
                                       resolution=view["resolution"],
                                       mass_max=mass_max, out="stellar_projection")
    pipe.run()
    arr = np.asarray(rt.get_task_chunk_array(step=0, level=0, field=out.field,
                                             version=0, block=0, dataset=ds))
    nx, ny = view["resolution"]
    rect = view["rect"]
    pixel_area = abs((rect[2] - rect[0]) / nx) * abs((rect[3] - rect[1]) / ny)
    pc_cm = 3.0856775814913673e18
    arr = arr.reshape(ny, nx) / pixel_area * pc_cm**2 / 1.98847e33
    np.savez(a.npz, surface_density_msun_pc2=arr, rect=np.asarray(rect))
    return 0


if __name__ == "__main__":
    sys.exit(main())
