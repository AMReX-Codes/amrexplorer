"""Exact leaf-cell traversal.

These tests validate raw source values and analysis totals against an
independent NumPy reconstruction of the fixture plotfiles."""

import _thread
import concurrent.futures
import gc
import os
import pathlib
import shutil
import signal
import sys
import threading
import time
import traceback
import warnings

import numpy as np
import pytest

import amrexplorer as ax
from conftest import density, leaf_reference, temperature
from plotfile_writer import GHOST_VALUE, Level, write_plotfile


def gather(ds, plan):
    rows = []
    for item in plan:
        chunk = ds.read_leaf_chunk(item)
        idx = chunk.indices() + (np.zeros(chunk.size, dtype=np.int32),) * (
            3 - chunk.dimension)
        for c in zip(*idx):
            rows.append((chunk.level, chunk.grid) + tuple(int(v) for v in c))
    return rows


def test_metadata(refined_3d):
    path, levels = refined_3d
    with ax.open_plotfile(path) as ds:
        assert ds.fields == ["density", "temperature"]
        assert ds.stored_fields == ds.fields and ds.derived_fields == []
        assert ds.time == 2.5
        assert ds.dimension == 3 and ds.finest_level == 2
        geometry = ds.geometry
        assert geometry["coordinate_system"] == "cartesian"
        assert geometry["domain_lower"] == [-1.0, -1.0, -1.0]
        info = ds.levels
        assert [l["dtype"] for l in info] == ["float64", "float32", "float32"]
        assert [l["ghost_cells"] for l in info] == [[1] * 3, [2] * 3, [0] * 3]
        assert [l["num_grids"] for l in info] == [4, 2, 1]
        assert info[1]["cell_size"] == [2.0 / 32] * 3
        assert ds.widest_dtype == np.float64
        assert ds.field_centering("density") == "cell"


@pytest.mark.parametrize("as_string", [False, True])
def test_unicode_path(refined_3d, tmp_path, as_string):
    source, _ = refined_3d
    path = tmp_path / "données_測試_🌌" / "plt00100"
    shutil.copytree(source, path)
    with ax.open_plotfile(str(path) if as_string else path) as ds:
        assert isinstance(ds.path, str)
        assert ds.path == str(path)
        assert "données_測試_🌌" in repr(ds)
        chunks = list(ds.leaf_cells(["density"]))
        assert chunks
        for chunk in chunks:
            i, j, k = chunk.indices()
            np.testing.assert_array_equal(
                chunk["density"], density(chunk.level, i, j, k))


def test_full_domain_partition_is_exact(refined_3d):
    path, levels = refined_3d
    with ax.open_plotfile(path) as ds:
        plan = ds.plan_leaf_chunks(["density"])
        got = gather(ds, plan)
    expected = leaf_reference(levels, 3)
    assert len(got) == len(set(got)), "a leaf cell appeared twice"
    assert sorted(got) == sorted(expected)
    # Canonical order: plan items by level then grid, cells within by FAB
    # (Fortran) order, which is exactly how the reference enumerates them.
    assert got == expected
    assert plan.total_cells == len(expected)


def test_total_volume_and_values(refined_3d):
    path, levels = refined_3d
    with ax.open_plotfile(path) as ds:
        volume = 0.0
        mass = 0.0
        expected_mass = 0.0
        for chunk in ds.leaf_cells(["density", "temperature"]):
            rho = chunk["density"]
            assert rho.dtype == np.float64
            assert not np.any(rho == GHOST_VALUE)
            stored = np.float32 if chunk.level > 0 else np.float64
            want = density(chunk.level, chunk.i, chunk.j, chunk.k).astype(stored)
            np.testing.assert_array_equal(rho, want.astype(np.float64))
            np.testing.assert_array_equal(
                chunk["temperature"],
                temperature(chunk.level, chunk.i, chunk.j, chunk.k))
            volume += chunk.cell_volume.sum()
            mass += (rho * chunk.cell_volume).sum()
            expected_mass += (want * chunk.cell_volume).sum()
        assert volume == pytest.approx(8.0, rel=1e-14)
        assert mass == pytest.approx(expected_mass, rel=1e-14)


