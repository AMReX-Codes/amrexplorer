import os
import sys

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(__file__))

from plotfile_writer import Level, write_plotfile  # noqa: E402


def density(level, i, j, k):
    # Distinct per level and per cell, exact in float32.
    return 1.0 + i + 64.0 * j + 4096.0 * k + 0.5 * level


def temperature(level, i, j, k):
    return 100.0 * (1 + (i + j + k) % 5) + level


@pytest.fixture
def refined_3d(tmp_path):
    """Three levels: partial refinement, several grids per level, stored ghost
    cells, mixed precision, a version-1 VisMF header on every level."""
    levels = [
        Level([((0, 0, 0), (7, 7, 7)), ((8, 0, 0), (15, 7, 7)),
               ((0, 8, 0), (15, 15, 7)), ((0, 0, 8), (15, 15, 15))],
              dtype="float64", ghost=1),
        Level([((4, 4, 4), (11, 11, 11)), ((12, 4, 4), (19, 11, 11))],
              dtype="float32", ghost=2),
        Level([((10, 10, 10), (17, 17, 17))], dtype="float32", ghost=0),
    ]
    path = write_plotfile(tmp_path / "plt3d", dim=3, domain_cells=(16, 16, 16),
                          fields={"density": density,
                                  "temperature": temperature},
                          levels=levels, prob_lo=(-1.0, -1.0, -1.0),
                          prob_hi=(1.0, 1.0, 1.0), time=2.5)
    return path, levels


@pytest.fixture
def refined_2d(tmp_path):
    levels = [
        Level([((0, 0), (7, 15)), ((8, 0), (15, 15))], ghost=1),
        Level([((4, 4), (11, 11)), ((16, 16), (23, 27))], ghost=1),
    ]
    path = write_plotfile(tmp_path / "plt2d", dim=2, domain_cells=(16, 16),
                          fields={"density": density,
                                  "temperature": temperature},
                          levels=levels, vismf_version=2)
    return path, levels


def leaf_reference(levels, dim, ref_ratio=2):
    """Every leaf cell as (level, i, j, k), computed independently of the
    library by refining coverage masks over each level's domain."""
    cells = []
    for lev, level in enumerate(levels):
        for grid, (lower, upper) in enumerate(level.boxes):
            axes = [np.arange(lower[a], upper[a] + 1) for a in range(dim)]
            while len(axes) < 3:
                axes.append(np.zeros(1, dtype=np.int64))
            k, j, i = np.meshgrid(axes[2], axes[1], axes[0], indexing="ij")
            i, j, k = i.ravel(), j.ravel(), k.ravel()
            leaf = np.ones(i.shape, dtype=bool)
            if lev + 1 < len(levels):
                for flo, fhi in levels[lev + 1].boxes:
                    inside = np.ones(i.shape, dtype=bool)
                    for a, idx in enumerate((i, j, k)[:dim]):
                        inside &= ((idx * ref_ratio <= fhi[a])
                                   & ((idx + 1) * ref_ratio - 1 >= flo[a]))
                    leaf &= ~inside
            for c in zip(i[leaf], j[leaf], k[leaf]):
                cells.append((lev, grid) + tuple(int(v) for v in c))
    return cells
