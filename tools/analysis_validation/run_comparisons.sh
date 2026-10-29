#!/usr/bin/env bash
# Run the ported field-agnostic analysis scripts (python/examples) against
# the Kangaroo scripts they replace on synthetic plotfiles, compare leaf cells
# with yt on real plotfiles, and report whether the outputs agree.
#
# Usage: tools/analysis_validation/run_comparisons.sh BUILD_DIR WORK_DIR
#
# Environment:
#   PYTHON           interpreter with numpy, yt and matplotlib (default: python3)
#   KANGAROO_DIR     Kangaroo checkout (default: ../kangaroo); skipped if absent
#   KANGAROO_PYTHON  its interpreter (default: $KANGAROO_DIR/.pixi/envs/pixi-hpx/bin/python)
#   QUOKKA_PLOTFILE  a real Quokka plotfile for the yt leaf-cell comparison;
#                    skipped if unset
#   QUOKKA_HALF_WIDTH  half width of the comparison box (default: 3e16)
#   QUOKKA_GALAXY    a real Quokka galaxy plotfile (quokka/tests/plt0000037)
#                    for a second yt leaf-cell comparison; skipped if unset
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 BUILD_DIR WORK_DIR" >&2
    exit 2
fi
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
build="$(cd "$1" && pwd)"
mkdir -p "$2"
work="$(cd "$2" && pwd)"
python="${PYTHON:-python3}"
kangaroo="${KANGAROO_DIR:-$repo/../kangaroo}"
kangaroo_python="${KANGAROO_PYTHON:-$kangaroo/.pixi/envs/pixi-hpx/bin/python}"
examples="$repo/python/examples"
export PYTHONPATH="$build/python${PYTHONPATH:+:$PYTHONPATH}"
failures=0

report() {
    if [[ "$1" == 0 ]]; then
        echo "PASS  $2"
    else
        echo "FAIL  $2"
        failures=$((failures + 1))
    fi
}

same_file() {
    cmp -s "$1" "$2"
}

have_kangaroo() {
    [[ -x "$kangaroo_python" && -d "$kangaroo/scripts" ]]
}

rm -rf "$work/plt_part" "$work/plt_nan"
"$python" "$here/make_particle_fixture.py" "$work/plt_part"
"$python" "$here/make_nan_fixture.py" "$work/plt_nan"

if have_kangaroo; then
    # --- Kangaroo: cell-average slice --------------------------------------
    # Planes in the lower half of a level-0 cell and off every face. Kangaroo
    # locates the plane on level 0 and refines that index (base_index *
    # ratio), so on finer levels it reads the lowest fine cell under the
    # coarse one, which is the right cell only there; its default plane, the
    # middle level-0 cell's center, lies on a level-1 face.
    # tests/python/test_slices.py checks the upper-half case against the
    # fixture's own values.
    for args in "--var gasDensity --coord=-2.3e21" \
                "--var gasDensity --axis x --zoom 1.5 --resolution 37,29 --coord=7.7e20" \
                "--var gasDensity --axis y --coord=1.3e21 --resolution 64,64"; do
        read -r -a options <<< "$args"
        "$python" "$examples/plotfile_slice.py" "$work/plt_part" "${options[@]}" \
            --no-plot --npz "$work/slice_ax.npz"
        (cd "$kangaroo" && "$kangaroo_python" "$here/kangaroo_slice_array.py" \
            "$work/plt_part" "$work/slice_k.npz" "${options[@]}" > /dev/null 2>&1)
        status=0
        "$python" - "$work/slice_ax.npz" "$work/slice_k.npz" <<'EOF2' || status=1
import sys
import numpy as np
a, b = np.load(sys.argv[1]), np.load(sys.argv[2])
x, y = a["slice"], b["slice"]
finite = np.isfinite(y)
ok = (x.shape == y.shape and np.array_equal(np.isnan(x), np.isnan(y))
      and np.array_equal(a["rect"], b["rect"])
      and np.all(np.abs(x[finite] - y[finite]) <= 1e-13 * np.abs(y[finite])))
sys.exit(0 if ok else 1)
EOF2
        report $status "cell-average slice vs Kangaroo, rtol 1e-13 ($args)"
    done

    # --- Kangaroo: per-FAB extrema -----------------------------------------
    for plotfile in plt_part plt_nan; do
        for args in "" "--level 1" "--level 0 --fab 1"; do
            read -r -a options <<< "$args"
            "$python" "$examples/plotfile_fab_minmax.py" "$work/$plotfile" \
                ${options[@]+"${options[@]}"} > "$work/minmax_ax.txt"
            (cd "$kangaroo" && "$kangaroo_python" scripts/plotfile_fab_minmax.py \
                "$work/$plotfile" ${options[@]+"${options[@]}"} \
                > "$work/minmax_k.txt" 2> /dev/null)
            status=0
            same_file "$work/minmax_ax.txt" "$work/minmax_k.txt" || status=1
            report $status "per-FAB extrema vs Kangaroo, identical output ($plotfile ${args:-all levels})"
        done
    done
