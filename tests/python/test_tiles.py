"""Tiles with ghost cells. Validates analysis totals (masked
tile reductions equal leaf-chunk totals) and raw values (ghost fill)."""

import numpy as np
import pytest

import amrexplorer as ax
from conftest import density
from plotfile_writer import Level, write_plotfile

SLOPES = (3.0, 2.0, -5.0)


def linear(n0):
    def phi(level, i, j, k):
        dx = 2.0 / (n0 * 2**level)
        x, y, z = (-1.0 + (c + 0.5) * dx for c in (i, j, k))
        return SLOPES[0] * x + SLOPES[1] * y + SLOPES[2] * z
    return phi


@pytest.fixture
def linear_3level(tmp_path):
    # Level 2 sits one level-1 cell from level 1's edge, so ghost width 4 on
    # level 2 reaches past level 1 to level 0: the recursion past l-1.
    levels = [Level([((0, 0, 0), (7, 7, 7)), ((8, 0, 0), (15, 15, 15)),
                     ((0, 8, 0), (7, 15, 15)), ((0, 0, 8), (7, 7, 15))], ghost=1),
              Level([((8, 8, 8), (23, 23, 23))], ghost=1),
              Level([((18, 18, 18), (33, 33, 33))])]
    path = write_plotfile(tmp_path / "pltlin", dim=3, domain_cells=(16, 16, 16),
                          fields={"phi": linear(16), "rho": density},
                          levels=levels, prob_lo=(-1.0,) * 3, prob_hi=(1.0,) * 3)
    return path, levels


def test_tiles_match_leaf_chunks(linear_3level):
    path, _ = linear_3level
    with ax.open_plotfile(path) as ds:
        plan = ds.plan_tiles(["rho"], ghost=2)
        leaf = ds.plan_leaf_chunks(["rho"])
        assert [(t.level, t.grid) for t in plan] == [(i.level, i.grid) for i in leaf]
        total = cells = 0.0
        for tile, item in zip(ds.tiles(["rho"], ghost=2), leaf):
            chunk = ds.read_leaf_chunk(item)
            mask = tile.leaf_mask
            assert mask.sum() == chunk.size
            # Fortran order, so a mask's flat order is the chunk's order.
            np.testing.assert_array_equal(tile["rho"].ravel(order="F")[mask.ravel(order="F")],
                                          chunk["rho"])
            total += float((tile["rho"][mask] * tile.cell_volume).sum())
            cells += mask.sum()
            assert not mask[:2].any() and not mask[-2:].any()
        direct = sum(float((c["rho"] * c.cell_volume).sum()) for c in ds.leaf_cells(["rho"]))
        assert total == pytest.approx(direct, rel=1e-14)
        assert cells == leaf.total_cells


@pytest.mark.parametrize("ghost", [1, 2, 4])
def test_linear_gradients_are_exact_across_levels(linear_3level, ghost):
    path, _ = linear_3level
    with ax.open_plotfile(path) as ds:
        levels_seen = set()
        for tile in ds.tiles(["phi"], ghost=ghost):
            phi = tile["phi"]
            gradient = np.gradient(phi, tile.dx, tile.dy, tile.dz)
            interior = tile.leaf_mask & tile.valid
            # Leaf cells whose stencil stays inside the domain.
            inside = np.ones_like(interior)
            for axis in range(3):
                valid = tile.valid
                lo = np.roll(valid, 1, axis=axis)
                hi = np.roll(valid, -1, axis=axis)
                inside &= lo & hi
            check = interior & inside
            for g, slope in zip(gradient, SLOPES):
                np.testing.assert_allclose(g[check], slope, rtol=0, atol=1e-11)
            # Interpolated ghosts carry the linear field exactly too.
            ok = tile.valid
            x, y, z = np.meshgrid(tile.x, tile.y, tile.z, indexing="ij")
            exact = SLOPES[0] * x + SLOPES[1] * y + SLOPES[2] * z
            np.testing.assert_allclose(phi[ok], exact[ok], rtol=0, atol=1e-12)
            levels_seen.update(np.unique(tile.source_level[ok]).tolist())
            if tile.level == 2 and ghost == 4:
                assert 0 in np.unique(tile.source_level), "recursion past level 1"
        assert {0, 1, 2} <= levels_seen


