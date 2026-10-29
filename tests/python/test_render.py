"""Headless rendering: the viewer's slice colouring and volume ray caster,
reached from Python. Validates visual equivalence (orientation, palette
mapping), not numbers."""

import numpy as np
import pytest

import amrexplorer as ax
from amrexplorer import render
from plotfile_writer import Level, write_plotfile


@pytest.fixture
def gradient_3d(tmp_path):
    return write_plotfile(tmp_path / "pltg", dim=3, domain_cells=(8, 8, 8),
                          fields={"u": lambda l, i, j, k: 1.0 + j + 0.0 * i + 0.0 * k},
                          levels=[Level([((0, 0, 0), (7, 7, 7))])])


def test_slice_image_orientation_and_palette(gradient_3d):
    with ax.open_plotfile(gradient_3d) as ds:
        image = render.slice_image(ds, "u", "z", 0.5, resolution=(8, 8),
                                   palette="viridis", vmin=1.0, vmax=8.0)
        assert image.shape == (8, 8, 4) and image.dtype == np.uint8
        assert np.all(image[..., 3] == 255)
        # u grows with y, and row 0 is the top of the picture.
        top, bottom = image[0, 0, :3], image[-1, 0, :3]
        assert np.all(image[0] == image[0, :1])      # rows are uniform
        viridis_end = np.array([0xFD, 0xE7, 0x25])   # viridis' yellow
        assert np.abs(top.astype(int) - viridis_end).max() < 24
        assert bottom[2] > bottom[1] * 0.8            # dark purple-blue end
        logged = render.slice_image(ds, "u", "z", 0.5, resolution=(8, 8), log=True)
        assert logged.shape == (8, 8, 4)
        with pytest.raises(ax.AnalysisError):
            render.slice_image(ds, "u", "z", 0.5, vmin=2.0, vmax=1.0)


def test_volume_image(gradient_3d, tmp_path):
    with ax.open_plotfile(gradient_3d) as ds:
        image = render.volume_image(ds, "u", size=(64, 48), max_opacity=0.2)
        assert image.shape == (48, 64, 4)
        assert image[..., 3].max() > 0 and image[0, 0, 3] == 0   # corner empty
        pytest.importorskip("matplotlib")
        render.save_png(image, tmp_path / "v.png")
        assert (tmp_path / "v.png").stat().st_size > 0
        with pytest.raises(ax.AnalysisError):
            render.volume_image(ds, "nothing")
        with pytest.raises(ax.AnalysisError):
            render.volume_image(ds, "u", vmin=1.0)


def test_volume_rendering_needs_3d(refined_2d):
    with ax.open_plotfile(refined_2d[0]) as ds:
        with pytest.raises(ax.AnalysisError, match="3-D"):
            render.volume_image(ds, "density")
