"""Write small AMReX plotfiles for tests.

The layout is AMReX's own (HyperCLaw-V1.1 Header, one VisMF Cell_H per level,
Cell_D_* payloads), so the C++ readers see what a real simulation writes:
per-level precision, stored ghost cells, version-1 FAB headers or version-2
headerless payloads, and AMReX particle containers.

Field values come from callables of (level, i, j, k) index arrays over the
*grown* box, so a test can compute the same values independently. Stored
ghost cells get GHOST_VALUE unless the field callable is asked to fill them.
"""

from __future__ import annotations

import os
import warnings
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Sequence, Tuple

import numpy as np

GHOST_VALUE = -1.0e30

_DESCRIPTORS = {
    "float64": "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))",
    "float32": "((8, (32 8 23 0 1 9 0 127)),(4, (4 3 2 1)))",
}

Box = Tuple[Sequence[int], Sequence[int]]
FieldFunction = Callable[[int, np.ndarray, np.ndarray, np.ndarray], np.ndarray]


@dataclass
class Level:
    boxes: List[Box]
    dtype: str = "float64"
    ghost: int = 0
    # Grids per Cell_D file; AMReX groups several FABs per file.
    grids_per_file: int = 2


@dataclass
class ParticleSpecies:
    """Particles of one species, all stored on level 0 grid by grid."""

    name: str
    real_names: List[str]
    int_names: List[str]
    # Per grid: (positions (n, dim), reals (n, nreal), ints (n, nint), ids (n,))
    grids: Dict[int, Tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]]
    dtype: str = "float64"
    level: int = 0


def _box_text(lower, upper, dim, nodal=None):
    nodal = nodal or [0] * dim
    lo = ",".join(str(int(v)) for v in lower[:dim])
    hi = ",".join(str(int(v)) for v in upper[:dim])
    no = ",".join(str(int(v)) for v in nodal[:dim])
    return f"(({lo}) ({hi}) ({no}))"


def _format(value: float) -> str:
    return repr(float(value))


def write_plotfile(
    path,
    *,
    dim: int,
    fields: Dict[str, FieldFunction],
    levels: List[Level],
    domain_cells: Sequence[int],
    ref_ratio: int = 2,
    prob_lo: Sequence[float] = (0.0, 0.0, 0.0),
    prob_hi: Sequence[float] = (1.0, 1.0, 1.0),
    time: float = 0.0,
    vismf_version: int = 1,
    fill_ghosts: bool = False,
    coord_sys: int = 0,
    particles: Sequence[ParticleSpecies] = (),
    quokka_metadata: bool = False,
) -> str:
    """Write a plotfile directory and return its path."""
    path = os.fspath(path)
    os.makedirs(path, exist_ok=True)
    names = list(fields)
    prob_lo = list(prob_lo)[:dim]
    prob_hi = list(prob_hi)[:dim]
    nlevels = len(levels)
    domains = []
    dxs = []
    for lev in range(nlevels):
        cells = [int(c) * ref_ratio**lev for c in domain_cells[:dim]]
        domains.append(([0] * dim, [c - 1 for c in cells]))
        dxs.append([(prob_hi[a] - prob_lo[a]) / cells[a] for a in range(dim)])

    lines = ["HyperCLaw-V1.1", str(len(names)), *names, str(dim),
             _format(time), str(nlevels - 1),
             " ".join(_format(v) for v in prob_lo),
             " ".join(_format(v) for v in prob_hi),
             " ".join(str(ref_ratio) for _ in range(nlevels - 1)),
             " ".join(_box_text(lo, hi, dim) for lo, hi in domains),
             " ".join("0" for _ in range(nlevels))]
    for dx in dxs:
        lines.append(" ".join(_format(v) for v in dx))
    lines.append(str(coord_sys))
    lines.append("0")
    for lev, level in enumerate(levels):
        lines.append(f"{lev} {len(level.boxes)} {_format(time)}")
        lines.append("0")
        for lower, upper in level.boxes:
            for a in range(dim):
                lo = prob_lo[a] + lower[a] * dxs[lev][a]
                hi = prob_lo[a] + (upper[a] + 1) * dxs[lev][a]
                lines.append(f"{_format(lo)} {_format(hi)}")
        lines.append(f"Level_{lev}/Cell")
        _write_level(path, lev, level, dim, names, fields, vismf_version,
                     fill_ghosts)
    with open(os.path.join(path, "Header"), "w") as out:
        out.write("\n".join(lines) + "\n")
    for species in particles:
        _write_particles(path, species, dim, levels)
    if quokka_metadata:
        # What yt's Quokka frontend recognizes a Quokka plotfile by; unit 1
        # on every axis, so yt's code units are the stored cgs numbers.
        with open(os.path.join(path, "metadata.yaml"), "w") as out:
            out.write("units:\n  unit_length: 1\n  unit_mass: 1\n"
                      "  unit_time: 1\n  unit_temperature: 1\n")
    return path


def _grown(lower, upper, ghost, dim):
    lo = [int(lower[a]) - ghost for a in range(dim)]
    hi = [int(upper[a]) + ghost for a in range(dim)]
    return lo, hi


def _indices(lo, hi, dim):
    axes = [np.arange(lo[a], hi[a] + 1) for a in range(dim)]
    while len(axes) < 3:
        axes.append(np.zeros(1, dtype=np.int64))
    # Fortran order: i fastest.
    k, j, i = np.meshgrid(axes[2], axes[1], axes[0], indexing="ij")
    return i.ravel(), j.ravel(), k.ravel()


