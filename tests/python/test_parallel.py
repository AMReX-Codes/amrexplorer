"""Parallel execution. Validates parallel execution: serial,
thread and process executors give bit-identical ordered reductions on
multilevel data, under either ownership model."""

import numpy as np
import pytest

import amrexplorer as ax
from amrexplorer import parallel
from conftest import density


def weighted_moments(chunk):
    rho = chunk["density"]
    volume = chunk.cell_volume
    # Deliberately order-sensitive floating point: a sum of products whose
    # last bits depend on the order partials are added in.
    return np.array([(rho * volume).sum(), (rho * rho * volume).sum() * 1e-3,
                     volume.sum(), chunk.size], dtype=np.float64)


def add(left, right):
    return left + right


def particle_mass(chunk):
    return float(np.sum(chunk["mass"]))


def serial_reference(path):
    with ax.open_plotfile(path) as ds:
        total = None
        for chunk in ds.leaf_cells(["density", "temperature"]):
            partial = weighted_moments(chunk)
            total = partial if total is None else total + partial
        return total


@pytest.mark.parametrize("executor,ownership,workers", [
    ("serial", "worker", 1),
    ("threads", "worker", 3),
    ("threads", "shared", 4),
    ("processes", "worker", 2),
])
def test_ordered_reduction_is_bit_identical(refined_3d, executor, ownership,
                                            workers):
    path, _ = refined_3d
    expected = serial_reference(path)
    request = parallel.LeafCells(["density", "temperature"])
    result = parallel.map_reduce(path, request, weighted_moments, add,
                                 executor=executor, workers=workers,
                                 ownership=ownership, max_in_flight=3)
    assert result.tobytes() == expected.tobytes()


def test_open_dataset_and_progress(refined_3d):
    path, _ = refined_3d
    calls = []
    with ax.open_plotfile(path, derived=[("twice", "2*density")],
                          cache_bytes=1 << 22) as ds:
        plan = ds.plan_leaf_chunks(["twice"])
        result = parallel.map_reduce(
            ds, parallel.LeafCells(["twice"]),
            lambda c: float(c["twice"].sum()), add, 0.0,
            executor="threads", workers=2,
            progress=lambda done, total: calls.append((done, total)))
        expected = 0.0
        for chunk in ds.leaf_cells(["twice"]):
            expected += float(chunk["twice"].sum())
    assert result == expected
    assert calls[-1] == (len(plan), len(plan))
    assert len(calls) == len(plan)


def test_partitioned_ranks_combine_to_the_whole(refined_3d):
    path, _ = refined_3d
    request = parallel.LeafCells(["density"])
    with ax.open_plotfile(path) as ds:
        plan = request.plan(ds)
        whole = parallel.map_reduce(ds, request, lambda c: c.size, add,
                                    executor="serial")
        ranks = [parallel.map_reduce(ds, request, lambda c: c.size, add, 0,
                                     executor="threads", workers=2,
                                     items=plan.partition(rank, 3))
                 for rank in range(3)]
    assert sum(ranks) == whole == plan.total_cells
    with pytest.raises(ax.AnalysisError):
        plan.partition(3, 3)


def test_unordered_reduction_has_the_same_total_count(refined_3d):
    path, _ = refined_3d
    request = parallel.LeafCells(["density"])
    count = parallel.map_reduce(path, request, lambda c: c.size, add,
                                executor="threads", workers=4, ordered=False)
    with ax.open_plotfile(path) as ds:
        assert count == request.plan(ds).total_cells


def test_particles_in_parallel(tmp_path):
    from plotfile_writer import Level, ParticleSpecies, write_plotfile

    rng = np.random.default_rng(5)
    grids = {g: (rng.random((50 + g, 2)), rng.random((50 + g, 1)),
                 np.zeros((50 + g, 0), dtype=int),
                 np.arange(1000 * g + 1, 1000 * g + 51 + g)) for g in range(4)}
    path = write_plotfile(
        tmp_path / "pltpp", dim=2, domain_cells=(8, 8),
        fields={"density": density},
        levels=[Level([((0, 0), (3, 3)), ((4, 0), (7, 3)), ((0, 4), (3, 7)),
                       ((4, 4), (7, 7))])],
        particles=[ParticleSpecies("stars", ["mass"], [], grids)])
    request = parallel.Particles("stars", ["mass"])
    serial = parallel.map_reduce(path, request, particle_mass, add,
                                 executor="serial")
    threads = parallel.map_reduce(path, request, particle_mass, add,
                                  executor="threads", workers=3)
    processes = parallel.map_reduce(path, request, particle_mass, add,
                                    executor="processes", workers=2)
    assert serial == threads == processes
    assert serial == pytest.approx(sum(g[1].sum() for g in grids.values()))


def test_executor_errors_propagate(refined_3d):
    path, _ = refined_3d

    def broken(chunk):
        raise RuntimeError("mapper failed")

    with pytest.raises(RuntimeError, match="mapper failed"):
        parallel.map_reduce(path, parallel.LeafCells(["density"]), broken, add,
                            executor="threads", workers=2)
    with pytest.raises(ax.AnalysisError):
        parallel.map_reduce(path, parallel.LeafCells(["density"]), broken, add,
                            executor="processes", ownership="shared")
    with pytest.raises(ax.AnalysisError):
        parallel.map_reduce(path, parallel.LeafCells(["density"]), broken, add,
                            executor="gpu")