else
    echo "SKIP  Kangaroo comparisons ($kangaroo_python not found)"
fi

# --- yt: leaf cells of a real Quokka plotfile --------------------------------
if [[ -n "${QUOKKA_PLOTFILE:-}" ]]; then
    status=0
    "$python" "$here/compare_with_yt.py" "$QUOKKA_PLOTFILE" \
        --half-width "${QUOKKA_HALF_WIDTH:-3e16}" \
        --output "$work/quokka_vs_yt.json" > /dev/null || status=$?
    if [[ $status == 0 ]]; then
        "$python" - "$work/quokka_vs_yt.json" <<'EOF' || status=1
import json, sys
r = json.load(open(sys.argv[1]))
ok = (r["levels_match"] and r["gasDensity_values_identical"]
      and r["temperature_values_identical"]
      and max(r["phase_mass_rel_difference"]) == 0.0)
sys.exit(0 if ok else 1)
EOF
    fi
    report $status "Quokka leaf cells and phase masses vs yt ($QUOKKA_PLOTFILE)"
    if have_kangaroo; then
        # Kangaroo on the real plotfile: a slice whose plane lies in the first
        # finest cell above a level-0 face (where Kangaroo's refined plane
        # index is right on every level), and the level-0 FAB extrema.
        "$python" "$examples/plotfile_slice.py" "$QUOKKA_PLOTFILE" --var gasDensity \
            --coord=1e10 --no-plot --npz "$work/q_slice_ax.npz"
        (cd "$kangaroo" && "$kangaroo_python" "$here/kangaroo_slice_array.py" \
            "$QUOKKA_PLOTFILE" "$work/q_slice_k.npz" --var gasDensity --coord=1e10 \
            > /dev/null 2>&1)
        status=0
        "$python" - "$work/q_slice_ax.npz" "$work/q_slice_k.npz" <<'EOF2' || status=1
import sys
import numpy as np
a, b = np.load(sys.argv[1]), np.load(sys.argv[2])
x, y = a["slice"], b["slice"]
f = np.isfinite(y)
ok = (x.shape == y.shape and np.array_equal(np.isfinite(x), f)
      and np.all(np.abs(x[f] - y[f]) <= 1e-12 * np.abs(y[f])))
sys.exit(0 if ok else 1)
EOF2
        report $status "Quokka cell-average slice vs Kangaroo, rtol 1e-12"
        "$python" "$examples/plotfile_fab_minmax.py" "$QUOKKA_PLOTFILE" --level 0 \
            > "$work/q_minmax_ax.txt"
        (cd "$kangaroo" && "$kangaroo_python" scripts/plotfile_fab_minmax.py \
            "$QUOKKA_PLOTFILE" --level 0 > "$work/q_minmax_k.txt" 2> /dev/null)
        status=0
        same_file "$work/q_minmax_ax.txt" "$work/q_minmax_k.txt" || status=1
        report $status "Quokka level-0 FAB extrema vs Kangaroo, identical output"
    fi
else
    echo "SKIP  real-plotfile comparisons (QUOKKA_PLOTFILE unset)"
fi

if [[ -n "${QUOKKA_GALAXY:-}" ]]; then
    galaxy="$QUOKKA_GALAXY"
    status=0
    "$python" "$here/compare_with_yt.py" "$galaxy" --half-width 3e22 \
        --temperature-field gasInternalEnergy --output "$work/real_yt.json" \
        > /dev/null 2>&1 || status=$?
    if [[ $status == 0 ]]; then
        "$python" - "$work/real_yt.json" <<'EOF2' || status=1
import json, sys
r = json.load(open(sys.argv[1]))
sys.exit(0 if r["levels_match"] and r["gasDensity_values_identical"]
         and r["gasInternalEnergy_values_identical"] else 1)
EOF2
    fi
    report $status "real galaxy leaf cells vs yt, identical values"
else
    echo "SKIP  real galaxy comparisons (QUOKKA_GALAXY unset)"
fi

if [[ $failures -ne 0 ]]; then
    echo "$failures comparison(s) failed; outputs are in $work"
    exit 1
fi
echo "all comparisons passed"
