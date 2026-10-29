#!/usr/bin/env python3
"""Write a small two-level single-precision plotfile whose field has NaN
cells, and one FAB of only NaN, for comparing per-FAB extrema."""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..",
                                "tests", "python"))
from plotfile_writer import Level, write_plotfile  # noqa: E402


def field(level, i, j, k):
    values = 1.0 + i + 10.0 * j + 100 * k + 0.25 * level
    values = np.where((i == 1) & (j == 1), np.nan, values)
    return np.where((level == 1) & (k >= 20), np.nan, values)


def main():
    write_plotfile(
        sys.argv[1], dim=3, domain_cells=(8, 8, 16),
        fields={"u": field, "w": lambda l, i, j, k: -1.5 * i + 0 * j},
        levels=[Level([((0, 0, 0), (7, 7, 7)), ((0, 0, 8), (7, 7, 15))],
                      dtype="float32"),
                Level([((4, 4, 16), (11, 11, 23)), ((0, 0, 10), (3, 3, 13))],
                      dtype="float32")])


if __name__ == "__main__":
    main()
