"""Exact, native-unit analysis of AMReX plotfiles.

Usage is in docs/python-analysis.md. Values are the
numbers the plotfile stores -- no units are read, attached, or converted --
and leaf-cell traversal partitions the AMR hierarchy exactly: every leaf
cell appears in exactly one chunk, and no covered coarse cell appears at all.

    import amrexplorer

    with amrexplorer.open_plotfile("plt00100") as ds:
        plan = ds.plan_leaf_chunks(fields=["gasDensity", "temperature"])
        for item in plan:
            chunk = ds.read_leaf_chunk(item)
            ...
"""

from __future__ import annotations

from ._dataset import (
    Dataset,
    DerivedField,
    LeafChunk,
    LeafChunkPlan,
    PlanItem,
    open_plotfile,
)
from ._amrexplorer import (
    AnalysisError,
    BlockReadError,
    CacheBudgetExceeded,
    CancelToken,
    DatasetClosed,
    DerivedFieldError,
    PlotfileReadError,
    ReadCancelled,
    default_cache_bytes,
)

__all__ = [
    "AnalysisError",
    "BlockReadError",
    "CacheBudgetExceeded",
    "CancelToken",
    "Dataset",
    "DatasetClosed",
    "DerivedField",
    "DerivedFieldError",
    "LeafChunk",
    "LeafChunkPlan",
    "PlanItem",
    "PlotfileReadError",
    "ReadCancelled",
    "default_cache_bytes",
    "open_plotfile",
]