def test_geometry_arrays(refined_3d):
    path, _ = refined_3d
    with ax.open_plotfile(path) as ds:
        plan = ds.plan_leaf_chunks([], dtype="float32")
        chunk = ds.read_leaf_chunk(plan[-1])
        dx = 2.0 / 64
        assert chunk.level == 2
        for name, axis in (("x", "i"), ("y", "j"), ("z", "k")):
            centers = getattr(chunk, name)
            assert centers.dtype == np.float64
            np.testing.assert_allclose(
                centers, -1.0 + (getattr(chunk, axis) + 0.5) * dx, rtol=0,
                atol=1e-15)
        assert chunk.dx.dtype == np.float64
        np.testing.assert_array_equal(chunk.dx, dx)
        np.testing.assert_array_equal(chunk.cell_volume, dx**3)
        assert chunk.i.dtype == np.int32
        # Fortran order: i fastest.
        assert list(chunk.i[:3]) == [10, 11, 12]
        assert chunk.j[0] == chunk.j[1]
        assert len(chunk) == 0 and chunk.size == 8**3
        with pytest.raises(KeyError):
            chunk["density"]


def test_arrays_are_read_only_and_masks_line_up(refined_2d):
    path, _ = refined_2d
    with ax.open_plotfile(path) as ds:
        chunk = next(ds.leaf_cells(["density", "temperature"]))
        rho = chunk["density"]
        with pytest.raises(ValueError):
            rho[0] = 1.0
        with pytest.raises(ValueError):
            chunk.x[0] = 1.0
        hot = chunk["temperature"] > 300
        assert rho[hot].shape == chunk.x[hot].shape == chunk.cell_volume[hot].shape
        with pytest.raises(AttributeError):
            chunk.z
        with pytest.raises(AttributeError):
            chunk.k
        assert not hasattr(chunk, "dz")


def test_two_dimensional_partition(refined_2d):
    path, levels = refined_2d
    with ax.open_plotfile(path) as ds:
        got = gather(ds, ds.plan_leaf_chunks(["density"]))
        volume = sum(c.cell_volume.sum() for c in ds.leaf_cells())
    assert got == leaf_reference(levels, 2)
    assert volume == pytest.approx(1.0, rel=1e-14)


def test_one_dimensional(tmp_path):
    levels = [Level([((0,), (15,))], ghost=2), Level([((8,), (15,))])]
    path = write_plotfile(tmp_path / "plt1d", dim=1, domain_cells=(16,),
                          fields={"u": lambda l, i, j, k: i + 0.25 * l},
                          levels=levels)
    with ax.open_plotfile(path) as ds:
        chunks = list(ds.leaf_cells(["u"]))
        assert [c.level for c in chunks] == [0, 1]
        np.testing.assert_array_equal(chunks[0].i, np.r_[0:4, 8:16])
        np.testing.assert_array_equal(chunks[0]["u"], np.r_[0:4, 8:16])
        np.testing.assert_array_equal(chunks[1]["u"], np.arange(8, 16) + 0.25)
        assert sum(c.cell_volume.sum() for c in chunks) == pytest.approx(1.0)
        assert not hasattr(chunks[0], "y")


def test_overlapping_coarsened_fine_grids(tmp_path):
    # Fine grids not aligned to the ratio coarsen onto a shared coarse cell;
    # that cell is covered once, not subtracted twice.
    levels = [Level([((0,), (7,))]), Level([((5,), (6,)), ((7,), (8,))])]
    path = write_plotfile(tmp_path / "pltodd", dim=1, domain_cells=(8,),
                          fields={"u": lambda l, i, j, k: i + 0.0},
                          levels=levels)
    with ax.open_plotfile(path) as ds:
        plan = ds.plan_leaf_chunks(["u"])
        assert plan[0].num_cells == 5
        np.testing.assert_array_equal(
            ds.read_leaf_chunk(plan[0]).i, [0, 1, 5, 6, 7])


