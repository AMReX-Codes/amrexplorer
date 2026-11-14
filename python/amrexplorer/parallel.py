"""Parallel map-reduce over plan items.

Each work item -- one grid's leaf cells -- is read and mapped to a partial
result by a worker, and the partials are combined with a reduction. Three things are fixed by design:

* Ownership. By default every worker opens its own dataset handle on the same
  plotfile, definitions and cache budget, and plans the same request itself.
  PlotfileDataset serializes block reads per handle, so a shared handle
  serializes every miss; worker-local handles read in parallel
  (tools/analysis_validation/benchmark_parallel.py measures both). Each item
  is read once, so a worker's cache would never be hit again: a worker drops
  its unpinned blocks after every item rather than keep up to ``cache_bytes``
  resident. ``ownership="shared"`` keeps one handle, and one cache, for every
  thread instead; that handle may be the caller's, so it is never cleared.
* Scheduling. At most ``max_in_flight`` items are read or mapped at once.
  Resident memory is bounded by that many items' blocks and chunks, plus the
  partials waiting to be combined, plus whatever the caller's own handle
  already caches when one is passed in. A handle opened for a path is cleared
  like a worker's. Items are handed out dynamically, in plan order.
* Workers. The default is ``AMREXPLORER_WORKERS`` if set, otherwise the CPUs
  this process may run on, at most 8; pass ``workers=`` to override either.
* Reduction. With ``ordered=True`` (the default) partials are combined in
  item order, ``reduce(reduce(initial, p0), p1), ...``, exactly as a serial
  loop would, so floating-point results are bit-for-bit identical to the
  serial executor whatever the worker count. ``ordered=False`` combines
  partials as they arrive: faster to drain, but floating-point sums may then
  differ in the last bits from run to run.

A plan is deterministic, so a worker that plans the same request on its own
handle gets the same items with the same indices; the executor checks that
the item count and cell total agree before using it.

For MPI, plan on every rank, pass ``items=plan.partition(rank, size)``, gather
the rank results and fold them in rank order. ``partition`` is round-robin, so
that fold groups partials differently from a serial loop: the result is the
same from run to run for a given number of ranks, but floating-point sums may
differ from the serial result in the last bits.
"""

from __future__ import annotations

import concurrent.futures as futures
import multiprocessing
import os
import threading
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable, List, Optional, Sequence

from . import _amrexplorer as _native
from ._dataset import Dataset, open_plotfile

_NOTHING = object()

#: Largest default worker count; more needs ``workers=`` or the variable.
DEFAULT_MAX_WORKERS = 8
WORKERS_VARIABLE = "AMREXPLORER_WORKERS"


def _available_cpus() -> int:
    if hasattr(os, "process_cpu_count"):  # Python 3.13+
        return os.process_cpu_count() or 1
    if hasattr(os, "sched_getaffinity"):  # Linux
        return len(os.sched_getaffinity(0)) or 1
    return os.cpu_count() or 1


def default_workers() -> int:
    """The worker count map_reduce uses when ``workers`` is not given:
    ``AMREXPLORER_WORKERS`` if set, otherwise the CPUs this process may run
    on, at most DEFAULT_MAX_WORKERS."""
    value = os.environ.get(WORKERS_VARIABLE, "").strip()
    if value:
        try:
            workers = int(value)
        except ValueError:
            workers = 0
        if workers < 1:
            raise _native.AnalysisError(
                f"{WORKERS_VARIABLE} must be a positive integer, not {value!r}")
        return workers
    return min(DEFAULT_MAX_WORKERS, _available_cpus())


@dataclass(frozen=True)
class LeafCells:
    """A leaf-cell request, planned identically on every worker."""

    fields: Sequence[str] = ()
    region: Any = None
    dtype: Any = None

    def plan(self, ds: Dataset):
        return ds.plan_leaf_chunks(list(self.fields), region=self.region,
                                   dtype=self.dtype)

    @staticmethod
    def total(plan) -> int:
        return plan.total_cells

    @staticmethod
    def read(ds: Dataset, item, cancel=None):
        return ds.read_leaf_chunk(item, cancel=cancel)


@dataclass(frozen=True)
class _Source:
    """How a worker opens its own handle on the dataset."""

    path: str
    derived: tuple
    cache_bytes: Optional[int]

    def open(self) -> Dataset:
        return open_plotfile(self.path, derived=self.derived,
                             cache_bytes=self.cache_bytes)


