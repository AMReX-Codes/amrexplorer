"""Batch ergonomics -- cancellation, progress and sequences."""

import threading

import numpy as np
import pytest

import amrexplorer as ax
from amrexplorer import parallel, sequence
from conftest import density, temperature
from plotfile_writer import Level, write_plotfile


def test_cancel_token_stops_reads_and_queries(refined_3d):
    path, _ = refined_3d
    token = ax.CancelToken()
    assert not token.cancelled
    with ax.open_plotfile(path) as ds:
        plan = ds.plan_leaf_chunks(["density"])
        ds.read_leaf_chunk(plan[0], cancel=token)
        token.cancel()
        assert token.cancelled
        with pytest.raises(ax.ReadCancelled):
            ds.read_leaf_chunk(plan[0], cancel=token)
        with pytest.raises(ax.ReadCancelled):
            ds.project("density", "z", cancel=token)
        with pytest.raises(ax.ReadCancelled):
            ds.slice("density", "z", cancel=token)
        with pytest.raises(ax.ReadCancelled):
            ds.slice_cell_average("density", "z", cancel=token)
        with pytest.raises(ax.ReadCancelled):
            list(ds.leaf_cells(["density"], cancel=token))


def test_cancel_from_progress_stops_map_reduce(refined_3d):
    path, _ = refined_3d
    token = ax.CancelToken()
    seen = []

    def progress(done, total):
        seen.append(done)
        if done == 2:
            token.cancel()

    for executor in ("serial", "threads"):
        seen.clear()
        token = ax.CancelToken()
        with pytest.raises(ax.ReadCancelled):
            parallel.map_reduce(path, parallel.LeafCells(["density"]),
                                lambda c: c.size, lambda a, b: a + b,
                                executor=executor, workers=2, max_in_flight=1,
                                progress=progress, cancel=token)
        assert max(seen) < 9


def test_progress_callbacks(refined_3d):
    path, _ = refined_3d
    calls = []
    with ax.open_plotfile(path) as ds:
        chunks = list(ds.leaf_cells(["density"],
                                    progress=lambda d, t: calls.append((d, t))))
    assert calls == [(i + 1, len(chunks)) for i in range(len(chunks))]


def test_find_and_open_sequence(tmp_path):
    for step in (10, 2, 100):
        write_plotfile(tmp_path / f"plt{step:05d}", dim=2, domain_cells=(4, 4),
                       fields={"density": density}, levels=[Level([((0, 0), (3, 3))])],
                       time=float(step))
    (tmp_path / "plt00002_projection.png").write_text("not a plotfile")
    (tmp_path / "chk00005").mkdir()
    found = sequence.find_plotfiles(tmp_path)
    assert [p.rsplit("/", 1)[-1] for p in found] == ["plt00002", "plt00010", "plt00100"]
    assert sequence.find_plotfiles(str(tmp_path / "plt0001*")) == found[1:2]
    assert sequence.find_plotfiles(found[0]) == found[:1]
    assert sequence.find_plotfiles([found[2], tmp_path]) == found
    times = []
    for ds in sequence.open_each(found):
        times.append(ds.time)
        last = ds
    assert times == [2.0, 10.0, 100.0] and last.closed
    out = sequence.output_path(found[0], "_slice.png")
    assert out.name == "plt00002_slice.png" and sequence.needs_output(out)
    out.write_text("")
    assert not sequence.needs_output(out) and sequence.needs_output(out, True)