def test_region_selects_by_cell_center_half_open(refined_2d):
    path, levels = refined_2d
    with ax.open_plotfile(path) as ds:
        # Level-1 centers are at (m + 0.5)/32, so 9.5/32 is exactly one of
        # them -- the boundary case the half-open rule decides.
        bound = 9.5 / 32
        region = ((0.0, 0.0), (bound, 1.0))
        plan = ds.plan_leaf_chunks(["density"], region=region)
        for chunk in (ds.read_leaf_chunk(item) for item in plan):
            assert np.all(chunk.x >= 0.0) and np.all(chunk.x < bound)
        cells = gather(ds, plan)
        full = leaf_reference(levels, 2)
        expected = []
        for cell in full:
            lev, _, i, j, _ = cell
            dx = 1.0 / (16 * 2**lev)
            if (i + 0.5) * dx < bound:
                expected.append(cell)
        assert cells == expected
        # A level-1 cell centered exactly on the upper bound is excluded; the
        # adjacent region picks it up, so the two partition the domain.
        other = ds.plan_leaf_chunks(["density"], region=((bound, 0.0), (1.0, 1.0)))
        assert sorted(cells + gather(ds, other)) == sorted(full)


@pytest.mark.parametrize("fields, region, match", [
    (["Density"], None, "unknown field 'Density'"),
    (["density", "density"], None, "twice"),
    (["density"], ((0.5, 0.0), (0.5, 1.0)), None),
    (["density"], ((0.0,), (1.0,)), None),
    (["density"], ((0.0, float("nan")), (1.0, 1.0)), None),
])
def test_request_errors(refined_2d, fields, region, match):
    path, _ = refined_2d
    with ax.open_plotfile(path) as ds:
        with pytest.raises(ValueError, match=match) as raised:
            ds.plan_leaf_chunks(fields, region=region)
    assert isinstance(raised.value, ax.AnalysisError)


def test_region_outside_domain_is_empty(refined_2d):
    path, _ = refined_2d
    with ax.open_plotfile(path) as ds:
        assert len(ds.plan_leaf_chunks(["density"],
                                       region=((2.0, 2.0), (3.0, 3.0)))) == 0


def test_dtype_rules(refined_3d, tmp_path):
    path, _ = refined_3d
    with ax.open_plotfile(path, derived=[("twice", "2*density")]) as ds:
        plan = ds.plan_leaf_chunks(["density", "twice"])
        assert plan.dtype == np.float64
        for chunk in (ds.read_leaf_chunk(i) for i in plan):
            assert chunk["density"].dtype == chunk["twice"].dtype == np.float64
            np.testing.assert_array_equal(chunk["twice"], 2 * chunk["density"])
        plan32 = ds.plan_leaf_chunks(["density", "twice"], dtype=np.float32)
        chunk = ds.read_leaf_chunk(plan32[0])
        assert chunk["density"].dtype == np.float32
        assert chunk["twice"].dtype == np.float32
        assert chunk.x.dtype == np.float64
        with pytest.raises(ax.AnalysisError):
            ds.plan_leaf_chunks(["density"], dtype=np.int32)
    single = write_plotfile(
        tmp_path / "plt32", dim=2, domain_cells=(8, 8),
        fields={"rho": density},
        levels=[Level([((0, 0), (7, 7))], dtype="float32")])
    with ax.open_plotfile(single, derived=[("third", "rho/3")]) as ds:
        chunk = next(ds.leaf_cells(["rho", "third"]))
        assert chunk["rho"].dtype == np.float32
        # Evaluated in float64, then narrowed to the widest stored dtype.
        np.testing.assert_array_equal(
            chunk["third"],
            (chunk["rho"].astype(np.float64) / 3).astype(np.float32))
        wide = next(ds.leaf_cells(["rho"], dtype="float64"))
        np.testing.assert_array_equal(wide["rho"], chunk["rho"])


