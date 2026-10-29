"""Numeric slices. Validates raw source values: the sampled
slice (through the viewer's query layer) and the exact cell-average slice,
against the fixture's analytic values."""

import numpy as np
import pytest

import amrexplorer as ax
from conftest import density


def test_sampled_slice_reads_finest_cell(refined_3d):
    path, _ = refined_3d
    with ax.open_plotfile(path) as ds:
        # One pixel per level-2 cell over level 2's grid, on a level-2 plane.
        dx2 = 2.0 / 64
        lo, hi = -1.0 + 10 * dx2, -1.0 + 18 * dx2
        z = -1.0 + 13.5 * dx2
        s = ds.slice("density", "z", z, rect=(lo, lo, hi, hi), resolution=(8, 8))
        assert s.values.shape == (8, 8) and s.valid.all()
        np.testing.assert_array_equal(s.source_level, 2)
        i = np.arange(10, 18)
        expected = density(2, i[None, :], i[:, None], 13).astype(np.float32)
        np.testing.assert_array_equal(s.values, expected)
        np.testing.assert_allclose(s.u, -1.0 + (i + 0.5) * dx2)
        coarse = ds.slice("density", "z", z, rect=(lo, lo, hi, hi),
                          resolution=(8, 8), max_level=0)
        np.testing.assert_array_equal(coarse.source_level, 0)
        linear = ds.slice("density", "z", z, sampling="linear")
        assert linear.sampling == "linear" and linear.valid.any()
        with pytest.raises(ax.AnalysisError):
            ds.slice("density", "z", sampling="cubic")
        with pytest.raises(ax.AnalysisError):
            ds.slice("nothing", "z")


def test_sampled_slice_outside_domain_is_invalid(refined_3d):
    path, _ = refined_3d
    with ax.open_plotfile(path) as ds:
        s = ds.slice("density", "x", 0.1, rect=(0.0, 0.0, 2.0, 1.0),
                     resolution=(4, 2))
        assert s.valid[:, :2].all() and not s.valid[:, 2:].any()
        np.testing.assert_array_equal(s.source_level[:, 2:], -1)
        assert s.masked().mask[:, 2:].all()


def test_two_dimensional_sampled_slice(refined_2d):
    path, _ = refined_2d
    with ax.open_plotfile(path) as ds:
        s = ds.slice("density")
        assert s.values.shape == (16, 16) and s.valid.all()


def test_cell_average_takes_the_cell_the_plane_is_in(refined_3d):
    path, _ = refined_3d
    with ax.open_plotfile(path) as ds:
        # Level-1 cells are 1/16 wide. A plane in level-1 cell 9, the upper
        # half of level-0 cell 4: the level-1 value must be cell 9's, not
        # the lowest fine cell under the coarse one (8).
        z = -1.0 + 9.7 / 16
        dx1 = 2.0 / 32
        lo, hi = -1.0 + 4 * dx1, -1.0 + 12 * dx1
        c = ds.slice_cell_average("density", "z", z, rect=(lo, lo, hi, hi),
                                  resolution=(4, 4))
        np.testing.assert_array_equal(c.finest_level, 1)
        np.testing.assert_allclose(c.coverage, 1.0, rtol=1e-14)
        i = np.arange(4, 12)
        cells = density(1, i[None, :], i[:, None], 9).astype(np.float32).astype(float)
        expected = cells.reshape(4, 2, 4, 2).mean(axis=(1, 3))
        np.testing.assert_allclose(c.image, expected, rtol=1e-14)


def test_cell_average_mixes_levels_by_area(refined_3d):
    path, _ = refined_3d
    with ax.open_plotfile(path) as ds:
        # One pixel spanning a level-0 cell half covered by level 1.
        z = -1.0 + 4.5 / 8   # inside level-0 cell 4 and level-1 cell 9
        c = ds.slice_cell_average("density", "z", z,
                                  rect=(-1.0 + 1.0 / 8, -1.0 + 2.0 / 8,
                                        -1.0 + 3.0 / 8, -1.0 + 3.0 / 8),
                                  resolution=(1, 1))
        # Level-0 cells 1 and 2 along x, row 2 along y: level 1 covers x
        # from 4/16 (= level-0 cell 2) onward, rows 4..11 at level 1.
        coarse = float(density(0, 1, 2, 4))
        fine = np.mean([np.float32(density(1, a, b, 9))
                        for a in (4, 5) for b in (4, 5)])
        assert c.image[0, 0] == pytest.approx(0.5 * coarse + 0.5 * fine, rel=1e-14)
        assert c.finest_level[0, 0] == 1


def test_cell_average_outside_and_errors(refined_3d, refined_2d):
    path, _ = refined_3d
    with ax.open_plotfile(path) as ds:
        c = ds.slice_cell_average("density", "y", 0.3, rect=(0.5, 0.0, 1.5, 1.0),
                                  resolution=(2, 1))
        assert np.isfinite(c.image[0, 0]) and np.isnan(c.image[0, 1])
        assert c.coverage[0, 1] == 0.0 and c.finest_level[0, 1] == -1
        with pytest.raises(ax.AnalysisError):
            ds.slice_cell_average("density", "z", float("nan"))
    with ax.open_plotfile(refined_2d[0]) as ds:
        with pytest.raises(ax.AnalysisError, match="3-D"):
            ds.slice_cell_average("density", "z")
