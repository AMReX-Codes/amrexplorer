"""Batch helpers for plotfile sequences.

    from amrexplorer import sequence

    for ds in sequence.open_each(sequence.find_plotfiles("outputs/")):
        out = sequence.output_path(ds.path, "_projection_z.png")
        if sequence.needs_output(out):
            ...
"""

from __future__ import annotations

import glob
import os
import re
from pathlib import Path
from typing import Iterable, Iterator, List, Optional, Union

from ._dataset import Dataset, open_plotfile

PathLike = Union[str, os.PathLike]

_TRAILING_NUMBER = re.compile(r"(\d+)$")


def is_plotfile(path: PathLike) -> bool:
    """A plotfile is a directory with a Header."""
    return (Path(path) / "Header").is_file()


def _step(path: Path):
    match = _TRAILING_NUMBER.search(path.name)
    return (0, int(match.group(1)), path.name) if match else (1, 0, path.name)


def find_plotfiles(source: Union[PathLike, Iterable[PathLike]],
                   prefix: Optional[str] = "plt") -> List[str]:
    """Plotfiles in order of their trailing step number.

    source: a plotfile (returned alone), a directory (its plotfile
        subdirectories whose names start with `prefix`; None for any name),
        a glob pattern, or an iterable of any of these.
    Paths that are not plotfiles, such as a script's .png outputs beside
    them, are skipped.
    """
    if not isinstance(source, (str, os.PathLike)):
        found = []
        for entry in source:
            found.extend(find_plotfiles(entry, prefix))
        return sorted(dict.fromkeys(found), key=lambda p: _step(Path(p)))
    path = Path(source)
    if is_plotfile(path):
        return [str(path)]
    if path.is_dir():
        candidates = [entry for entry in path.iterdir()
                      if prefix is None or entry.name.startswith(prefix)]
    else:
        candidates = [Path(entry) for entry in glob.glob(str(source))]
    return [str(entry) for entry in sorted(
        (c for c in candidates if is_plotfile(c)), key=_step)]


def open_each(paths: Iterable[PathLike], derived=(),
              cache_bytes: Optional[int] = None) -> Iterator[Dataset]:
    """Open each plotfile in turn, closing it before the next opens, so a
    long sequence holds one block cache at a time."""
    for path in paths:
        ds = open_plotfile(path, derived=derived, cache_bytes=cache_bytes)
        try:
            yield ds
        finally:
            ds.close()


def output_path(plotfile: PathLike, suffix: str,
                directory: Optional[PathLike] = None) -> Path:
    """`<plotfile name><suffix>` beside the plotfile, or in `directory`."""
    plotfile = Path(plotfile)
    base = Path(directory) if directory is not None else plotfile.parent
    return base / f"{plotfile.name}{suffix}"


def needs_output(path: PathLike, overwrite: bool = False) -> bool:
    """Whether a frame still has to be written: it does not exist yet, or
    the caller asked to overwrite."""
    return overwrite or not Path(path).exists()


__all__ = ["find_plotfiles", "is_plotfile", "needs_output", "open_each",
           "output_path"]