def test_derived_fields_strict_at_open(refined_2d):
    path, _ = refined_2d
    with pytest.raises(ax.DerivedFieldError):
        ax.open_plotfile(path, derived=[("bad", "nosuchfield*2")])
    with pytest.raises(ax.DerivedFieldError):
        ax.open_plotfile(path, derived=[ax.DerivedField("bad", "density +")])
    with ax.open_plotfile(
            path, derived=[ax.DerivedField("p", "density*temperature"),
                           {"name": "q", "expression": "p + x"}]) as ds:
        assert ds.derived_fields == ["p", "q"]
        for chunk in ds.leaf_cells(["density", "temperature", "p", "q"]):
            np.testing.assert_allclose(
                chunk["p"], chunk["density"] * chunk["temperature"], rtol=1e-15)
            np.testing.assert_allclose(chunk["q"], chunk["p"] + chunk.x,
                                       rtol=1e-15)


def test_plan_rejects_items_over_budget(refined_3d):
    path, _ = refined_3d
    # Level-0 grid 0 is 8^3 valid, 10^3 grown: 8000 bytes per block plus the
    # allowance. The largest grid (16x16x8, 18x18x10 grown) fits two blocks
    # in 64 KiB; grid 2 (16x8x8) cannot fit four (two stored fields and two
    # derived).
    with ax.open_plotfile(path, cache_bytes=64 * 1024,
                          derived=[("a", "density+1"), ("b", "a*temperature")]) as ds:
        plan = ds.plan_leaf_chunks(["density", "temperature"])
        assert plan[0].estimated_bytes == 2 * (1000 * 8 + ax._amrexplorer.BLOCK_ALLOWANCE_BYTES)
        list(ds.leaf_cells(["density", "temperature"]))
        # "b" pins b, a, density and temperature.
        with pytest.raises(ax.AnalysisError, match="level 0 grid 2") as error:
            ds.plan_leaf_chunks(["b"])
        assert "cache_bytes" in str(error.value)
        ds.set_cache_bytes(1 << 20)
        assert ds.cache_bytes == 1 << 20
        plan = ds.plan_leaf_chunks(["b"])
        assert plan[0].estimated_bytes == 4 * (1000 * 8 + ax._amrexplorer.BLOCK_ALLOWANCE_BYTES)


def test_read_after_budget_lowered_raises_naming_item(refined_3d):
    path, _ = refined_3d
    with ax.open_plotfile(path, cache_bytes=1 << 20) as ds:
        plan = ds.plan_leaf_chunks(["density"])
        ds.set_cache_bytes(1024)
        with pytest.raises(ax.CacheBudgetExceeded, match="plan item 0"):
            ds.read_leaf_chunk(plan[0])


def test_cache_budget_validation(refined_2d, monkeypatch):
    path, _ = refined_2d
    monkeypatch.setenv("AMREXPLORER_CACHE_SIZE_MB", "12")
    with ax.open_plotfile(path) as ds:
        assert ds.cache_bytes == 12 * 1024 * 1024
    with ax.open_plotfile(path, cache_bytes=5000) as ds:
        assert ds.cache_bytes == 5000
        with pytest.raises(ax.AnalysisError):
            ds.set_cache_bytes(0)
    bad_values = ["0", "lots", "12MB", str(2**64)]
    if sys.platform != "win32":
        # Windows has no empty environment variables: setting one to ""
        # removes it, and the default budget applies.
        bad_values.append("")
    for bad in bad_values:
        monkeypatch.setenv("AMREXPLORER_CACHE_SIZE_MB", bad)
        with pytest.raises(ax.AnalysisError, match="AMREXPLORER_CACHE_SIZE_MB"):
            ax.open_plotfile(path)
    monkeypatch.delenv("AMREXPLORER_CACHE_SIZE_MB")
    with ax.open_plotfile(path) as ds:
        assert ds.cache_bytes == 1 << 30 == ax.default_cache_bytes()
    with pytest.raises(ax.AnalysisError):
        ax.open_plotfile(path, cache_bytes=0)


