"""Smoke tests: every example script runs on a small fixture and writes its
outputs. They validate that the examples execute, not their numbers -- the
comparisons with yt and Kangaroo are tools/analysis_validation's."""

import json
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
    sfr = root / "plt_sfr"
    import make_sfr_fixture

    sys.argv = ["make_sfr_fixture.py", str(sfr)]
    make_sfr_fixture.main()
    magnetized = write_plotfile(
        root / "plt_b", dim=3, domain_cells=(8, 8, 8),
        fields={"gasDensity": lambda l, i, j, k: 1e-24 * (1.0 + i + j + k),
                "x-BField": lambda l, i, j, k: 1.0 + 0.0 * i,
                "y-BField": lambda l, i, j, k: 0.1 * (j - 4.0),
                "z-BField": lambda l, i, j, k: 0.0 * i},
        levels=[Level([((0, 0, 0), (7, 7, 7))])])
    return root, path, sfr, magnetized


def test_temperature_phase_masses(galaxy):
    _, path, _, _ = galaxy
    result = json.loads(run("temperature_phase_masses.py", path))
    assert result["leaf_cells"] > 0
    assert result["total_mass_native"] == pytest.approx(
        sum(p["mass_native"] for p in result["phases"]))


def test_enclosed_sfr(galaxy, tmp_path):
    _, _, sfr, _ = galaxy
    run("plotfile_enclosed_sfr.py", sfr, "--nbins", "8", "--rmax-kpc", "8",
        "--zmax-kpc", "8", "--output-json", tmp_path / "d.json",
        "--output-json-wind", tmp_path / "w.json",
        "--output-json-joint", tmp_path / "j.json")
    disk = json.loads((tmp_path / "d.json").read_text())
    assert disk["mode"] == "radius" and len(disk["radii_kpc"]) == 8


def test_particle_histograms(galaxy, tmp_path):
    root, path, _, _ = galaxy
    out = run("plotfile_particle_mass_histogram.py", path, "--bins", "8",
              "--output", tmp_path / "h.csv", "--plot-output", tmp_path / "h.png")
    assert "bin_lo,bin_hi,count" in out and (tmp_path / "h.png").exists()
    out = run("stellar_pop_mass_hist.py", path, "--bins", "5", "--png",
              tmp_path / "s.png")
    assert out.startswith("# stage") and (tmp_path / "s.png").exists()


def test_kennicutt_schmidt(galaxy, tmp_path):
    _, path, _, _ = galaxy
    out = run("compute_kennicutt_schmidt.py", path, "--resolution", "16",
              "--sfr-window-myr", "50", "--output-csv", tmp_path / "ks.csv",
              "--output-plot", tmp_path / "ks.png")
    assert "Valid KS pixels" in out
    data = np.loadtxt(tmp_path / "ks.csv", delimiter=",", skiprows=1, ndmin=2)
    assert data.shape[1] == 4


def test_projection_slice_profile_and_phase(galaxy, tmp_path):
    _, path, _, _ = galaxy
    run("plotfile_projection.py", path, "--var", "gasDensity", "--output",
        tmp_path / "p.png", "--npz", tmp_path / "p.npz")
    assert np.load(tmp_path / "p.npz")["surface_density_msun_pc2"].shape == (16, 16)
    run("plotfile_slice.py", path, "--var", "gasDensity", "--output",
        tmp_path / "s.png", "--npz", tmp_path / "s.npz")
    run("plotfile_slice.py", path, "--sampled", "--no-plot")
    run("plot_surface_density_profile.py", path, "--resolution", "32",
        "--n-bins", "8", "--r-max-kpc", "8", "--output-dir", tmp_path)
    assert (tmp_path / "plt00010_surface_density_profile.csv").exists()
    run("plot_density_temperature_phase.py", path, "--bins", "16", "--output",
        tmp_path / "phase.png")
    assert np.load(tmp_path / "phase.npz")["hist"].shape == (16, 16)


def test_frames_with_streamlines_and_skip(galaxy, tmp_path):
    _, path, _, magnetized = galaxy
    out = run("plot_frames.py", magnetized, "--kind", "slice", "--axis", "z",
              "--resolution", "64", "--output-dir", tmp_path)
    assert "wrote" in out
    assert run("plot_frames.py", magnetized, "--kind", "slice", "--axis", "z",
               "--output-dir", tmp_path) == ""
    run("plot_frames.py", path, "--kind", "projection", "--axis", "z",
        "--zoom", "2", "--resolution", "64", "--output-dir", tmp_path)
    assert (tmp_path / "plt00010_projection_z_gasDensity.png").exists()
