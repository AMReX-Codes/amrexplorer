#!/usr/bin/env python3
"""Benchmark leaf-cell reads: serial, threads sharing one dataset handle,
threads with a handle each, and processes, on the same plan.

Each configuration opens fresh handles, so the block cache starts empty; the
operating system's page cache is warmed by an untimed serial pass first, so
the numbers compare decode-and-copy throughput and lock contention rather
than disk speed.

    python benchmark_parallel.py PLOTFILE --half-width 3e17 --fields gasDensity temperature
"""

import argparse
import json
import time

import numpy as np

import amrexplorer
from amrexplorer import parallel


def mass(chunk):
    return float(np.sum(chunk["gasDensity"] * chunk.cell_volume))


def add(a, b):
    return a + b


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("plotfile")
    parser.add_argument("--half-width", type=float, default=None)
    parser.add_argument("--fields", nargs="+", default=["gasDensity"])
    parser.add_argument("--workers", type=int, nargs="+", default=[2, 4, 8])
    parser.add_argument("--cache-mb", type=int, default=2048)
    args = parser.parse_args()
    region = None
    if args.half_width:
        region = ([-args.half_width] * 3, [args.half_width] * 3)
    request = parallel.LeafCells(args.fields, region=region)
    cache = args.cache_mb * 1024 * 1024
    with amrexplorer.open_plotfile(args.plotfile, cache_bytes=cache) as ds:
        plan = request.plan(ds)
        print(f"{len(plan)} items, {plan.total_cells} leaf cells", flush=True)

    def run(executor, workers, ownership):
        start = time.perf_counter()
        value = parallel.map_reduce(args.plotfile, request, mass, add, 0.0,
                                    executor=executor, workers=workers,
                                    ownership=ownership, cache_bytes=cache)
        return time.perf_counter() - start, value

    run("serial", 1, "worker")  # warm the page cache
    results = []
    seconds, reference = run("serial", 1, "worker")
    results.append(("serial", 1, "worker", seconds))
    for workers in args.workers:
        for executor, ownership in (("threads", "shared"), ("threads", "worker"),
                                    ("processes", "worker")):
            seconds, value = run(executor, workers, ownership)
            assert value == reference, (executor, workers, ownership)
            results.append((executor, workers, ownership, seconds))
    serial = results[0][3]
    for executor, workers, ownership, seconds in results:
        print(f"{executor:9s} workers={workers:2d} ownership={ownership:6s} "
              f"{seconds:8.3f} s  speedup {serial / seconds:5.2f}  "
              f"{plan.total_cells / seconds / 1e6:7.2f} Mcell/s", flush=True)
    print(json.dumps([dict(zip(("executor", "workers", "ownership", "seconds"), r))
                      for r in results]))


if __name__ == "__main__":
    main()