def test_chunk_lifetime_and_no_pins(refined_3d):
    path, _ = refined_3d
    ds = ax.open_plotfile(path)
    plan = ds.plan_leaf_chunks(["density"])
    chunks = [ds.read_leaf_chunk(item) for item in plan]
    assert ds.cache_metrics()["pinned_bytes"] == 0
    array = chunks[-1]["density"]
    x = chunks[-1].x
    expected = array.copy()
    ds.close()
    assert ds.closed
    del chunks
    gc.collect()
    np.testing.assert_array_equal(array, expected)
    assert x.size == array.size
    with pytest.raises(ax.DatasetClosed):
        ds.read_leaf_chunk(plan[0])
    with pytest.raises(ax.DatasetClosed):
        ds.fields


def test_items_are_bound_to_their_dataset(refined_2d):
    path, _ = refined_2d
    with ax.open_plotfile(path) as first, ax.open_plotfile(path) as second:
        assert first.id != second.id
        plan = first.plan_leaf_chunks(["density"])
        assert plan[0].dataset_id == first.id
        with pytest.raises(ax.AnalysisError, match="different dataset"):
            second.read_leaf_chunk(plan[0])


def test_reading_is_repeatable_and_order_free(refined_3d):
    path, _ = refined_3d
    with ax.open_plotfile(path) as ds:
        plan = ds.plan_leaf_chunks(["density", "temperature"])
        forward = [ds.read_leaf_chunk(i)["density"].copy() for i in plan]
        ds.set_cache_bytes(64 * 1024)  # force re-reads from disk
        backward = [ds.read_leaf_chunk(i)["density"]
                    for i in reversed(list(plan))][::-1]
        for a, b in zip(forward, backward):
            np.testing.assert_array_equal(a, b)
        assert [i.index for i in plan] == list(range(len(plan)))
        assert plan[-1].index == len(plan) - 1


def test_unsupported_geometry_and_missing_plotfile(tmp_path):
    path = write_plotfile(tmp_path / "pltcyl", dim=2, domain_cells=(8, 8),
                          fields={"rho": density},
                          levels=[Level([((0, 0), (7, 7))])], coord_sys=1)
    with ax.open_plotfile(path) as ds:
        with pytest.raises(ax.AnalysisError, match="Cartesian"):
            ds.plan_leaf_chunks(["rho"])
    with pytest.raises(ax.PlotfileReadError):
        ax.open_plotfile(tmp_path / "nothing")


def test_region_over_a_huge_domain_is_not_empty(tmp_path):
    # A domain of exactly 2^64 cells, as deep levels of real runs reach: a
    # cell count of the region's index box wraps to zero in 64 bits, which
    # once made the planner skip the level.
    path = write_plotfile(tmp_path / "plthuge", dim=3,
                          domain_cells=(2**22, 2**21, 2**21),
                          fields={"u": lambda l, i, j, k: 1.0 + i + 0.0 * j},
                          levels=[Level([((0, 0, 0), (3, 3, 3))])])
    with ax.open_plotfile(path) as ds:
        full = ds.plan_leaf_chunks(["u"], region=(ds.geometry["domain_lower"],
                                                  ds.geometry["domain_upper"]))
        assert len(full) == 1 and full.total_cells == 64
        assert len(ds.plan_leaf_chunks(["u"])) == 1


