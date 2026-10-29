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
    ParticleChunk,
    ParticleItem,
    ParticlePlan,
    PlanItem,
    open_plotfile,
)
from ._projection import CellAverageSlice, Deposition, Projection, SampledSlice
from ._tiles import Tile, TileItem, TilePlan
from ._amrexplorer import (
    AnalysisError,
    BlockReadError,
    CacheBudgetExceeded,
    CancelToken,
    DatasetClosed,
    DerivedFieldError,
    ParticleReadError,
    PlotfileReadError,
    ReadCancelled,
    default_cache_bytes,
)

__all__ = [
    "AnalysisError",
    "BlockReadError",
    "CellAverageSlice",
    "CacheBudgetExceeded",
    "CancelToken",
    "Dataset",
    "DatasetClosed",
    "Deposition",
    "DerivedField",
    "DerivedFieldError",
    "LeafChunk",
    "LeafChunkPlan",
    "ParticleChunk",
    "ParticleItem",
    "ParticlePlan",
    "ParticleReadError",
    "PlanItem",
    "PlotfileReadError",
    "Projection",
    "ReadCancelled",
    "SampledSlice",
    "Tile",
    "TileItem",
    "TilePlan",
    "default_cache_bytes",
    "open_plotfile",
]
