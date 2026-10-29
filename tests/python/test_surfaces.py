"""Surface-intersection geometry. Validates analysis totals:
summed areas converge to the analytic surface areas on uniform and
multilevel grids, disk moments are exact, and faces are counted once."""

import math

import numpy as np
import pytest

import amrexplorer as ax
from amrexplorer import surfaces
from plotfile_writer import Level, write_plotfile


def uniform(tmp_path, n, levels=None):
    return write_plotfile(
        tmp_path / f"plt_u{n}_{0 if levels is None else len(levels)}", dim=3,
        domain_cells=(n, n, n),
        fields={"rho": lambda l, i, j, k: 1.0 + 0.0 * i},
        levels=levels or [Level([((0, 0, 0), (n - 1, n - 1, n - 1))])],
        prob_lo=(-1.0, -1.0, -1.0), prob_hi=(1.0, 1.0, 1.0))


def total_sphere_area(ds, center, radius):
    total = 0.0
    for chunk in ds.leaf_cells([], region=surfaces.sphere_region(ds, center, radius)):
        total += surfaces.sphere_area_in_cells(chunk, center, radius)["area"].sum()
    return total


def test_sphere_area_converges(tmp_path):
    radius = 0.61
    center = (0.013, -0.021, 0.007)
    exact = 4 * math.pi * radius**2
    errors = []
    for n in (16, 32, 64):
        with ax.open_plotfile(uniform(tmp_path, n)) as ds:
            errors.append(abs(total_sphere_area(ds, center, radius) - exact) / exact)
    assert errors[-1] < 0.01
    assert errors[0] > errors[1] > errors[2]


def test_sphere_normals_and_padding_matters(tmp_path):
    with ax.open_plotfile(uniform(tmp_path, 16)) as ds:
        center = (0.0, 0.0, 0.0)
        for chunk in ds.leaf_cells([]):
            s = surfaces.sphere_area_in_cells(chunk, center, 0.5)
            hit = s["area"] > 0
            norm = np.sqrt(s["normal_x"] ** 2 + s["normal_y"] ** 2 + s["normal_z"] ** 2)
            np.testing.assert_allclose(norm[hit], 1.0, rtol=1e-14)
            assert np.all(norm[~hit] == 0.0)
            r = np.sqrt(chunk.x**2 + chunk.y**2 + chunk.z**2)
            np.testing.assert_allclose(s["normal_x"][hit], chunk.x[hit] / r[hit])
        # Planning with the unpadded bounding box loses cells the sphere
        # crosses; the padded region keeps them.
        tight = ([-0.5] * 3, [0.5] * 3)
        lost = sum(surfaces.sphere_area_in_cells(c, center, 0.5)["area"].sum()
                   for c in ds.leaf_cells([], region=tight))
        assert lost < total_sphere_area(ds, center, 0.5)
        assert total_sphere_area(ds, center, 0.5) == pytest.approx(
            sum(surfaces.sphere_area_in_cells(c, center, 0.5)["area"].sum()
                for c in ds.leaf_cells([])), rel=1e-14)


def test_multilevel_sphere_area(tmp_path):
    levels = [Level([((0, 0, 0), (15, 15, 15))]),
              Level([((8, 8, 8), (23, 23, 23))]),
              Level([((20, 20, 20), (43, 43, 43))])]
    with ax.open_plotfile(uniform(tmp_path, 16, levels)) as ds:
        radius = 0.4
        area = total_sphere_area(ds, (0.0, 0.0, 0.0), radius)
        assert area == pytest.approx(4 * math.pi * radius**2, rel=0.02)


def test_cylinder_areas_converge(tmp_path):
    radius, half = 0.55, 0.37
    center = (0.011, -0.017, 0.003)
    wall_errors, cap_errors = [], []
    for n in (16, 32, 64):
        with ax.open_plotfile(uniform(tmp_path, n)) as ds:
            wall = upper = lower = mx = my = 0.0
            region = surfaces.cylinder_region(ds, center, radius, half)
            for chunk in ds.leaf_cells([], region=region):
                s = surfaces.cylinder_area_in_cells(chunk, center, radius, half)
                wall += s["wall_area"].sum()
                upper += s["upper_cap_area"].sum()
                lower += s["lower_cap_area"].sum()
                mx += (s["cap_moment_x"] * (s["upper_cap_area"] > 0)).sum()
                my += (s["cap_moment_y"] * (s["upper_cap_area"] > 0)).sum()
        # Caps are exact: the clipped footprints tile the disk.
        assert upper == pytest.approx(math.pi * radius**2, rel=1e-12)
        assert lower == pytest.approx(math.pi * radius**2, rel=1e-12)
        assert abs(mx) < 1e-12 and abs(my) < 1e-12
        wall_errors.append(abs(wall - 4 * math.pi * radius * half)
                           / (4 * math.pi * radius * half))
    assert wall_errors[-1] < 0.01 and wall_errors[0] > wall_errors[-1]


def test_cap_on_a_face_is_counted_once(tmp_path):
    with ax.open_plotfile(uniform(tmp_path, 16)) as ds:
        half = 0.25   # exactly on the face between z cells 9 and 10
        total_upper = total_lower = 0.0
        for chunk in ds.leaf_cells([]):
            s = surfaces.cylinder_area_in_cells(chunk, (0.0, 0.0, 0.0), 0.6, half)
            total_upper += s["upper_cap_area"].sum()
            total_lower += s["lower_cap_area"].sum()
            # The interior-side cell takes the cap: z in [0.125, 0.25).
            hit = s["upper_cap_area"] > 0
            assert np.all(np.isclose(chunk.z[hit], 0.1875))
        assert total_upper == pytest.approx(math.pi * 0.36, rel=1e-12)
        assert total_lower == pytest.approx(math.pi * 0.36, rel=1e-12)


def test_disk_rectangle_moments_are_exact():
    r = 2.0
    area, mx, my = surfaces.disk_rectangle_moments(r, 0.0, r, 0.0, r)
    assert area == pytest.approx(math.pi * r * r / 4, rel=1e-14)
    assert mx == pytest.approx(r**3 / 3, rel=1e-14)
    assert my == pytest.approx(r**3 / 3, rel=1e-14)
    area, mx, my = surfaces.disk_rectangle_moments(r, -5.0, 5.0, -5.0, 5.0)
    assert area == pytest.approx(math.pi * r * r, rel=1e-14)
    assert abs(mx) < 1e-14 and abs(my) < 1e-14
    assert surfaces.disk_rectangle_moments(r, 3.0, 4.0, 0.0, 1.0) == (0.0, 0.0, 0.0)


def test_errors(tmp_path, refined_2d):
    with ax.open_plotfile(uniform(tmp_path, 8)) as ds:
        chunk = next(ds.leaf_cells([]))
        with pytest.raises(ax.AnalysisError):
            surfaces.sphere_area_in_cells(chunk, (0, 0, 0), -1.0)
        with pytest.raises(ax.AnalysisError):
            surfaces.cylinder_area_in_cells(chunk, (0, 0), 1.0, 1.0)
    with ax.open_plotfile(refined_2d[0]) as ds:
        with pytest.raises(ax.AnalysisError, match="3-D"):
            surfaces.sphere_area_in_cells(next(ds.leaf_cells([])), (0, 0, 0), 0.5)
