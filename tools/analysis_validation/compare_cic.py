#!/usr/bin/env python3
"""Compare the conservative CIC deposit with Kangaroo's per-block deposit.

They must agree exactly (to rounding) in pixels away from every boundary
where Kangaroo drops weight -- at least one level-0 cell from any grid's
image-plane footprint edge, and outside every refined footprint -- and the
conservative deposit must keep all the selected mass, which Kangaroo's does
not. Agreement is to 1e-12 of each pixel's value or 1e-15 of the image's
maximum, whichever is larger. Prints the interior pixel count, the worst interior difference, and the
fraction of mass Kangaroo loses.

    compare_cic.py PLOTFILE OURS.npz KANGAROO.npz [--axis z]
"""

import argparse
import sys

import numpy as np

import amrexplorer


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("plotfile")
    p.add_argument("ours")
    p.add_argument("theirs")
    p.add_argument("--axis", default="z")
    a = p.parse_args()
    ours, theirs = np.load(a.ours), np.load(a.theirs)
    x = ours["surface_density_msun_pc2"]
    y = theirs["surface_density_msun_pc2"]
    if x.shape != y.shape or not np.array_equal(ours["rect"], theirs["rect"]):
        print("shape or rect differ")
        return 1
    u, v = {"x": (1, 2), "y": (0, 2), "z": (0, 1)}[a.axis]
    u0, v0, u1, v1 = ours["rect"]
    ny, nx = x.shape
    pu = u0 + (np.arange(nx) + 0.5) * (u1 - u0) / nx
    pv = v0 + (np.arange(ny) + 0.5) * (v1 - v0) / ny
    PU, PV = np.meshgrid(pu, pv)
    interior = np.ones(x.shape, dtype=bool)
    with amrexplorer.open_plotfile(a.plotfile) as ds:
        dx0 = ds.levels[0]["cell_size"]
        for block in ds.blocks():
            lo, hi = block["lower"], block["upper"]
            inside = ((PU > lo[u]) & (PU < hi[u]) & (PV > lo[v]) & (PV < hi[v]))
            if block["level"] > 0:
                grown = ((PU > lo[u] - dx0[u]) & (PU < hi[u] + dx0[u])
                         & (PV > lo[v] - dx0[v]) & (PV < hi[v] + dx0[v]))
                interior &= ~grown
                continue
            deep = ((PU > lo[u] + dx0[u]) & (PU < hi[u] - dx0[u])
                    & (PV > lo[v] + dx0[v]) & (PV < hi[v] - dx0[v]))
            interior &= ~(inside & ~deep)
    # Relative per pixel, with a floor of 1e-15 of the image's maximum: a
    # particle almost exactly on a cell center leaves a CIC fraction of
    # ~1e-16 that one code rounds to zero and the other keeps, ~1e-17 in an
    # image whose pixels are ~1e-1.
    floor = 1e-3 * np.max(np.abs(y))
    scale = np.maximum(np.maximum(np.abs(x[interior]), np.abs(y[interior])), floor)
    difference = np.abs(x[interior] - y[interior])
    worst = (np.max(np.where(scale > 0, difference / np.where(scale > 0, scale, 1), 0))
             if interior.any() else np.inf)
    selected = float(ours["selected_weight"])
    kept = (x.sum() * (u1 - u0) / nx * (v1 - v0) / ny) / (3.0856775814913673e18**2
                                                         / 1.98847e33)
    lost = 1.0 - y.sum() / x.sum()
    print(f"interior pixels {interior.sum()}/{x.size}, worst interior rel {worst:.3e}, "
          f"conservative deposit keeps {kept / selected + float(ours['outside_weight']) / selected:.15f}"
          f" of the mass, Kangaroo loses {lost:.3%}")
    ok = interior.sum() > 0 and worst <= 1e-12 and abs(
        kept / selected + float(ours["outside_weight"]) / selected - 1) < 1e-12
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