def test_outside_domain_is_invalid_and_periodic_wraps(tmp_path):
    levels = [Level([((0, 0), (3, 7)), ((4, 0), (7, 7))])]
    path = write_plotfile(tmp_path / "pltp", dim=2, domain_cells=(8, 8),
                          fields={"u": lambda l, i, j, k: i + 10.0 * j},
                          levels=levels)
    with ax.open_plotfile(path) as ds:
        tile = next(ds.tiles(["u"], ghost=1))
        assert tile.shape == (6, 10)
        u = tile["u"]
        assert not tile.valid[0].any() and np.isnan(u[0]).all()      # x < 0
        assert tile.valid[5, 1:-1].all()                            # from grid 1
        np.testing.assert_array_equal(u[5, 1:-1], 4 + 10.0 * np.arange(8))
        assert not tile.valid[:, 0].any()                           # y < 0
        wrapped = next(ds.tiles(["u"], ghost=1, periodic=(True, True)))
        assert wrapped.valid.all()
        w = wrapped["u"]
        np.testing.assert_array_equal(w[0, 1:-1], 7 + 10.0 * np.arange(8))
        np.testing.assert_array_equal(w[1:-1, 0], np.arange(4) + 70.0)
        assert w[0, 0] == 7 + 70.0


def test_interpolation_choices(tmp_path):
    levels = [Level([((0,), (7,))]), Level([((4,), (9,))])]
    quad = lambda l, i, j, k: ((i + 0.5) / 2**l) ** 2
    path = write_plotfile(tmp_path / "plt1q", dim=1, domain_cells=(8,),
                          fields={"q": quad}, levels=levels)
    with ax.open_plotfile(path) as ds:
        def ghosts(interpolation):
            tile = [t for t in ds.tiles(["q"], ghost=1, interpolation=interpolation)
                    if t.level == 1][0]
            return tile["q"][0], tile["q"][-1]   # fine cells 3 and 10
        # Parents: coarse cell 1 (fine 3) and coarse 5 (fine 10).
        c = lambda n: (n + 0.5) ** 2
        lo, hi = ghosts("constant")
        assert (lo, hi) == (c(1), c(5))
        lo, hi = ghosts("linear")
        assert lo == pytest.approx(c(1) + 0.5 * (c(2) - c(0)) * 0.25)
        assert hi == pytest.approx(c(5) - 0.5 * (c(6) - c(4)) * 0.25)
        lo, hi = ghosts("limited")
        assert lo == pytest.approx(c(1) + min(0.5 * (c(2) - c(0)), 2 * (c(1) - c(0)),
                                              2 * (c(2) - c(1))) * 0.25)
        with pytest.raises(ax.AnalysisError):
            ds.plan_tiles(["q"], interpolation="cubic")


def test_tile_planning_errors_and_budget(linear_3level):
    path, _ = linear_3level
    with ax.open_plotfile(path, cache_bytes=20000) as ds:
        # The leaf planner refuses the FAB itself first; either way the
        # message says what to raise.
        with pytest.raises(ax.AnalysisError, match="raise cache_bytes"):
            ds.plan_tiles(["phi"], ghost=1)
    with ax.open_plotfile(path) as ds:
        with pytest.raises(ax.AnalysisError):
            ds.plan_tiles([], ghost=1)
        with pytest.raises(ax.AnalysisError):
            ds.plan_tiles(["phi"], ghost=-1)
        item = ds.plan_tiles(["phi"], ghost=2)[-1]
        assert (2, 0) in item.touched and (1, 0) in item.touched
        assert item.estimated_bytes > 0
        with ax.open_plotfile(path) as other:
            with pytest.raises(ax.AnalysisError, match="different dataset"):
                other.read_tile(item)
        tile = ds.read_tile(item)
        assert tile["phi"].dtype == np.float64
        assert next(ds.tiles(["phi"], dtype="float32"))["phi"].dtype == np.float32
