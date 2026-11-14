"""The parallel and batch examples in docs/python-analysis.md, run as
scripts the way a user would run them: in a fresh interpreter, with
``__main__`` as the module, so the processes example's guard and
module-level functions are what make it work."""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

import amrexplorer as ax
from conftest import density, temperature
from plotfile_writer import Level, write_plotfile

GUIDE = Path(__file__).resolve().parents[2] / "docs" / "python-analysis.md"
SECTIONS = ("## Parallel reductions", "## Batches")


def guide_examples():
    """(heading, code) for each Python block in the parallel and batch
    sections, under its nearest heading."""
    text = GUIDE.read_text(encoding="utf-8")
    start = text.index(SECTIONS[0])
    end = text.index("\n## ", text.index(SECTIONS[1]) + 1)
    examples, heading = [], None
    for match in re.finditer(r"^(#{2,3} [^\n]+)$|^```python\n(.*?)^```$",
                             text[start:end], re.M | re.S):
        if match.group(1):
            heading = match.group(1).lstrip("# ")
        else:
            examples.append((heading, match.group(2)))
    return examples


EXAMPLES = guide_examples()


def write_fixture(path):
    levels = [
        Level([((0, 0, 0), (7, 7, 7)), ((8, 0, 0), (15, 15, 15)),
               ((0, 8, 0), (7, 15, 15)), ((0, 0, 8), (7, 7, 15))]),
        Level([((4, 4, 4), (11, 11, 11))], dtype="float32"),
    ]
    return write_plotfile(path, dim=3, domain_cells=(16, 16, 16),
                          fields={"density": density,
                                  "temperature": temperature},
                          levels=levels, time=1.5)


def expected_mass(path):
    with ax.open_plotfile(path) as ds:
        return sum(float(np.sum(c["density"] * c.cell_volume))
                   for c in ds.leaf_cells(["density"]))


def test_the_guide_has_the_examples():
    headings = [heading for heading, _ in EXAMPLES]
    assert headings == ["Parallel reductions", "Processes", "MPI", "Batches"]


@pytest.mark.parametrize("heading,code", EXAMPLES,
                         ids=[heading for heading, _ in EXAMPLES])
def test_guide_example_runs(tmp_path, heading, code):
    if "mpi4py" in code:
        pytest.importorskip("mpi4py")
    plotfile = write_fixture(tmp_path / "plt00100")
    for step in ("00100", "00200"):
        write_fixture(tmp_path / "outputs" / f"plt{step}")
    script = tmp_path / "example.py"
    script.write_text(code, encoding="utf-8")
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join(
        [str(Path(ax.__file__).parents[1])]
        + [p for p in env.get("PYTHONPATH", "").split(os.pathsep) if p])
    result = subprocess.run([sys.executable, str(script), str(plotfile)],
                            cwd=tmp_path, env=env, capture_output=True,
                            text=True, timeout=120)
    assert result.returncode == 0, result.stderr

    mass = expected_mass(plotfile)
    if heading == "Batches":
        assert result.stdout == ""
        outputs = sorted((tmp_path / "outputs").glob("*_mass.json"))
        assert [p.name for p in outputs] == ["plt00100_mass.json",
                                             "plt00200_mass.json"]
        for output in outputs:
            assert json.loads(output.read_text()) == pytest.approx(
                {"time": 1.5, "mass": mass})
        # A rerun finds every output written and skips them all.
        stamps = [p.stat().st_mtime_ns for p in outputs]
        rerun = subprocess.run([sys.executable, str(script)], cwd=tmp_path,
                               env=env, capture_output=True, text=True,
                               timeout=120)
        assert rerun.returncode == 0, rerun.stderr
        assert [p.stat().st_mtime_ns for p in outputs] == stamps
    else:
        printed = result.stdout.split()[-1]
        assert float(printed) == pytest.approx(mass, rel=1e-12)