def test_derived_spellings(refined_2d):
    path, _ = refined_2d
    with ax.open_plotfile(path, derived={"p": "density*temperature",
                                         "vx": "p + x"}) as ds:
        assert ds.derived_fields == ["p", "vx"]
        for chunk in ds.leaf_cells(["p", "vx"]):
            np.testing.assert_allclose(chunk["vx"], chunk["p"] + chunk.x,
                                       rtol=1e-15)
    # A bare string would otherwise unpack per character ('vx' -> v = x).
    with pytest.raises(TypeError, match="string"):
        ax.open_plotfile(path, derived=["vx"])
    with pytest.raises(TypeError, match="string"):
        ax.open_plotfile(path, derived="vx")
    with pytest.raises(TypeError, match="pair"):
        ax.open_plotfile(path, derived=[("a", "density", "extra")])


def test_metadata_survives_a_missing_fab(refined_3d, tmp_path):
    source, _ = refined_3d
    path = tmp_path / "partial"
    shutil.copytree(source, path)
    # Version-1 headers record no precision; level 0's comes from its first
    # FAB, which a partial copy lacks.
    (path / "Level_0" / "Cell_D_00000").unlink()
    with ax.open_plotfile(path) as ds:
        assert [l["dtype"] for l in ds.levels] == [None, "float32", "float32"]
        assert ds.widest_dtype is None
        with pytest.raises(ax.AnalysisError, match="precision"):
            ds.plan_leaf_chunks(["density"])
        # A dtype the script gives needs no level's precision, so planning
        # reads no FAB header and succeeds.
        plan = ds.plan_leaf_chunks(["density"], dtype="float32")
        assert plan.dtype == np.float32 and len(plan) > 0


def test_plan_items_compare_by_plan_and_position(refined_2d):
    path, _ = refined_2d
    with ax.open_plotfile(path) as ds:
        plan = ds.plan_leaf_chunks(["density"])
        other = ds.plan_leaf_chunks(["density"])
        assert len(plan) >= 3
        item = plan[2]
        assert item == plan[2] and hash(item) == hash(plan[2])
        assert item != plan[1]
        assert item in plan and item not in other
        assert plan.index(item) == 2 and plan.count(item) == 1
        assert other.count(item) == 0
        with pytest.raises(ValueError):
            other.index(item)
        with pytest.raises(ValueError):
            plan.index(item, 3)
        assert all(i in plan for i in plan.partition(0, 2))
        assert len(set(plan) | set(plan.partition(1, 2))) == len(plan)
        assert "density" not in plan


@pytest.mark.skipif(sys.platform == "win32",
                    reason="Windows names are UTF-16, never stray bytes")
def test_error_names_a_non_utf8_path(refined_2d, tmp_path):
    # A POSIX name is bytes. The message escapes the ones that are not
    # UTF-8; undecodable, they would replace the error with
    # UnicodeDecodeError.
    source, _ = refined_2d
    parent = os.path.join(os.fsencode(tmp_path), b"donn\xe9es")
    try:
        os.mkdir(parent)
    except OSError:
        pytest.skip("the filesystem refuses names that are not UTF-8")
    path = os.path.join(parent, b"plt00100")
    shutil.copytree(source, os.fsdecode(path))
    (pathlib.Path(os.fsdecode(path)) / "Level_1" / "Cell_D_00000").unlink()
    with ax.open_plotfile(os.fsdecode(path)) as ds:
        plan = ds.plan_leaf_chunks(["density"])
        item = next(item for item in plan if item.level == 1)
        with pytest.raises(ax.BlockReadError) as error:
            ds.read_leaf_chunk(item)
        assert "donn\\xe9es" in str(error.value)
    with pytest.raises(ax.PlotfileReadError) as error:
        ax.open_plotfile(os.fsdecode(os.path.join(parent, b"missing")))
    assert "donn\\xe9es" in str(error.value)


