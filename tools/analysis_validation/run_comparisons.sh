#!/usr/bin/env bash
# Run the ported per-FAB extrema script (python/examples) against the Kangaroo
# script it replaces on synthetic plotfiles, compare leaf cells with yt on
# real plotfiles, and report whether the outputs agree.
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

rm -rf "$work/plt_amr" "$work/plt_nan"
"$python" "$here/make_amr_fixture.py" "$work/plt_amr"
"$python" "$here/make_nan_fixture.py" "$work/plt_nan"

if have_kangaroo; then
    # --- Kangaroo: per-FAB extrema -----------------------------------------
    for plotfile in plt_amr plt_nan; do
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
        # Kangaroo on the real plotfile: the level-0 FAB extrema.
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
