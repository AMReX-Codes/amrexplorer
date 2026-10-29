"""Smoke tests: every example script runs on a small fixture and writes its
outputs. They validate that the examples execute, not their numbers -- the
comparisons with yt and Kangaroo are tools/analysis_validation's."""

import os
import subprocess
import sys

import numpy as np
import pytest

pytest.importorskip("matplotlib")

HERE = os.path.dirname(__file__)
EXAMPLES = os.path.join(HERE, "..", "..", "python", "examples")
VALIDATION = os.path.join(HERE, "..", "..", "tools", "analysis_validation")


def run(script, *args, cwd=None):
    env = dict(os.environ, MPLBACKEND="Agg")
    completed = subprocess.run(
        [sys.executable, os.path.join(EXAMPLES, script), *map(str, args)],
        cwd=cwd, env=env, capture_output=True, text=True, timeout=300)
    assert completed.returncode == 0, completed.stderr
    return completed.stdout


@pytest.fixture(scope="module")
def galaxy(tmp_path_factory):
    sys.path.insert(0, VALIDATION)
    import make_particle_fixture
    from plotfile_writer import Level, write_plotfile

    root = tmp_path_factory.mktemp("examples")
    path = root / "plt00010"
    sys.argv = ["make_particle_fixture.py", str(path), "--particles-per-grid", "200"]
    make_particle_fixture.main()
    magnetized = write_plotfile(
        root / "plt_b", dim=3, domain_cells=(8, 8, 8),
        fields={"gasDensity": lambda l, i, j, k: 1e-24 * (1.0 + i + j + k),
                "x-BField": lambda l, i, j, k: 1.0 + 0.0 * i,
                "y-BField": lambda l, i, j, k: 0.1 * (j - 4.0),
                "z-BField": lambda l, i, j, k: 0.0 * i},
        levels=[Level([((0, 0, 0), (7, 7, 7))])])
    return root, path, magnetized


def test_slice_and_fab_minmax(galaxy, tmp_path):
    _, path, _ = galaxy
    run("plotfile_slice.py", path, "--var", "gasDensity", "--output",
        tmp_path / "s.png", "--npz", tmp_path / "s.npz")
    assert np.load(tmp_path / "s.npz")["slice"].ndim == 2
    run("plotfile_slice.py", path, "--sampled", "--no-plot")
    out = run("plotfile_fab_minmax.py", path, "--level", "0")
    assert "gasDensity" in out


def test_frames_with_streamlines_and_skip(galaxy, tmp_path):
    _, path, magnetized = galaxy
    out = run("plot_frames.py", magnetized, "--kind", "slice", "--axis", "z",
              "--resolution", "64", "--output-dir", tmp_path)
    assert "wrote" in out
    assert run("plot_frames.py", magnetized, "--kind", "slice", "--axis", "z",
               "--output-dir", tmp_path) == ""
    run("plot_frames.py", path, "--kind", "projection", "--axis", "z",
        "--zoom", "2", "--resolution", "64", "--output-dir", tmp_path)
    assert (tmp_path / "plt00010_projection_z_gasDensity.png").exists()
