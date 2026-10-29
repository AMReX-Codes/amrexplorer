"""Conservative AMR-aware CIC deposition. Validates analysis
totals: pixel plus outside weight equals the direct sum, with particles next
to block, coarse-fine and domain boundaries, and agreement with a NumPy
reference deposit on a uniform grid."""

import numpy as np
import pytest

import amrexplorer as ax
from plotfile_writer import Level, ParticleSpecies, write_plotfile


def species(positions_by_grid, name="stars", level=0, seed=0):
    rng = np.random.default_rng(seed)
    grids = {}
    next_id = 1
    for grid, positions in positions_by_grid.items():
        n = len(positions)
        grids[grid] = (np.asarray(positions, dtype=float),
                       rng.uniform(1.0, 2.0, (n, 1)), np.zeros((n, 0), dtype=int),
                       np.arange(next_id, next_id + n))
        next_id += n
    return ParticleSpecies(name, ["mass"], [], grids, level=level)


def grid_of(boxes, n, point):
    cell = np.floor((np.asarray(point) + 1.0) / 2.0 * n).astype(int)
    for g, (lo, hi) in enumerate(boxes):
        if np.all(cell >= lo) and np.all(cell <= hi):
            return g
    raise AssertionError(point)


def plotfile(tmp_path, name, levels, particles, n=8):
    return write_plotfile(tmp_path / name, dim=3, domain_cells=(n, n, n),
                          fields={"rho": lambda l, i, j, k: 1.0 + 0.0 * i},
                          levels=levels, prob_lo=(-1.0,) * 3, prob_hi=(1.0,) * 3,
                          particles=particles)


def reference_cic(points, weights, n):
    """2-D CIC on an n x n level covering [-1, 1]^2, folding at the walls."""
    image = np.zeros((n, n))
    dx = 2.0 / n
    for (x, y, _), w in zip(points, weights):
        su, sv = (x + 1.0) / dx - 0.5, (y + 1.0) / dx - 0.5
        iu, iv = int(np.floor(su)), int(np.floor(sv))
        fu, fv = su - iu, sv - iv
        for du, dv, f in ((0, 0, (1 - fu) * (1 - fv)), (1, 0, fu * (1 - fv)),
                          (0, 1, (1 - fu) * fv), (1, 1, fu * fv)):
            image[np.clip(iv + dv, 0, n - 1), np.clip(iu + du, 0, n - 1)] += w * f
    return image


def test_uniform_multiblock_matches_numpy_reference(tmp_path):
    boxes = [((0, 0, 0), (3, 7, 7)), ((4, 0, 0), (7, 3, 7)), ((4, 4, 0), (7, 7, 7))]
    rng = np.random.default_rng(1)
    points = rng.uniform(-1, 1, (300, 3))
    # Next to block boundaries (x = 0, y = 0) and the domain walls.
    points[:6] = [[0.01, 0.3, 0], [-0.01, -0.3, 0.5], [0.5, 0.004, 0],
                  [-0.999, 0.2, 0], [0.999, -0.999, 0.1], [0.1, 0.9999, -0.3]]
    by_grid = {}
    for p in points:
        by_grid.setdefault(grid_of(boxes, 8, p), []).append(p)
    s = species(by_grid)
    path = plotfile(tmp_path, "pltu", [Level(boxes)], [s])
    with ax.open_plotfile(path) as ds:
        d = ds.deposit_particles("stars", "mass", resolution=(8, 8))
        weights = np.concatenate([s.grids[g][1][:, 0] for g in sorted(s.grids)])
        ordered = np.concatenate([np.asarray(by_grid[g]) for g in sorted(by_grid)])
        np.testing.assert_allclose(d.image, reference_cic(ordered, weights, 8),
                                   rtol=1e-13, atol=1e-13)
        assert d.outside_weight == pytest.approx(0.0, abs=1e-12)
        assert d.image.sum() == pytest.approx(weights.sum(), rel=1e-14)
        assert d.selected_particles == 300
        assert d.surface_density.sum() * d.pixel_area == pytest.approx(weights.sum())


