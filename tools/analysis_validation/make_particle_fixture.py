#!/usr/bin/env python3
"""Write a synthetic 3-D plotfile with a star-particle species for comparing
particle ports with Kangaroo: two levels of gas, several grids, discrete and continuous masses
(so the top-k modes are meaningful), birth times and masses, and an
integer evolution stage."""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..",
                                "tests", "python"))
from plotfile_writer import Level, ParticleSpecies, write_plotfile  # noqa: E402

MSUN = 1.98847e33
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
    parser.add_argument("--particles-per-grid", type=int, default=2000)
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()
    rng = np.random.default_rng(args.seed)
    boxes = [((0, 0, 0), (7, 7, 15)), ((8, 0, 0), (15, 15, 15)),
             ((0, 8, 0), (7, 15, 15))]
    dx = 2 * HALF_WIDTH / 16
    grids = {}
    next_id = 1
    for grid, (lo, hi) in enumerate(boxes):
        n = args.particles_per_grid + 37 * grid
        lower = np.array(lo) * dx - HALF_WIDTH
        upper = (np.array(hi) + 1) * dx - HALF_WIDTH
        positions = lower + (upper - lower) * rng.random((n, 3))
        discrete = rng.choice([10.0, 20.0, 50.0, 100.0], size=n) * MSUN
        continuous = 10 ** rng.uniform(0.5, 3.5, size=n) * MSUN
        mass = np.where(rng.random(n) < 0.6, discrete, continuous)
        birth_time = rng.uniform(0, 3e15, size=n)
        birth_mass = mass * rng.uniform(1.0, 1.5, size=n)
        reals = np.column_stack([mass, birth_time, birth_mass])
        ints = rng.choice([0, 1, 2, 4], size=(n, 1))
        ids = np.arange(next_id, next_id + n)
        next_id += n
        grids[grid] = (positions, reals, ints, ids)
    species = ParticleSpecies("StochasticStellarPop_particles",
                              ["mass", "birth_time", "mass_at_birth"],
                              ["evolution_stage"], grids)
    write_plotfile(
        args.output, dim=3, domain_cells=(16, 16, 16),
        fields={"gasDensity": density, "temperature": temperature},
        levels=[Level(boxes),
                # One precision throughout: yt reads a mixed-precision
                # hierarchy's float32 level as NaN.
                Level([((8, 8, 12), (23, 23, 19)), ((4, 20, 14), (11, 27, 17))])], prob_lo=(-HALF_WIDTH,) * 3,
        prob_hi=(HALF_WIDTH,) * 3, time=3.0e15, particles=[species],
        quokka_metadata=True)


if __name__ == "__main__":
    main()
