"""Milestone 6: block metadata and per-block statistics. Validates raw
source values: extrema against the decoded fixture, ghost cells and NaN
included as the design specifies."""

import numpy as np
import pytest

import amrexplorer as ax
from conftest import density, leaf_reference
from plotfile_writer import GHOST_VALUE, Level, write_plotfile


def test_block_listing(refined_3d):
    path, levels = refined_3d
    with ax.open_plotfile(path) as ds:
        blocks = ds.blocks()
        assert [(b["level"], b["grid"]) for b in blocks] == [
            (l, g) for l, level in enumerate(levels) for g in range(len(level.boxes))]
        leaves = leaf_reference(levels, 3)
        for block in blocks:
            lower, upper = levels[block["level"]].boxes[block["grid"]]
            assert block["box"] == (tuple(lower), tuple(upper))
            cells = int(np.prod(np.subtract(upper, lower) + 1))
            assert block["cells"] == cells
            leaf = sum(1 for c in leaves if c[:2] == (block["level"], block["grid"]))
            assert block["covered_cells"] == cells - leaf
            dx = 2.0 / (16 * 2 ** block["level"])
            np.testing.assert_allclose(block["lower"], -1.0 + np.array(lower) * dx)
            np.testing.assert_allclose(block["upper"], -1.0 + (np.array(upper) + 1) * dx)
            assert block["file"].startswith(f"Level_{block['level']}/Cell_D_")
        assert ds.blocks(level=2)[0]["covered_fraction"] == 0.0
        assert 0.0 < ds.blocks(level=0)[0]["covered_fraction"] < 1.0
        with pytest.raises(ax.AnalysisError):
            ds.blocks(level=3)


def test_block_statistics_include_ghosts_and_covered_cells(refined_3d):
    path, levels = refined_3d
    with ax.open_plotfile(path, derived=[("twice", "2*density")]) as ds:
        stats = ds.block_statistics(["density", "twice"], level=0)
        assert len(stats) == 4
        for entry in stats:
            lower, upper = levels[0].boxes[entry["grid"]]
            # Stored ghost cells hold GHOST_VALUE in the fixture, so the
            # stored field's minimum over the on-disk FAB is that value.
            assert entry["minimum"]["density"] == GHOST_VALUE
            assert entry["includes_ghost_cells"]["density"]
            assert entry["box"]["density"] == (
                tuple(np.subtract(lower, 1)), tuple(np.add(upper, 1)))
            # Derived fields cover the valid box only: covered cells count.
            i, j, k = (np.arange(lower[a], upper[a] + 1) for a in range(3))
            values = density(0, i[None, None, :], j[None, :, None], k[:, None, None])
            assert entry["minimum"]["twice"] == 2 * values.min()
            assert entry["maximum"]["twice"] == 2 * values.max()
            assert entry["maximum"]["density"] == values.max()
            assert not entry["includes_ghost_cells"]["twice"]
        one = ds.block_statistics("density", level=1, grid=1)
        assert [(e["level"], e["grid"]) for e in one] == [(1, 1)]
        with pytest.raises(ax.AnalysisError):
            ds.block_statistics(["density"], level=1, grid=2)
        with pytest.raises(ax.AnalysisError):
            ds.block_statistics(["nothing"])


def test_nan_is_ignored_and_all_nan_reports_nan(tmp_path):
    def field(level, i, j, k):
        values = 1.0 + i + 10.0 * j
        values = np.where((i == 1) & (j == 1), np.nan, values)
        return np.where(j >= 4, np.nan, values)

    path = write_plotfile(tmp_path / "pltnan", dim=2, domain_cells=(4, 8),
                          fields={"u": field},
                          levels=[Level([((0, 0), (3, 3)), ((0, 4), (3, 7))],
                                        dtype="float32")])
    with ax.open_plotfile(path) as ds:
        first, second = ds.block_statistics(["u"])
        assert first["minimum"]["u"] == 1.0 and first["maximum"]["u"] == 34.0
        assert np.isnan(second["minimum"]["u"]) and np.isnan(second["maximum"]["u"])
