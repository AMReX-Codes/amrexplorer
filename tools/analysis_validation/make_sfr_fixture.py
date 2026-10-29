#!/usr/bin/env python3
"""Write a synthetic Quokka-like 3-D plotfile for comparing the enclosed-SFR
port with Kangaroo: three levels, partial refinement, mixed precision, and a
smooth disk-like SFR rate density in cgs. Stored ghost cells are optional
because Kangaroo's reader does not skip them (--ghost 0 for a comparison)."""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..",
                                "tests", "python"))
from plotfile_writer import Level, write_plotfile  # noqa: E402

KPC = 3.0856775814913673e21
HALF_WIDTH = 8 * KPC


def sfr(level, i, j, k):
    dx = 2 * HALF_WIDTH / (16 * 2**level)
    x, y, z = (-HALF_WIDTH + (n + 0.5) * dx for n in (i, j, k))
    radius = np.hypot(x, y)
    return (1e-30 * np.exp(-radius / (2 * KPC)) * np.exp(-np.abs(z) / (0.5 * KPC))
            * (1 + 0.3 * np.sin(i + 2 * j + 3 * k)))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output")
    parser.add_argument("--ghost", type=int, default=0)
    args = parser.parse_args()
    g = args.ghost
    levels = [
        Level([((0, 0, 0), (7, 7, 15)), ((8, 0, 0), (15, 15, 15)),
               ((0, 8, 0), (7, 15, 15))], ghost=g),
        Level([((8, 8, 12), (23, 23, 19)), ((12, 24, 12), (19, 27, 19))],
              dtype="float32", ghost=g),
        Level([((24, 24, 28), (39, 39, 35))], dtype="float32", ghost=g),
    ]
    write_plotfile(
        args.output, dim=3, domain_cells=(16, 16, 16),
        fields={"gasDensity": lambda l, i, j, k: 1e-24 + 0 * i,
                "particle.StochasticStellarPop.birth_mass_density": sfr},
        levels=levels, prob_lo=(-HALF_WIDTH,) * 3, prob_hi=(HALF_WIDTH,) * 3,
        time=3.0e15)


if __name__ == "__main__":
    main()