def _write_level(path, lev, level, dim, names, fields, version, fill_ghosts):
    directory = os.path.join(path, f"Level_{lev}")
    os.makedirs(directory, exist_ok=True)
    dtype = np.dtype(level.dtype).newbyteorder("<")
    descriptor = _DESCRIPTORS[level.dtype]
    offsets = []
    minima = []
    maxima = []
    files = {}
    for grid, (lower, upper) in enumerate(level.boxes):
        filename = f"Cell_D_{grid // level.grids_per_file:05d}"
        lo, hi = _grown(lower, upper, level.ghost, dim)
        i, j, k = _indices(lo, hi, dim)
        valid = np.ones(i.shape, dtype=bool)
        for a, idx in enumerate((i, j, k)[:dim]):
            valid &= (idx >= lower[a]) & (idx <= upper[a])
        payload = bytearray()
        if version == 1:
            payload += (f"FAB {descriptor}{_box_text(lo, hi, dim)} "
                        f"{len(names)}\n").encode()
        grid_min = []
        grid_max = []
        for name in names:
            values = np.asarray(fields[name](lev, i, j, k), dtype=np.float64)
            values = np.broadcast_to(values, i.shape).copy()
            if not fill_ghosts:
                values[~valid] = GHOST_VALUE
            stored = values.astype(dtype)
            # AMReX writes NaN for an all-NaN FAB's extrema; so does this.
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", RuntimeWarning)
                grid_min.append(float(np.nanmin(stored)))
                grid_max.append(float(np.nanmax(stored)))
            payload += stored.tobytes()
        buffer = files.setdefault(filename, bytearray())
        offsets.append((filename, len(buffer)))
        buffer += payload
        minima.append(grid_min)
        maxima.append(grid_max)
    for filename, buffer in files.items():
        with open(os.path.join(directory, filename), "wb") as out:
            out.write(bytes(buffer))

    header = [str(version), "1", str(len(names)), str(level.ghost),
              f"({len(level.boxes)} 0"]
    for lower, upper in level.boxes:
        header.append(_box_text(lower, upper, dim))
    header.append(")")
    header.append(str(len(level.boxes)))
    for filename, offset in offsets:
        header.append(f"FabOnDisk: {filename} {offset}")
    header.append("")
    if version == 1:
        for matrix in (minima, maxima):
            header.append(f"{len(level.boxes)},{len(names)}")
            for row in matrix:
                header.append("".join(f"{v!r}," for v in row))
            header.append("")
    else:
        header.append(descriptor)
    with open(os.path.join(directory, "Cell_H"), "w") as out:
        out.write("\n".join(header) + "\n")


def _write_particles(path, species, dim, levels):
    """An AMReX particle container as WritePlotFile writes it: Header plus
    Level_l/Particle_H and DATA_ files holding, per grid, the integer records
    (id, cpu, then int components) and then the real records (positions, then
    real components)."""
    directory = os.path.join(path, species.name)
    lev = species.level
    os.makedirs(os.path.join(directory, f"Level_{lev}"), exist_ok=True)
    nreal = len(species.real_names)
    nint = len(species.int_names)
    ngrids = len(levels[lev].boxes)
    total = sum(len(g[0]) for g in species.grids.values())
    max_id = max([int(g[3].max()) for g in species.grids.values()
                  if len(g[3])] + [0])
    # AMReX's plotfile layout: Version_Two_Dot_Zero, the identity words
    # written as (id, cpu), and the header's legacy checkpoint flag 1.
    version = ("Version_Two_Dot_Zero_double" if species.dtype == "float64"
               else "Version_Two_Dot_Zero_single")
    real_dtype = np.dtype(species.dtype).newbyteorder("<")
    header = [version, str(dim), str(nreal), *species.real_names,
              str(nint), *species.int_names, "1", str(total), str(max_id + 1),
              str(len(levels) - 1)]
    for level_index in range(len(levels)):
        header.append(str(len(levels[level_index].boxes)))
    data = bytearray()
    level_lines = []
    for level_index in range(len(levels)):
        for grid in range(len(levels[level_index].boxes)):
            entry = species.grids.get(grid) if level_index == lev else None
            count = 0 if entry is None else len(entry[0])
            if count == 0:
                if level_index == lev:
                    level_lines.append("0 0 0")
                continue
            positions, reals, ints, ids = entry
            offset = len(data)
            int_block = np.zeros((count, 2 + nint), dtype="<i4")
            int_block[:, 0] = ids
            int_block[:, 1] = 3
            if nint:
                int_block[:, 2:] = ints
            data += int_block.tobytes()
            real_block = np.zeros((count, dim + nreal), dtype=real_dtype)
            real_block[:, :dim] = positions
            if nreal:
                real_block[:, dim:] = reals
            data += real_block.tobytes()
            level_lines.append(f"0 {count} {offset}")
    for level_index in range(len(levels)):
        if level_index == lev:
            header.extend(level_lines)
        else:
            header.extend("0 0 0" for _ in levels[level_index].boxes)
    with open(os.path.join(directory, "Header"), "w") as out:
        out.write("\n".join(header) + "\n")
    with open(os.path.join(directory, f"Level_{lev}", "DATA_00000"), "wb") as out:
        out.write(bytes(data))
    for level_index in range(len(levels)):
        os.makedirs(os.path.join(directory, f"Level_{level_index}"),
                    exist_ok=True)
        with open(os.path.join(directory, f"Level_{level_index}",
                               "Particle_H"), "w") as out:
            boxes = levels[level_index].boxes
            out.write(f"({len(boxes)} 0\n")
            for lower, upper in boxes:
                out.write(_box_text(lower, upper, dim) + "\n")
            out.write(")\n")