def test_multilevel_conservation_and_coarse_fine(tmp_path):
    coarse = [((0, 0, 0), (7, 7, 7))]
    fine = [((4, 4, 4), (11, 11, 11))]
    rng = np.random.default_rng(2)
    # Coarse particles, many next to the fine region's edge at +-0.5; fine
    # particles next to its edge, so their CIC reaches coarse-only cells.
    coarse_points = np.concatenate([rng.uniform(-1, 1, (200, 3)),
                                    [[-0.51, 0.0, 0.0], [0.0, 0.52, 0.1],
                                     [-0.49, -0.49, 0.2]]])
    fine_points = np.concatenate([rng.uniform(-0.5, 0.5, (200, 3)),
                                  [[-0.499, 0.0, 0.0], [0.4999, 0.4999, 0.0]]])
    stars = species({0: coarse_points}, name="coarse_particles")
    young = species({0: fine_points}, name="fine_particles", level=1, seed=3)
    path = plotfile(tmp_path, "pltml", [Level(coarse), Level(fine)], [stars, young])
    with ax.open_plotfile(path) as ds:
        for name, source in (("coarse_particles", stars), ("fine_particles", young)):
            total = source.grids[0][1].sum()
            for resolution, rect in (((16, 16), None), ((13, 7), (-0.7, -0.6, 0.3, 0.9))):
                d = ds.deposit_particles(name, "mass", resolution=resolution, rect=rect)
                assert d.image.sum() + d.outside_weight == pytest.approx(total, rel=1e-13)
                assert d.selected_weight == pytest.approx(total, rel=1e-14)
                assert np.all(d.image >= -1e-15)
        # A coarse particle whose CIC lands wholly in covered cells deposits
        # on the fine cells under them, evenly by area.
        d = ds.deposit_particles("coarse_particles", None, resolution=(16, 16),
                                 mask=lambda c: (np.abs(c.x + 0.49) < 1e-9))
        assert d.selected_particles == 1
        # Point (-0.49, -0.49): CIC over coarse cells 1 and 2 in x and y
        # (0.46 and 0.54 of the weight per axis). Coarse cell 2 lies under
        # the fine grid (coarse 2..5), so its share lands on the four fine
        # cells beneath it -- pixels 4..5 at this resolution -- evenly.
        assert d.image.sum() == pytest.approx(1.0, rel=1e-14)
        np.testing.assert_allclose(d.image[2:4, 2:4].sum(), 0.46 * 0.46, rtol=1e-12)
        np.testing.assert_allclose(d.image[4:6, 4:6], 0.54 * 0.54 / 4, rtol=1e-12)
        nonzero = np.argwhere(d.image > 0)
        assert nonzero.min() == 2 and nonzero.max() == 5


def test_particles_outside_bounds_and_periodic(tmp_path):
    boxes = [((0, 0, 0), (7, 7, 7))]
    points = [[0.999, 0.0, 0.0], [0.0, 0.0, 0.95], [0.3, -0.999, -0.95]]
    s = species({0: points})
    path = plotfile(tmp_path, "pltper", [Level(boxes)], [s])
    with ax.open_plotfile(path) as ds:
        slab = ds.deposit_particles("stars", None, bounds=(-0.5, 0.5),
                                    resolution=(8, 8))
        assert slab.selected_particles == 1 and slab.image.sum() == pytest.approx(1.0)
        folded = ds.deposit_particles("stars", None, resolution=(8, 8))
        wrapped = ds.deposit_particles("stars", None, resolution=(8, 8),
                                       periodic=(True, True, False))
        assert folded.image.sum() == pytest.approx(3.0)
        assert wrapped.image.sum() == pytest.approx(3.0)
        # The x = 0.999 particle: folded keeps all weight in column 7;
        # periodic sends part of it to column 0.
        assert folded.image[:, 0].sum() == 0.0 or folded.image[:, 0].sum() < 1e-12
        assert wrapped.image[:, 0].sum() > 0.4
        half = ds.deposit_particles("stars", None, rect=(0.0, -1.0, 1.0, 1.0),
                                    resolution=(4, 8))
        assert half.image.sum() + half.outside_weight == pytest.approx(3.0)
        assert half.outside_weight == pytest.approx(0.5)  # the x = 0 particle


def test_mask_and_errors(tmp_path):
    s = species({0: [[0.1, 0.1, 0.1], [0.2, 0.2, 0.2]]})
    path = plotfile(tmp_path, "pltm", [Level([((0, 0, 0), (7, 7, 7))])], [s])
    with ax.open_plotfile(path) as ds:
        heavy = ds.deposit_particles("stars", "mass", mask=lambda c: c["mass"] > 1.5)
        expected = sum(m for m in s.grids[0][1][:, 0] if m > 1.5)
        assert heavy.selected_weight == pytest.approx(expected)
        with pytest.raises(ax.AnalysisError, match="mask"):
            ds.deposit_particles("stars", mask=lambda c: np.ones(5, bool))
        with pytest.raises(ax.AnalysisError):
            ds.deposit_particles("stars", method="ngp")
        with pytest.raises(ax.AnalysisError):
            ds.deposit_particles("stars", "nothing")


def test_non_finite_weight_raises(tmp_path):
    s = species({0: [[0.1, 0.1, 0.1]]})
    s.grids[0][1][0, 0] = np.nan
    path = plotfile(tmp_path, "pltnf", [Level([((0, 0, 0), (7, 7, 7))])], [s])
    with ax.open_plotfile(path) as ds:
        with pytest.raises(ax.AnalysisError, match="non-finite"):
            ds.deposit_particles("stars", "mass")


def test_totals_are_compensated(tmp_path):
    # Many particles of widely varying weight: a plain running sum of them
    # drifts from the exactly rounded total; the deposit's must not.
    import math

    rng = np.random.default_rng(9)
    n = 20000
    points = rng.uniform(-1, 1, (n, 3))
    s = species({0: points})
    s.grids[0][1][:, 0] = 10.0 ** rng.uniform(-8, 8, n)
    path = plotfile(tmp_path, "pltsum", [Level([((0, 0, 0), (7, 7, 7))])], [s])
    with ax.open_plotfile(path) as ds:
        d = ds.deposit_particles("stars", "mass", resolution=(8, 8))
        exact = math.fsum(s.grids[0][1][:, 0])
        assert d.selected_weight == pytest.approx(exact, rel=4e-16)
        assert math.fsum(d.image.ravel()) + d.outside_weight == pytest.approx(exact, rel=1e-14)
