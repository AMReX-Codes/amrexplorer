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


def test_clear_unpinned_cache_releases_read_blocks(refined_3d):
    path, _ = refined_3d
    with ax.open_plotfile(path) as ds:
        chunks = list(ds.leaf_cells(["density"]))
        assert ds.cache_metrics()["resident_bytes"] > 0
        ds.clear_unpinned_cache()
        metrics = ds.cache_metrics()
        assert metrics["resident_bytes"] == 0
        assert metrics["pinned_bytes"] == 0
        # Chunks are copies and outlive the cleared blocks.
        assert all(chunk["density"].size == chunk.size for chunk in chunks)


class _RecordingCells(parallel.LeafCells):
    """Records each handle's resident cache bytes as a read starts."""

    resident = []

    @staticmethod
    def read(ds, item, cancel=None):
        _RecordingCells.resident.append(
            (id(ds), ds.cache_metrics()["resident_bytes"]))
        return parallel.LeafCells.read(ds, item, cancel)


@pytest.mark.parametrize("executor", ["serial", "threads"])
def test_worker_handles_hold_no_blocks_between_items(refined_3d, executor):
    # With a path, every handle -- the serial executor's included -- is the
    # call's own, so each is cleared after every item.
    path, _ = refined_3d
    _RecordingCells.resident = []
    count = parallel.map_reduce(path, _RecordingCells(["density"]),
                                lambda c: c.size, add, executor=executor,
                                workers=2)
    with ax.open_plotfile(path) as ds:
        plan = ds.plan_leaf_chunks(["density"])
        assert count == plan.total_cells
        assert len(_RecordingCells.resident) == len(plan)
    # Every worker reads several items, and each starts with an empty cache.
    assert len({handle for handle, _ in _RecordingCells.resident}) <= 2
    assert all(resident == 0 for _, resident in _RecordingCells.resident)


@pytest.mark.parametrize("executor,ownership", [("threads", "shared"),
                                                ("serial", "worker")])
def test_callers_handle_keeps_its_cache(refined_3d, executor, ownership):
    path, _ = refined_3d
    with ax.open_plotfile(path) as ds:
        parallel.map_reduce(ds, parallel.LeafCells(["density"]),
                            lambda c: c.size, add, executor=executor,
                            workers=2, ownership=ownership)
        assert ds.cache_metrics()["resident_bytes"] > 0


def test_default_workers(monkeypatch):
    monkeypatch.delenv(parallel.WORKERS_VARIABLE, raising=False)
    monkeypatch.setattr(parallel, "_available_cpus", lambda: 128)
    assert parallel.default_workers() == parallel.DEFAULT_MAX_WORKERS == 8
    monkeypatch.setattr(parallel, "_available_cpus", lambda: 3)
    assert parallel.default_workers() == 3
    # The variable is taken as given, above the cap and the CPU count alike.
    monkeypatch.setenv(parallel.WORKERS_VARIABLE, "32")
    assert parallel.default_workers() == 32
    for bad in ("0", "-2", "many"):
        monkeypatch.setenv(parallel.WORKERS_VARIABLE, bad)
        with pytest.raises(ax.AnalysisError, match=parallel.WORKERS_VARIABLE):
            parallel.default_workers()


def test_workers_argument_overrides_the_variable(refined_3d, monkeypatch):
    path, _ = refined_3d
    monkeypatch.setenv(parallel.WORKERS_VARIABLE, "not a number")
    count = parallel.map_reduce(path, parallel.LeafCells(["density"]),
                                lambda c: c.size, add, executor="threads",
                                workers=2)
    assert count > 0
    with pytest.raises(ax.AnalysisError, match="at least 1"):
        parallel.map_reduce(path, parallel.LeafCells(["density"]),
                            lambda c: c.size, add, workers=0)