def test_cancelled_token_stops_open_plan_and_read(refined_2d):
    path, _ = refined_2d
    token = ax.CancelToken()
    token.cancel()
    with pytest.raises(ax.ReadCancelled):
        ax.open_plotfile(path, cancel=token)
    with ax.open_plotfile(path) as ds:
        with pytest.raises(ax.ReadCancelled):
            ds.plan_leaf_chunks(["density"], cancel=token)
        with pytest.raises(ax.ReadCancelled):
            list(ds.leaf_cells(["density"], cancel=token))
        item = ds.plan_leaf_chunks(["density"])[0]

        # Off the main thread no signal handler can run, so the token
        # reaches the read directly.
        def read_in_thread(cancel):
            with concurrent.futures.ThreadPoolExecutor(1) as pool:
                return pool.submit(ds.read_leaf_chunk, item,
                                   cancel=cancel).result()

        with pytest.raises(ax.ReadCancelled):
            read_in_thread(token)
        assert read_in_thread(ax.CancelToken()).size == item.num_cells


def test_read_error_names_a_unicode_path(refined_2d, tmp_path):
    # Version-2 headers record precision, so planning reads no FAB and the
    # missing file is first touched by the read.
    source, _ = refined_2d
    path = tmp_path / "données_測試_🌌" / "plt00100"
    shutil.copytree(source, path)
    (path / "Level_1" / "Cell_D_00000").unlink()
    with ax.open_plotfile(path) as ds:
        plan = ds.plan_leaf_chunks(["density"])
        item = next(item for item in plan if item.level == 1)
        with pytest.raises(ax.BlockReadError) as error:
            ds.read_leaf_chunk(item)
        assert "données_測試_🌌" in str(error.value)


@pytest.fixture
def slow_read(tmp_path):
    """A dataset and one plan item whose cold read takes most of a second:
    one 96^3 grid and sixteen derived fields of transcendental functions,
    evaluated on the first read. Reads check for cancellation between
    fields."""
    n = 96
    levels = [Level([((0, 0, 0), (n - 1,) * 3)], dtype="float64", ghost=0)]
    path = write_plotfile(tmp_path / "slow", dim=3, domain_cells=(n,) * 3,
                          fields={"density": density}, levels=levels)
    derived = [(f"d{i}", " + ".join(
        f"sqrt(density*density + {i + m}) * exp(-x*x*{m + 1})"
        f" * log(1 + y*y*{m})" for m in range(6))) for i in range(16)]
    with ax.open_plotfile(path, derived=derived) as ds:
        item = ds.plan_leaf_chunks([name for name, _ in derived],
                                   dtype="float32")[0]
        yield ds, item


class _Interrupter:
    """Sends SIGINT to the main thread again and again from the moment the
    read has begun (its first cache miss) until the block ends. One signal on
    a timer could land before the read starts, or after a fast read has
    returned and pytest.raises has exited; this cannot."""

    def __init__(self, ds):
        self._ds = ds
        self._done = threading.Event()
        self._misses = ds.cache_metrics()["misses"]
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        # cache_metrics needs the GIL, which the read releases while it waits.
        while (not self._done.is_set()
               and self._ds.cache_metrics()["misses"] == self._misses):
            self._done.wait(0.001)
        while not self._done.is_set():
            _thread.interrupt_main(signal.SIGINT)
            self._done.wait(0.005)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._done.set()
        self._thread.join()
        # Let a signal still pending run under the test's handler, not the
        # one the test restores: the interpreter runs handlers at loop
        # back-edges.
        for _ in range(1000):
            pass


@pytest.mark.skipif(sys.version_info < (3, 10),
                    reason="interrupt_main(signum) needs Python 3.10")
def test_signal_handler_cancels_a_read_in_progress(slow_read):
    ds, item = slow_read
    token = ax.CancelToken()
    previous = signal.signal(signal.SIGINT, lambda *_: token.cancel())
    try:
        # Python runs the handler only between bytecodes; the read must give
        # it the chance, or the read finishes and returns a chunk.
        with _Interrupter(ds):
            with pytest.raises(ax.ReadCancelled):
                ds.read_leaf_chunk(item, cancel=token)
    finally:
        signal.signal(signal.SIGINT, previous)
    assert token.cancelled


