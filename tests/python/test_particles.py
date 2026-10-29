"""Complete particle attributes, one chunk per grid.

Validates raw source values: every attribute of every particle, against the
arrays the fixture wrote."""

import numpy as np
import pytest

import amrexplorer as ax
from plotfile_writer import Level, ParticleSpecies, write_plotfile


def species_grids(rng, counts, dim, nreal, nint, first_id=1):
    grids = {}
    next_id = first_id
    for grid, n in counts.items():
        grids[grid] = (rng.random((n, dim)), rng.random((n, nreal)),
                       rng.integers(-5, 50, (n, nint)),
                       np.arange(next_id, next_id + n))
        next_id += n
    return grids


@pytest.fixture
def particle_plotfile(tmp_path):
    rng = np.random.default_rng(3)
    boxes = [((0, 0, 0), (3, 7, 7)), ((4, 0, 0), (7, 7, 3)),
             ((4, 0, 4), (7, 7, 5)), ((4, 0, 6), (7, 7, 7))]
    stars = ParticleSpecies(
        "stars", ["mass", "age"], ["kind", "flag"],
        species_grids(rng, {0: 7, 1: 0, 2: 3, 3: 5}, 3, 2, 2))
    tracers = ParticleSpecies(
        "tracers", [], [], species_grids(rng, {1: 4}, 3, 0, 0, first_id=100),
        dtype="float32")
    path = write_plotfile(tmp_path / "pltp", dim=3, domain_cells=(8, 8, 8),
                          fields={"rho": lambda l, i, j, k: 1.0 + i},
                          levels=[Level(boxes)], particles=[stars, tracers])
    return path, stars, tracers


def test_species_metadata(particle_plotfile):
    path, stars, tracers = particle_plotfile
    with ax.open_plotfile(path) as ds:
        info = {s["name"]: s for s in ds.particle_species}
        assert sorted(info) == ["stars", "tracers"]
        assert info["stars"]["real_components"] == ["mass", "age"]
        assert info["stars"]["int_components"] == ["kind", "flag"]
        assert info["stars"]["num_particles"] == 15
        assert info["stars"]["dtype"] == "float64"
        assert info["tracers"]["dtype"] == "float32"
        assert info["tracers"]["real_components"] == []


def test_one_chunk_per_nonempty_grid_in_order(particle_plotfile):
    path, stars, _ = particle_plotfile
    with ax.open_plotfile(path) as ds:
        plan = ds.plan_particle_chunks("stars")
        assert [(i.level, i.grid, i.num_particles) for i in plan] == [
            (0, 0, 7), (0, 2, 3), (0, 3, 5)]
        assert plan.total_particles == 15
        assert plan.fields == ["mass", "age", "kind", "flag"]
        for item, chunk in zip(plan, ds.particle_chunks("stars")):
            positions, reals, ints, ids = stars.grids[item.grid]
            assert chunk.grid == item.grid and chunk.size == len(ids)
            np.testing.assert_array_equal(chunk.x, positions[:, 0])
            np.testing.assert_array_equal(chunk.z, positions[:, 2])
            np.testing.assert_array_equal(chunk["mass"], reals[:, 0])
            np.testing.assert_array_equal(chunk["age"], reals[:, 1])
            np.testing.assert_array_equal(chunk["kind"], ints[:, 0])
            np.testing.assert_array_equal(chunk["flag"], ints[:, 1])
            np.testing.assert_array_equal(chunk.id, ids)
            np.testing.assert_array_equal(chunk.cpu, 3)
            assert chunk["mass"].dtype == np.float64
            assert chunk["kind"].dtype == np.int32
            assert chunk.id.dtype == np.int64
            with pytest.raises(ValueError):
                chunk["mass"][0] = 0.0


def test_field_selection_and_errors(particle_plotfile):
    path, stars, _ = particle_plotfile
    with ax.open_plotfile(path) as ds:
        chunk = next(ds.particle_chunks("stars", ["age"]))
        assert list(chunk) == ["age"]
        with pytest.raises(KeyError):
            chunk["mass"]
        np.testing.assert_array_equal(chunk.y, stars.grids[0][0][:, 1])
        with pytest.raises(ax.AnalysisError, match="unknown particle field"):
            ds.plan_particle_chunks("stars", ["nothing"])
        with pytest.raises(ax.AnalysisError, match="unknown particle species"):
            ds.plan_particle_chunks("gas")
        with pytest.raises(ax.AnalysisError, match="twice"):
            ds.plan_particle_chunks("stars", ["mass", "mass"])


def test_single_precision_and_positions_only(particle_plotfile):
    path, _, tracers = particle_plotfile
    with ax.open_plotfile(path) as ds:
        chunks = list(ds.particle_chunks("tracers"))
        assert len(chunks) == 1 and len(chunks[0]) == 0
        chunk = chunks[0]
        assert chunk.x.dtype == np.float32
        np.testing.assert_array_equal(
            chunk.x, tracers.grids[1][0][:, 0].astype(np.float32))
        np.testing.assert_array_equal(chunk.id, np.arange(100, 104))


def test_particle_chunks_outlive_dataset(particle_plotfile):
    path, stars, _ = particle_plotfile
    ds = ax.open_plotfile(path)
    plan = ds.plan_particle_chunks("stars")
    mass = ds.read_particle_chunk(plan[0])["mass"]
    ds.close()
    np.testing.assert_array_equal(mass, stars.grids[0][1][:, 0])
    with pytest.raises(ax.DatasetClosed):
        ds.read_particle_chunk(plan[0])


def test_particle_items_are_bound_to_their_dataset(particle_plotfile):
    path, _, _ = particle_plotfile
    with ax.open_plotfile(path) as one, ax.open_plotfile(path) as two:
        plan = one.plan_particle_chunks("stars")
        with pytest.raises(ax.AnalysisError, match="different dataset"):
            two.read_particle_chunk(plan[0])


def test_partition_covers_particles_once(particle_plotfile):
    path, _, _ = particle_plotfile
    with ax.open_plotfile(path) as ds:
        plan = ds.plan_particle_chunks("stars")
        ids = np.concatenate([ds.read_particle_chunk(i).id
                              for rank in range(2)
                              for i in plan.partition(rank, 2)])
        assert sorted(ids.tolist()) == list(range(1, 16))
