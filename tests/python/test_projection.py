"""Exact leaf-cell projections. Validates analysis totals:
conservation against a direct leaf-cell sum with matching (fractional
overlap) region semantics, and pixel values against an independent NumPy
reference."""

import numpy as np
import pytest

import amrexplorer as ax
from plotfile_writer import Level, write_plotfile


def overlap(lo, hi, blo, bhi):
    return np.maximum(0.0, np.minimum(hi, bhi) - np.maximum(lo, blo))


def reference_projection(ds, field, axis, rect, bounds, resolution, weight=None):
    """The same integral by brute force: every leaf cell against every pixel."""
    u, v = {0: (1, 2), 1: (0, 2), 2: (0, 1)}[axis]
    nu, nv = resolution
    du = (rect[2] - rect[0]) / nu
    dv = (rect[3] - rect[1]) / nv
    pu = rect[0] + du * np.arange(nu)
    pv = rect[1] + dv * np.arange(nv)
    total = np.zeros((nv, nu))
    wtotal = np.zeros((nv, nu))
    volume = np.zeros((nv, nu))
    names = [field] + ([weight] if weight and weight != field else [])
    for chunk in ds.leaf_cells(names, dtype=np.float64):
        f = chunk[field]
        w = chunk[weight] if weight else np.ones_like(f)
        lows = [np.asarray(c) - 0.5 * np.asarray(d)
                for c, d in zip(chunk.centers(), chunk.widths())]
        highs = [np.asarray(c) + 0.5 * np.asarray(d)
                 for c, d in zip(chunk.centers(), chunk.widths())]
        oa = overlap(lows[axis], highs[axis], *bounds)
        ou = overlap(lows[u][:, None], highs[u][:, None], pu[None, :], pu[None, :] + du)
        ov = overlap(lows[v][:, None], highs[v][:, None], pv[None, :], pv[None, :] + dv)
        vol = np.einsum("c,cj,ci->cji", oa, ov, ou)
        total += np.einsum("c,cji->ji", f * w, vol)
        wtotal += np.einsum("c,cji->ji", w, vol)
        volume += vol.sum(axis=0)
    area = du * dv
    image = total / wtotal if weight else total / area
    return image, volume / (area * (bounds[1] - bounds[0])), total.sum()


def test_full_domain_is_conservative(refined_3d):
    path, _ = refined_3d
    with ax.open_plotfile(path) as ds:
        direct = sum(float((c["density"] * c.cell_volume).sum())
                     for c in ds.leaf_cells(["density"], dtype=np.float64))
        for axis in "xyz":
            p = ds.project("density", axis)
            assert p.image.shape == (16, 16)
            assert p.image.flags.writeable is False
            assert np.sum(p.image) * p.pixel_area == pytest.approx(direct, rel=1e-13)
            assert p.direct_integral == pytest.approx(direct, rel=1e-13)
            np.testing.assert_allclose(p.coverage, 1.0, rtol=1e-13)
            assert p.labels == {"x": ("y", "z"), "y": ("x", "z"), "z": ("x", "y")}[axis]


def test_partial_slab_matches_brute_force(refined_3d):
    path, _ = refined_3d
    rect = (-0.73, -0.41, 0.66, 0.97)       # cuts cells on every level
    bounds = (-0.29, 0.53)
    resolution = (13, 9)                     # pixels unaligned with cells
    with ax.open_plotfile(path) as ds:
        for axis in (0, 1, 2):
            p = ds.project("density", axis, rect=rect, bounds=bounds,
                           resolution=resolution)
            image, coverage, total = reference_projection(
                ds, "density", axis, rect, bounds, resolution)
            assert p.image.shape == (9, 13)
            np.testing.assert_allclose(p.image, image, rtol=1e-12)
            np.testing.assert_allclose(p.coverage, coverage, rtol=1e-12)
            np.testing.assert_allclose(p.coverage, 1.0, rtol=1e-12)
            assert np.sum(p.pixel_integral) == pytest.approx(total, rel=1e-12)
            assert p.direct_integral == pytest.approx(total, rel=1e-12)


def test_weighted_projection(refined_3d):
    path, _ = refined_3d
    rect = (-1.0, -1.0, 1.0, 1.0)
    with ax.open_plotfile(path) as ds:
        p = ds.project("temperature", "z", weight="density", resolution=(7, 5),
                       rect=rect, bounds=(-0.4, 0.6))
        image, _, _ = reference_projection(ds, "temperature", 2, rect,
                                           (-0.4, 0.6), (7, 5), weight="density")
        np.testing.assert_allclose(p.image, image, rtol=1e-12)
        assert p.weight_image is not None
        with pytest.raises(ax.AnalysisError):
            p.pixel_integral
        same = ds.project("density", "z", weight="density", resolution=(4, 4))
        assert np.all(np.isfinite(same.image))


def test_image_beyond_domain_has_partial_coverage(refined_3d):
    path, _ = refined_3d
    with ax.open_plotfile(path) as ds:
        # Half the image lies outside the domain on u, and the line of
        # sight extends past it too.
        p = ds.project("density", "z", rect=(0.0, -1.0, 2.0, 1.0),
                       bounds=(-1.0, 3.0), resolution=(4, 2))
        np.testing.assert_allclose(p.coverage[:, :2], 0.5, rtol=1e-13)
        np.testing.assert_array_equal(p.coverage[:, 2:], 0.0)
        np.testing.assert_array_equal(p.image[:, 2:], 0.0)


def test_zoom_and_errors(refined_3d, refined_2d):
    path, _ = refined_3d
    with ax.open_plotfile(path) as ds:
        p = ds.project("density", "y", zoom=2.0)
        assert p.rect == (-0.5, -0.5, 0.5, 0.5)
        assert p.extent == (-0.5, 0.5, -0.5, 0.5)
        with pytest.raises(ax.AnalysisError):
            ds.project("density", "w")
        with pytest.raises(ax.AnalysisError):
            ds.project("density", "z", method="sample")
        with pytest.raises(ax.AnalysisError):
            ds.project("density", "z", bounds=(1.0, 0.0))
        with pytest.raises(ax.AnalysisError):
            ds.project("density", "z", resolution=(0, 4))
        with pytest.raises(ax.AnalysisError):
            ds.project("nothing", "z")
    with ax.open_plotfile(refined_2d[0]) as ds:
        with pytest.raises(ax.AnalysisError, match="3-D"):
            ds.project("density", "z")


def test_mixed_precision_values_are_native(tmp_path):
    levels = [Level([((0, 0, 0), (3, 3, 3))], dtype="float32")]
    path = write_plotfile(tmp_path / "plt32p", dim=3, domain_cells=(4, 4, 4),
                          fields={"rho": lambda l, i, j, k: 0.1 + i + 0 * j},
                          levels=levels)
    with ax.open_plotfile(path) as ds:
        p = ds.project("rho", "z")
        stored = (0.1 + np.arange(4)).astype(np.float32).astype(np.float64)
        np.testing.assert_allclose(p.image, np.tile(stored, (4, 1)), rtol=1e-15)
