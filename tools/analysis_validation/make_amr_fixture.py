#!/usr/bin/env python3
"""Write a synthetic 3-D plotfile for comparing ports with Kangaroo and yt:
two levels of gas density and temperature over several grids, the fine grids
straddling coarse grid boundaries."""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..",
                                "tests", "python"))
from plotfile_writer import Level, write_plotfile  # noqa: E402

KPC = 3.0856775814913673e21
HALF_WIDTH = 8 * KPC


def density(level, i, j, k):
    dx = 2 * HALF_WIDTH / (16 * 2**level)
    x, y, z = (-HALF_WIDTH + (n + 0.5) * dx for n in (i, j, k))
    return 1e-24 * np.exp(-np.hypot(x, y) / (3 * KPC) - np.abs(z) / KPC) * (
        1 + 0.2 * np.cos(i - j + 2 * k))


def temperature(level, i, j, k):
    dx = 2 * HALF_WIDTH / (16 * 2**level)
    x, y, z = (-HALF_WIDTH + (n + 0.5) * dx for n in (i, j, k))
    return 10 ** (1.5 + 6 * (np.abs(z) / HALF_WIDTH) + 0.5 * np.sin(x / KPC + y / KPC))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output")
    args = parser.parse_args()
    boxes = [((0, 0, 0), (7, 7, 15)), ((8, 0, 0), (15, 15, 15)),
             ((0, 8, 0), (7, 15, 15))]
    write_plotfile(
        args.output, dim=3, domain_cells=(16, 16, 16),
        fields={"gasDensity": density, "temperature": temperature},
        levels=[Level(boxes),
                # One precision throughout: yt reads a mixed-precision
                # hierarchy's float32 level as NaN.
                Level([((8, 8, 12), (23, 23, 19)), ((4, 20, 14), (11, 27, 17))])],
        prob_lo=(-HALF_WIDTH,) * 3, prob_hi=(HALF_WIDTH,) * 3, time=3.0e15,
        quokka_metadata=True)


if __name__ == "__main__":
    main()
