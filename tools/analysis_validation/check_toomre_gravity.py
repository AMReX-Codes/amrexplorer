#!/usr/bin/env python3
"""Check a Toomre-Q CSV's radial gravity against the exact answer for the
MHD fixture's harmonic potential (make_mhd_fixture.py): the mass-weighted
mean of Omega^2 R over each annulus, with the same cell selection and
weights the moments use. Prints the relative error per bin; exits nonzero
if any bin of a cell-uniform region is off by more than --rtol."""

import argparse
import csv
import sys

import numpy as np

import amrexplorer  # noqa: E402 -- PYTHONPATH must name the build

KPC = 3.0856775814913673e21
OMEGA = 2e7 / (5 * KPC)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("plotfile")
    p.add_argument("csv")
    p.add_argument("--r-min-kpc", type=float, default=0.5)
    p.add_argument("--r-max-kpc", type=float, default=7.0)
    p.add_argument("--bins", type=int, default=13)
    p.add_argument("--z-max-kpc", type=float, default=4.0)
    p.add_argument("--rtol", type=float, default=None,
                   help="require every bin within this relative error")
    a = p.parse_args()
    edges = np.linspace(a.r_min_kpc * KPC, a.r_max_kpc * KPC, a.bins + 1)
    zmax = a.z_max_kpc * KPC
    num = np.zeros(a.bins)
    mass = np.zeros(a.bins)
    with amrexplorer.open_plotfile(a.plotfile) as ds:
        for c in ds.leaf_cells(["gasDensity"], dtype=np.float64):
            R = np.hypot(c.x, c.y)
            overlap = np.maximum(0, np.minimum(c.z + 0.5 * c.dz, zmax)
                                 - np.maximum(c.z - 0.5 * c.dz, -zmax))
            keep = (R >= edges[0]) & (R <= edges[-1]) & (overlap > 0)
            bins = np.minimum(np.searchsorted(edges, R[keep], side="right") - 1,
                              a.bins - 1)
            m = c["gasDensity"][keep] * c.dx[keep] * c.dy[keep] * overlap[keep]
            np.add.at(num, bins, m * OMEGA**2 * R[keep])
            np.add.at(mass, bins, m)
    populated = mass > 0
    exact = np.where(populated, num / np.where(populated, mass, 1.0), np.nan)
    got = np.array([float(r["radial_gravity_cm_s2"]) for r in csv.DictReader(open(a.csv))])
    error = got / exact - 1
    print("relative error per bin:", " ".join(f"{e:+.2e}" for e in error))
    if np.isfinite(got[~populated]).any():
        return 1
    if a.rtol is not None and np.max(np.abs(error[populated])) > a.rtol:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