def _source_of(dataset, derived, cache_bytes) -> _Source:
    if isinstance(dataset, Dataset):
        return _Source(dataset.path, tuple(dataset._definitions),
                       dataset.cache_bytes if cache_bytes is None else cache_bytes)
    return _Source(os.fspath(dataset), tuple(derived or ()), cache_bytes)


def _check_plan(reference, plan, request) -> None:
    if (len(plan) != len(reference)
            or request.total(plan) != request.total(reference)):
        raise _native.AnalysisError(
            "a worker's plan does not match the coordinator's; the plotfile "
            "may have changed while the job ran")


class _Combiner:
    """Combines partials, in item order when ordered."""

    def __init__(self, reduce, initial, ordered, positions):
        self._reduce = reduce
        self._value = initial
        self._ordered = ordered
        # Plan positions in the order they must be combined.
        self._order = list(positions)
        self._next = 0
        self._waiting = {}

    def _combine(self, partial):
        if self._value is _NOTHING:
            self._value = partial
        else:
            self._value = self._reduce(self._value, partial)

    def add(self, position, partial):
        if not self._ordered:
            self._combine(partial)
            return
        self._waiting[position] = partial
        while (self._next < len(self._order)
               and self._order[self._next] in self._waiting):
            self._combine(self._waiting.pop(self._order[self._next]))
            self._next += 1

    @property
    def pending(self) -> int:
        return len(self._waiting)

    def result(self):
        if self._value is _NOTHING:
            return None
        return self._value


# Process workers keep their handle and plan in module state, set up once by
# the pool initializer.
_process_state = {}


def _process_init(source, request, expected):
    ds = source.open()
    plan = request.plan(ds)
    if (len(plan), request.total(plan)) != expected:
        raise _native.AnalysisError(
            "a worker's plan does not match the coordinator's")
    _process_state.update(ds=ds, plan=plan, request=request)


def _process_run(mapper, position):
    state = _process_state
    ds = state["ds"]
    try:
        chunk = state["request"].read(ds, state["plan"][position])
    finally:
        ds.clear_unpinned_cache()
    return position, mapper(chunk)


def map_reduce(dataset, request, mapper: Callable, reduce: Callable,
               initial=_NOTHING, *, executor: str = "threads",
               workers: Optional[int] = None,
               max_in_flight: Optional[int] = None,
               ordered: bool = True, ownership: str = "worker",
               items: Optional[Iterable[int]] = None,
               derived=(), cache_bytes: Optional[int] = None,
               progress: Optional[Callable[[int, int], None]] = None,
               cancel=None, mp_context: Optional[str] = None):
    """Map every planned item to a partial result and reduce the partials.

    dataset: an open Dataset, or a plotfile path (then `derived` and
        `cache_bytes` say how to open it).
    request: LeafCells(...).
    mapper: chunk -> partial. Must be picklable for executor="processes".
    reduce: (accumulated, partial) -> accumulated.
    initial: the starting value; omitted, the first partial starts.
    executor: "serial", "threads", or "processes".
    workers: worker count (default: default_workers(), which reads
        AMREXPLORER_WORKERS or else uses the available CPUs, at most 8).
    max_in_flight: items read or mapped at once (default: 2 * workers).
    ordered: combine in item order, reproducing the serial result exactly.
    ownership: "worker" (a handle per worker, cleared after each item) or
        "shared" (threads only: one handle and cache; less memory, slower).
    items: plan positions to process (default: all), e.g. a rank's
        plan.partition(rank, size) as [item.index for item in ...].
    progress: called as progress(done, total) after each item completes.
    cancel: a CancelToken; once cancelled, no further item starts, reads in
        progress raise ReadCancelled (threads and serial), and the call
        raises ReadCancelled.
    """
    if executor not in ("serial", "threads", "processes"):
        raise _native.AnalysisError(
            "executor must be 'serial', 'threads', or 'processes'")
    if ownership not in ("worker", "shared"):
        raise _native.AnalysisError("ownership must be 'worker' or 'shared'")
    if ownership == "shared" and executor == "processes":
        raise _native.AnalysisError(
            "processes cannot share a dataset handle; use ownership='worker'")
    if workers is None:
        workers = default_workers()
    elif int(workers) < 1:
        raise _native.AnalysisError("workers must be at least 1")
    workers = int(workers)
    max_in_flight = max(1, int(max_in_flight or 2 * workers))

    source = _source_of(dataset, derived, cache_bytes)
    owned = None
    if isinstance(dataset, Dataset):
        coordinator = dataset
    else:
        coordinator = owned = source.open()
    try:
        plan = request.plan(coordinator)
        positions = list(range(len(plan))) if items is None else [
            int(getattr(i, "index", i)) for i in items]
        if any(not 0 <= p < len(plan) for p in positions):
            raise _native.AnalysisError("an item position is outside the plan")
        combiner = _Combiner(reduce, initial, ordered, positions)
        total = len(positions)

        if executor == "serial":
            for done, position in enumerate(positions, start=1):
                _check_cancel(cancel)
                try:
                    chunk = request.read(coordinator, plan[position], cancel)
                finally:
                    if owned is not None:
                        owned.clear_unpinned_cache()
                combiner.add(position, mapper(chunk))
                if progress:
                    progress(done, total)
            return combiner.result()

        if executor == "threads":
            return _run_threads(coordinator, plan, request, source, mapper,
                                combiner, positions, workers, max_in_flight,
                                ownership, progress, cancel)
        return _run_processes(plan, request, source, mapper, combiner,
                              positions, workers, max_in_flight, progress,
                              mp_context, cancel)
    finally:
        if owned is not None:
            owned.close()