@pytest.mark.skipif(sys.version_info < (3, 10),
                    reason="interrupt_main(signum) needs Python 3.10")
def test_keyboard_interrupt_stops_a_read(slow_read):
    ds, item = slow_read
    # Python's own SIGINT handler raises KeyboardInterrupt; this one does
    # the same, once, so the signals that follow cannot escape the test.
    raised = []

    def interrupt(*_):
        if not raised:
            raised.append(True)
            raise KeyboardInterrupt

    previous = signal.signal(signal.SIGINT, interrupt)
    try:
        with _Interrupter(ds):
            with pytest.raises(KeyboardInterrupt):
                ds.read_leaf_chunk(item)
    finally:
        signal.signal(signal.SIGINT, previous)
    # The interrupted read let go of its pins; the dataset still reads.
    assert ds.cache_metrics()["pinned_bytes"] == 0
    assert ds.read_leaf_chunk(item).size == 96 ** 3


def _in_forked_child(body, from_thread=False, timeout=60):
    """Runs body() in a forked child, from the main thread or from another
    one, and returns the child's exit code: 0 when body() returned true. A
    child that hangs is killed and fails the test."""
    def fork():
        with warnings.catch_warnings():
            # Python 3.12 warns about forking a process with threads.
            warnings.simplefilter("ignore", DeprecationWarning)
            pid = os.fork()
        if pid == 0:
            code = 1
            try:
                code = 0 if body() else 2
            except BaseException:
                traceback.print_exc()
            finally:
                os._exit(code)
        return pid

    if from_thread:
        pids = []
        thread = threading.Thread(target=lambda: pids.append(fork()))
        thread.start()
        thread.join()
        pid = pids[0]
    else:
        pid = fork()
    deadline = time.monotonic() + timeout
    while True:
        done, status = os.waitpid(pid, os.WNOHANG)
        if done:
            return os.waitstatus_to_exitcode(status)
        if time.monotonic() > deadline:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            pytest.fail("the forked child hung")
        time.sleep(0.01)


needs_fork = pytest.mark.skipif(not hasattr(os, "fork"),
                                reason="needs os.fork")


@needs_fork
def test_a_forked_child_opens_plans_and_reads(refined_2d):
    # multiprocessing forks by default on Linux before Python 3.14: a child
    # has none of its parent's threads, so nothing may wait on one.
    path, _ = refined_2d
    with ax.open_plotfile(path) as ds:
        plan = ds.plan_leaf_chunks(["density"])
        expected = ds.read_leaf_chunk(plan[0]).size

        def child():
            if ds.read_leaf_chunk(plan[0]).size != expected:
                return False
            with ax.open_plotfile(path) as fresh:
                return sum(c.size for c in fresh.leaf_cells(["density"])) \
                    == plan.total_cells

        assert _in_forked_child(child) == 0


@needs_fork
@pytest.mark.skipif(sys.version_info < (3, 10),
                    reason="interrupt_main(signum) needs Python 3.10")
def test_signals_reach_a_read_in_a_child_forked_from_a_thread(slow_read):
    # The forking thread is the child's main thread, so signal handlers run
    # there, and a read on it must poll for them.
    ds, item = slow_read

    def child():
        # A handler that cancels, not one that raises: KeyboardInterrupt
        # would also surface after a read that ran to the end, whereas only
        # a read that polled is stopped by the token.
        token = ax.CancelToken()
        signal.signal(signal.SIGINT, lambda *_: token.cancel())
        with _Interrupter(ds):
            try:
                ds.read_leaf_chunk(item, cancel=token)
            except ax.ReadCancelled:
                return True
        return False

    assert _in_forked_child(child, from_thread=True) == 0