def _check_cancel(cancel):
    if cancel is not None and cancel.cancelled:
        raise _native.ReadCancelled("read cancelled")


def _run_threads(coordinator, plan, request, source, mapper, combiner,
                 positions, workers, max_in_flight, ownership, progress,
                 cancel):
    local = threading.local()
    handles = []
    handles_lock = threading.Lock()

    def worker_plan():
        if ownership == "shared":
            return coordinator, plan
        if not hasattr(local, "ds"):
            ds = source.open()
            with handles_lock:
                handles.append(ds)
            worker = request.plan(ds)
            _check_plan(plan, worker, request)
            local.ds, local.plan = ds, worker
        return local.ds, local.plan

    def run(position):
        _check_cancel(cancel)
        ds, worker = worker_plan()
        try:
            chunk = request.read(ds, worker[position], cancel)
        finally:
            if ownership == "worker":
                ds.clear_unpinned_cache()
        return position, mapper(chunk)

    try:
        with futures.ThreadPoolExecutor(max_workers=workers) as pool:
            _drain(pool, run, combiner, positions, max_in_flight, progress,
                   cancel=cancel)
    finally:
        for ds in handles:
            ds.close()
    return combiner.result()


def _run_processes(plan, request, source, mapper, combiner, positions,
                   workers, max_in_flight, progress, mp_context, cancel):
    context = multiprocessing.get_context(mp_context) if mp_context else None
    expected = (len(plan), request.total(plan))
    with futures.ProcessPoolExecutor(
            max_workers=workers, mp_context=context,
            initializer=_process_init,
            initargs=(source, request, expected)) as pool:
        _drain(pool, lambda p: pool.submit(_process_run, mapper, p),
               combiner, positions, max_in_flight, progress, submit=False,
               cancel=cancel)
    return combiner.result()


def _drain(pool, run, combiner, positions, max_in_flight, progress,
           submit=True, cancel=None):
    """Keep at most max_in_flight items running or waiting to be combined,
    handing them out in plan order."""
    pending = set()
    upcoming = iter(positions)
    done = 0
    total = len(positions)
    exhausted = False
    try:
        while True:
            _check_cancel(cancel)
            while (not exhausted
                   and len(pending) + combiner.pending < max_in_flight):
                position = next(upcoming, None)
                if position is None:
                    exhausted = True
                    break
                pending.add(pool.submit(run, position) if submit else run(position))
            if not pending:
                break
            finished, pending = futures.wait(
                pending, return_when=futures.FIRST_COMPLETED)
            for future in finished:
                position, partial = future.result()
                combiner.add(position, partial)
                done += 1
                if progress:
                    progress(done, total)
    except BaseException:
        for future in pending:
            future.cancel()
        raise


__all__ = ["DEFAULT_MAX_WORKERS", "LeafCells", "default_workers",
           "map_reduce"]
