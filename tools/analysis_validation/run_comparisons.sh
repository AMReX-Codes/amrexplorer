#!/usr/bin/env bash
# Run the ported analysis scripts (python/examples) against the originals they
# replace -- Kangaroo's plotfile_* scripts and the yt scripts in
# incite-2026/postprocessing -- on synthetic plotfiles, and report whether the
# outputs agree. Results are summarized in docs/analysis-validation.md.
#
# Usage: tools/analysis_validation/run_comparisons.sh BUILD_DIR WORK_DIR
#
# Environment:
#   PYTHON           interpreter with numpy, yt and matplotlib (default: python3)
#   KANGAROO_DIR     Kangaroo checkout (default: ../kangaroo); skipped if absent
#   KANGAROO_PYTHON  its interpreter (default: $KANGAROO_DIR/.pixi/envs/pixi-hpx/bin/python)
#   INCITE_DIR       incite-2026 checkout (default: ../incite-2026); skipped if absent
#   QUOKKA_PLOTFILE  a real Quokka plotfile for the yt leaf-cell comparison;
#                    skipped if unset
#   QUOKKA_HALF_WIDTH  half width of the comparison box (default: 3e16)
#   QUOKKA_GALAXY    a real Quokka galaxy plotfile with MHD fields, a passive
#                    scalar, the stellar birth-mass density and CIC particles
#                    (quokka/tests/plt0000037); skipped if unset
#   QUOKKA_DISK      a real Quokka disk galaxy with a temperature and a
#                    potential (amrex-visit-plugin/example_data/DiskGalaxy/
#                    plt0000020); skipped if unset
#   QUOKKA_STARS     a real Quokka plotfile with StochasticStellarPop
#                    particles (quokka/tests/plt0000010); skipped if unset
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
incite="${INCITE_DIR:-$repo/../incite-2026}"
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

have_incite() {
    [[ -d "$incite/postprocessing" ]] && "$python" -c "import yt" 2>/dev/null
}

rm -rf "$work/plt_sfr" "$work/plt_part" "$work/plt_nan"
"$python" "$here/make_sfr_fixture.py" "$work/plt_sfr"
"$python" "$here/make_particle_fixture.py" "$work/plt_part"
"$python" "$here/make_nan_fixture.py" "$work/plt_nan"
rm -rf "$work/plt_mhd" "$work/plt_mhd1" "$work/plt_mhd1b"
"$python" "$here/make_mhd_fixture.py" "$work/plt_mhd"
"$python" "$here/make_mhd_fixture.py" "$work/plt_mhd1" --levels 1
"$python" "$here/make_mhd_fixture.py" "$work/plt_mhd1b" --single-grid

# --- Kangaroo: enclosed SFR -------------------------------------------------
if have_kangaroo; then
    sfr_args=(--rmax-kpc 8 --zmax-kpc 8 --nbins 20)
    (cd "$work" && "$python" "$examples/plotfile_enclosed_sfr.py" plt_sfr \
        "${sfr_args[@]}" --output-json ax_disk.json \
        --output-json-wind ax_wind.json --output-json-joint ax_joint.json \
        > /dev/null 2>&1)
    (cd "$kangaroo" && "$kangaroo_python" scripts/plotfile_enclosed_sfr.py \
        "$work/plt_sfr" "${sfr_args[@]}" --output-json "$work/k_disk.json" \
        --output-json-wind "$work/k_wind.json" \
        --output-json-joint "$work/k_joint.json" > /dev/null 2>&1)
    for kind in disk wind joint; do
        status=0
        "$python" "$here/compare_json.py" "$work/ax_$kind.json" \
            "$work/k_$kind.json" --rtol 1e-12 > "$work/sfr_$kind.diff" \
            || status=$?
        report $status "enclosed SFR ($kind JSON) vs Kangaroo, rtol 1e-12"
    done

    # --- Kangaroo: particle mass histogram -----------------------------------
    index=0
    for args in "--bins 32 --output OUT.csv --no-plot" \
                "--bins 20 --range 1,3.2 --log-range --density --output OUT.csv --no-plot" \
                "--topk 6" \
                "--bins 16 --output OUT.npz --no-plot"; do
        index=$((index + 1))
        read -r -a ours <<< "${args//OUT/$work/ax_hist$index}"
        read -r -a theirs <<< "${args//OUT/$work/k_hist$index}"
        "$python" "$examples/plotfile_particle_mass_histogram.py" \
            "$work/plt_part" "${ours[@]}" > "$work/ax_hist$index.txt"
        (cd "$kangaroo" && "$kangaroo_python" \
            scripts/plotfile_particle_mass_histogram.py "$work/plt_part" \
            "${theirs[@]}" > "$work/k_hist$index.txt" 2> /dev/null)
        status=0
        same_file "$work/ax_hist$index.txt" "$work/k_hist$index.txt" || status=1
        if [[ -f "$work/ax_hist$index.csv" ]]; then
            same_file "$work/ax_hist$index.csv" "$work/k_hist$index.csv" \
                || status=1
        fi
        if [[ -f "$work/ax_hist$index.npz" ]]; then
            "$python" - "$work/ax_hist$index.npz" "$work/k_hist$index.npz" <<'EOF' || status=1
import sys
import numpy as np
a, b = np.load(sys.argv[1]), np.load(sys.argv[2])
sys.exit(0 if all(np.array_equal(a[k], b[k]) for k in ("edges", "counts")) else 1)
EOF
        fi
        report $status "particle mass histogram vs Kangaroo, identical output ($args)"
    done

    # --- Kangaroo: projection ----------------------------------------------
    for args in "--var gasDensity" \
                "--var gasDensity --axis x --zoom 2 --resolution 40,24" \
                "--var gasDensity --axis y --axis-bounds=-2e21,9e21 --resolution 13,17"; do
        read -r -a options <<< "$args"
        "$python" "$examples/plotfile_projection.py" "$work/plt_part" \
            "${options[@]}" --output "$work/proj_ax.png" --npz "$work/proj_ax.npz"
        (cd "$kangaroo" && "$kangaroo_python" \
            "$here/kangaroo_projection_array.py" "$work/plt_part" \
            "$work/proj_k.npz" "${options[@]}" > /dev/null 2>&1)
        status=0
        "$python" - "$work/proj_ax.npz" "$work/proj_k.npz" <<'EOF2' || status=1
import sys
import numpy as np
a, b = np.load(sys.argv[1]), np.load(sys.argv[2])
x, y = a["surface_density_msun_pc2"], b["surface_density_msun_pc2"]
ok = (x.shape == y.shape and np.array_equal(a["rect"], b["rect"])
      and np.all(np.abs(x - y) <= 1e-13 * np.abs(y)))
sys.exit(0 if ok else 1)
EOF2
        report $status "projection vs Kangaroo, rtol 1e-13 ($args)"
    done

    # --- Kangaroo: cell-average slice --------------------------------------
    # Planes in the lower half of a level-0 cell and off every face. Kangaroo
    # locates the plane on level 0 and refines that index (base_index *
    # ratio), so on finer levels it reads the lowest fine cell under the
    # coarse one, which is the right cell only there; its default plane, the
    # middle level-0 cell's center, lies on a level-1 face. See
    # docs/analysis-validation.md; tests/python/test_slices.py checks the
    # upper-half case against the fixture's own values.
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
    for plotfile in plt_part plt_sfr plt_nan; do
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

    # --- Kangaroo: spherical flux surfaces -----------------------------------
    for args in "--radius-kpc 3.3" \
                "--rmin-kpc 0.5 --rmax-kpc 12 --nbins 24" \
                "--rmin-kpc 0.5 --rmax-kpc 12" \
                "--radius-kpc 4.1 --temperature-bins 1e3,1e4 1e5,1e6" \
                "--rmin-kpc 1 --rmax-kpc 9 --nbins 7 --temperature-bins 100 1e4 9998872.55104388" \
                "--radius 6.2e21 --gamma 1.4" \
                "--radius-kpc 2" "--radius-kpc 4 --temperature-bins 100 1e4 1e7" \
                "--radius-kpc 8"; do
        read -r -a options <<< "$args"
        "$python" "$examples/plotfile_flux_surface.py" "$work/plt_mhd" \
            "${options[@]}" --output-json "$work/sphere_ax.json" > /dev/null
        (cd "$kangaroo" && "$kangaroo_python" scripts/plotfile_flux_surface.py \
            "$work/plt_mhd" "${options[@]}" --output-json "$work/sphere_k.json" \
            > /dev/null 2>&1)
        status=0
        "$python" "$here/compare_json.py" "$work/sphere_ax.json" \
            "$work/sphere_k.json" --rtol 1e-12 > "$work/sphere.diff" || status=$?
        report $status "spherical flux surface vs Kangaroo, rtol 1e-12 ($args)"
    done

    # --- Kangaroo: cylindrical flux surfaces ---------------------------------
    # Not --height-kpc 2: Kangaroo's block culling drops the level-2 caps
    # there (an unfused block bound one ulp below the height); see
    # docs/analysis-validation.md.
    for args in "--radius-kpc 3 --height-kpc 1.2" \
                "--rmin-kpc 0.3 --rmax-kpc 7.5 --nbins 24 --height-kpc 1.2" \
                "--radius-kpc 3 --zmin-kpc 0.1 --zmax-kpc 7.5 --nbins 24" \
                "--radius-kpc 4 --zmin-kpc 0.2 --zmax-kpc 6" \
                "--radius-kpc 2.5 --height-kpc 0.8 --temperature-bins 1e3 1e4,1e5 1e6" \
                "--radius-kpc 1.7 --zmin-kpc 0.3 --zmax-kpc 3 --nbins 6 --temperature-bins 1e2 1e3 1e4 1e5 1e6 1e7" \
                "--radius-kpc 3 --height 3.085677581491369e+21" \
                "--radius-kpc 2.2 --height 1.5428387907456844e+21" \
                "--radius-kpc 3 --height-kpc 0.5" \
                "--radius 3.085677581491369e+21 --height-kpc 1.3" \
                "--radius-kpc 3 --height 7.714193953728418e21"; do
        read -r -a options <<< "$args"
        "$python" "$examples/plotfile_cylindrical_flux_surface.py" "$work/plt_mhd" \
            "${options[@]}" --output-json "$work/cylinder_ax.json" > /dev/null
        (cd "$kangaroo" && "$kangaroo_python" \
            scripts/plotfile_cylindrical_flux_surface.py "$work/plt_mhd" \
            "${options[@]}" --output-json "$work/cylinder_k.json" > /dev/null 2>&1)
        status=0
        "$python" "$here/compare_json.py" "$work/cylinder_ax.json" \
            "$work/cylinder_k.json" --rtol 1e-12 > "$work/cylinder.diff" || status=$?
        report $status "cylindrical flux surface vs Kangaroo, rtol 1e-12 ($args)"
    done
    # The --height-kpc 2 disagreement is Kangaroo's: with the height one ulp
    # lower its culling keeps the level-2 block and the two agree.
    "$python" "$examples/plotfile_cylindrical_flux_surface.py" "$work/plt_mhd" \
        --radius-kpc 3 --height 6.171355162982735e21 \
        --output-json "$work/cylinder_ax.json" > /dev/null
    (cd "$kangaroo" && "$kangaroo_python" \
        scripts/plotfile_cylindrical_flux_surface.py "$work/plt_mhd" \
        --radius-kpc 3 --height 6.171355162982734e21 \
        --output-json "$work/cylinder_k.json" > /dev/null 2>&1)
    status=0
    "$python" - "$work/cylinder_ax.json" "$work/cylinder_k.json" <<'EOF2' || status=1
import json, sys
a, b = (json.load(open(path)) for path in sys.argv[1:])
def flat(value, out):
    if isinstance(value, dict):
        for key in sorted(value):
            if key not in ("plotfile", "height", "height_kpc", "heights", "heights_kpc"):
                flat(value[key], out)
    elif isinstance(value, list):
        for item in value:
            flat(item, out)
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        out.append(float(value))
    return out
x, y = flat(a, []), flat(b, [])
ok = len(x) == len(y) and all(abs(p - q) <= 1e-12 * max(abs(p), abs(q), 1e-300)
                              for p, q in zip(x, y))
sys.exit(0 if ok else 1)
EOF2
    report $status "cylindrical flux surface at h = 2 kpc vs Kangaroo one ulp lower, rtol 1e-12"

    # --- Kangaroo: stellar CIC projection ------------------------------------
    # Exact agreement only away from block, coarse-fine and domain
    # boundaries, where Kangaroo drops weight; compare_cic.py checks that and
    # that the conservative deposit keeps all the mass.
    for case in "z|" "z|--resolution 64,64 --mass-max 60" \
                "x|--axis x --zoom 1.5 --resolution 40,40"; do
        axis="${case%%|*}"
        args="${case#*|}"
        read -r -a options <<< "$args"
        "$python" "$examples/plotfile_projection_cic_stellar.py" "$work/plt_part" \
            ${options[@]+"${options[@]}"} --output "$work/cic_ax.png" \
            --npz "$work/cic_ax.npz" 2> /dev/null
        (cd "$kangaroo" && "$kangaroo_python" "$here/kangaroo_cic_array.py" \
            "$work/plt_part" "$work/cic_k.npz" ${options[@]+"${options[@]}"} \
            > /dev/null 2>&1)
        status=0
        "$python" "$here/compare_cic.py" "$work/plt_part" "$work/cic_ax.npz" \
            "$work/cic_k.npz" --axis "$axis" > "$work/cic.txt" || status=$?
        report $status "stellar CIC projection vs Kangaroo away from boundaries, rtol 1e-12 (${args:-defaults}): $(cat "$work/cic.txt")"
    done

    # --- Kangaroo: Toomre Q --------------------------------------------------
    # Every column but the gravity-derived ones: Kangaroo's potential
    # gradient is a different stencil (see docs/analysis-validation.md).
    rm -rf "$work/toomre_ax" "$work/toomre_k"
    toomre=(--r-min-kpc 0.5 --r-max-kpc 7 --bins 13)
    "$python" "$examples/plotfile_toomre_q.py" "$work/plt_mhd" "${toomre[@]}" \
        --output-dir "$work/toomre_ax" > /dev/null 2>&1
    (cd "$kangaroo" && "$kangaroo_python" scripts/plotfile_toomre_q.py \
        "$work/plt_mhd" "${toomre[@]}" --output-dir "$work/toomre_k" > /dev/null 2>&1)
    status=0
    "$python" - "$work/toomre_ax/plt_mhd_toomre_q.csv" \
        "$work/toomre_k/plt_mhd_toomre_q.csv" <<'EOF2' || status=1
import csv, sys
import numpy as np
gravity = {"q_thermal_magnetic", "q_thermal_turbulent", "q_thermal_turbulent_magnetic",
           "radial_gravity_cm_s2", "circular_speed_km_s", "omega_myr_inv",
           "kappa_myr_inv", "valid"}
a = list(csv.DictReader(open(sys.argv[1])))
b = list(csv.DictReader(open(sys.argv[2])))
ok = len(a) == len(b) and list(a[0]) == list(b[0])
for key in a[0]:
    if key in gravity:
        continue
    x = np.array([float(r[key]) for r in a])
    y = np.array([float(r[key]) for r in b])
    ok = ok and np.all(np.abs(x - y) <= 1e-13 * np.abs(y))
sys.exit(0 if ok else 1)
EOF2
    report $status "Toomre Q annular moments vs Kangaroo, rtol 1e-13 (gravity-independent columns)"

    # Where both gradient stencils are exact -- one grid, one level, a
    # quadratic potential -- every column agrees, gravity and Q included.
    rm -rf "$work/toomre1_ax" "$work/toomre1_k"
    "$python" "$examples/plotfile_toomre_q.py" "$work/plt_mhd1b" "${toomre[@]}" \
        --output-dir "$work/toomre1_ax" > /dev/null 2>&1
    (cd "$kangaroo" && "$kangaroo_python" scripts/plotfile_toomre_q.py \
        "$work/plt_mhd1b" "${toomre[@]}" --output-dir "$work/toomre1_k" > /dev/null 2>&1)
    status=0
    "$python" - "$work/toomre1_ax/plt_mhd1b_toomre_q.csv" \
        "$work/toomre1_k/plt_mhd1b_toomre_q.csv" <<'EOF2' || status=1
import csv, sys
import numpy as np
a = list(csv.DictReader(open(sys.argv[1])))
b = list(csv.DictReader(open(sys.argv[2])))
ok = len(a) == len(b) and list(a[0]) == list(b[0])
for key in a[0]:
    if key == "valid":
        ok = ok and [r[key] for r in a] == [r[key] for r in b]
        continue
    x = np.array([float(r[key]) for r in a])
    y = np.array([float(r[key]) for r in b])
    f = np.isfinite(y)
    ok = ok and np.array_equal(np.isfinite(x), f) and np.all(
        np.abs(x[f] - y[f]) <= 1e-13 * np.abs(y[f]))
sys.exit(0 if ok else 1)
EOF2
    report $status "Toomre Q vs Kangaroo, every column, rtol 1e-13 (one grid, one level)"
else
    echo "SKIP  Kangaroo comparisons ($kangaroo_python not found)"
fi

# --- yt: Kennicutt-Schmidt ---------------------------------------------------
if have_incite; then
    # Resolutions where no cell center lies on a pixel edge; see
    # docs/analysis-validation.md for why 32 is ill-conditioned here.
    for resolution in 24 40; do
        args=(--resolution "$resolution" --sfr-window-myr 20 --no-plot)
        "$python" "$incite/postprocessing/compute_kennicutt_schmidt.py" \
            "$work/plt_part" "${args[@]}" --output-csv "$work/ks_yt.csv" \
            > /dev/null 2>&1
        "$python" "$examples/compute_kennicutt_schmidt.py" "$work/plt_part" \
            "${args[@]}" --output-csv "$work/ks_ax.csv" > /dev/null
        status=0
        "$python" - "$work/ks_ax.csv" "$work/ks_yt.csv" <<'EOF' || status=1
import sys
import numpy as np
a = np.loadtxt(sys.argv[1], delimiter=",", skiprows=1)
b = np.loadtxt(sys.argv[2], delimiter=",", skiprows=1)
ok = a.shape == b.shape and np.all(np.abs(a - b) <= 1e-14 * np.abs(b))
sys.exit(0 if ok else 1)
EOF
        report $status "Kennicutt-Schmidt CSV vs yt, rtol 1e-14 (resolution $resolution)"
    done

    # --- yt: stellar population mass histogram -------------------------------
    for args in "" "--bins 12 --linear" "--min 20 --max 500 --bins 7" \
                "--print-stage-counts" "--output OUT.txt"; do
        read -r -a ours <<< "${args//OUT/$work/ax_stellar}"
        read -r -a theirs <<< "${args//OUT/$work/yt_stellar}"
        (cd "$work" && "$python" "$incite/postprocessing/stellar_pop_mass_hist.py" \
            plt_part ${theirs[@]+"${theirs[@]}"} --png yt.png \
            > yt_stellar.out 2> /dev/null)
        (cd "$work" && "$python" "$examples/stellar_pop_mass_hist.py" \
            plt_part ${ours[@]+"${ours[@]}"} --png ax.png > ax_stellar.out)
        status=0
        same_file "$work/ax_stellar.out" "$work/yt_stellar.out" || status=1
        if [[ -f "$work/ax_stellar.txt" ]]; then
            same_file "$work/ax_stellar.txt" "$work/yt_stellar.txt" || status=1
            rm -f "$work/ax_stellar.txt" "$work/yt_stellar.txt"
        fi
        report $status "stellar population mass histogram vs yt, identical output (${args:-defaults})"
    done

    # --- yt: density-temperature phase histogram -----------------------------
    for weight in mass volume count; do
        "$python" "$incite/postprocessing/plot_density_temperature_phase.py" \
            "$work/plt_part" --bins 40 --weight "$weight" \
            --output "$work/phase_yt.png" --npz "$work/phase_yt.npz" > /dev/null 2>&1
        "$python" "$examples/plot_density_temperature_phase.py" "$work/plt_part" \
            --bins 40 --weight "$weight" --output "$work/phase_ax.png" \
            --npz "$work/phase_ax.npz"
        status=0
        "$python" - "$work/phase_ax.npz" "$work/phase_yt.npz" <<'EOF2' || status=1
import sys
import numpy as np
a, b = np.load(sys.argv[1]), np.load(sys.argv[2])
ok = (all(np.array_equal(a[k], b[k]) for k in ("density_edges", "temperature_edges"))
      and np.all(np.abs(a["hist"] - b["hist"]) <= 1e-14 * np.abs(b["hist"])))
sys.exit(0 if ok else 1)
EOF2
        report $status "density-temperature phase histogram vs yt, rtol 1e-14 ($weight)"
    done

    # --- yt: radial surface-density profile ----------------------------------
    # Pixels that nest in the finest cells, where yt's pixel-center sampling
    # and the exact pixel average agree. With --width-kpc the pixel edges are
    # placed in unyt's kiloparsec, 1.7e-10 shorter than the fixture's, so
    # near the image edge they straddle cell faces by ~1e-9 kpc and the two
    # semantics differ by up to ~2e-8; see docs/analysis-validation.md.
    for case in "1e-13|--resolution 64 --n-bins 16 --r-max-kpc 8" \
                "1e-7|--resolution 96 --n-bins 24 --r-max-kpc 6 --width-kpc 12"; do
        rtol="${case%%|*}"
        args="${case#*|}"
        read -r -a options <<< "$args"
        rm -rf "$work/profile_yt" "$work/profile_ax"
        "$python" "$incite/postprocessing/plot_surface_density_profile.py" \
            "$work/plt_part" "${options[@]}" --output-dir "$work/profile_yt" \
            > /dev/null 2>&1
        "$python" "$examples/plot_surface_density_profile.py" "$work/plt_part" \
            "${options[@]}" --output-dir "$work/profile_ax" > /dev/null
        status=0
        "$python" - "$work/profile_ax/plt_part_surface_density_profile.csv" \
            "$work/profile_yt/plt_part_surface_density_profile.csv" "$rtol" <<'EOF2' || status=1
import sys
import numpy as np
a = np.loadtxt(sys.argv[1], delimiter=",", skiprows=1)
b = np.loadtxt(sys.argv[2], delimiter=",", skiprows=1)
rtol = float(sys.argv[3])
same_nan = np.array_equal(np.isnan(a), np.isnan(b))
finite = np.isfinite(b)
ok = (a.shape == b.shape and same_nan
      and np.all(np.abs(a[finite] - b[finite]) <= rtol * np.abs(b[finite])))
sys.exit(0 if ok else 1)
EOF2
        report $status "surface-density profile CSV vs yt, rtol $rtol ($args)"
    done
else
    echo "SKIP  yt comparisons (no incite-2026 checkout or no yt for $python)"
fi

# --- Exact answers: Toomre Q radial gravity ----------------------------------
# The fixture's harmonic potential has a closed-form radial gravity. On one
# level the port's tile gradient is exact on any grid layout; next to
# coarse-fine boundaries, linear ghost interpolation of a quadratic costs
# accuracy (bounded here at 5%; see docs/analysis-validation.md).
toomre=(--r-min-kpc 0.5 --r-max-kpc 7 --bins 13)
for case in "plt_mhd1|1e-13" "plt_mhd|0.05"; do
    plotfile="${case%%|*}"
    rtol="${case#*|}"
    rm -rf "$work/toomre_exact"
    "$python" "$examples/plotfile_toomre_q.py" "$work/$plotfile" "${toomre[@]}" \
        --output-dir "$work/toomre_exact" > /dev/null 2>&1
    status=0
    "$python" "$here/check_toomre_gravity.py" "$work/$plotfile" \
        "$work/toomre_exact/${plotfile}_toomre_q.csv" --rtol "$rtol" \
        > "$work/toomre_exact.txt" 2> /dev/null || status=$?
    report $status "Toomre Q radial gravity vs the exact answer, rtol $rtol ($plotfile)"
done

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
        # Kangaroo on the real plotfile: the projection, a slice whose plane
        # lies in the first finest cell above a level-0 face (where
        # Kangaroo's refined plane index is right on every level), and the
        # level-0 FAB extrema.
        "$python" "$examples/plotfile_projection.py" "$QUOKKA_PLOTFILE" \
            --var gasDensity --output "$work/q_proj.png" --npz "$work/q_proj_ax.npz"
        (cd "$kangaroo" && "$kangaroo_python" "$here/kangaroo_projection_array.py" \
            "$QUOKKA_PLOTFILE" "$work/q_proj_k.npz" --var gasDensity > /dev/null 2>&1)
        status=0
        "$python" - "$work/q_proj_ax.npz" "$work/q_proj_k.npz" <<'EOF2' || status=1
import sys
import numpy as np
a, b = np.load(sys.argv[1]), np.load(sys.argv[2])
x, y = a["surface_density_msun_pc2"], b["surface_density_msun_pc2"]
sys.exit(0 if x.shape == y.shape and np.all(np.abs(x - y) <= 1e-12 * np.abs(y)) else 1)
EOF2
        report $status "Quokka projection vs Kangaroo, rtol 1e-12"
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

# --- Real Quokka plotfiles with particles and MHD fields ------------------------
# Every output is deleted before the run that writes it, so a run that fails
# cannot be compared through a stale file.
if [[ -n "${QUOKKA_GALAXY:-}" ]] && have_kangaroo; then
    galaxy="$QUOKKA_GALAXY"
    # Flux sign bins and nets on real data hold cells whose normal velocity
    # is ~0, and nets of nearly cancelling bins; their relative error says
    # nothing about the integral (a one-ulp shift of the geometry moves such
    # a bin by 1e-10). Compared against each quantity's largest magnitude.
    for case in "plotfile_flux_surface|--radius-kpc 10" \
                "plotfile_flux_surface|--rmin-kpc 1 --rmax-kpc 60 --nbins 12" \
                "plotfile_cylindrical_flux_surface|--radius-kpc 10 --height-kpc 2.3" \
                "plotfile_cylindrical_flux_surface|--rmin-kpc 1 --rmax-kpc 40 --nbins 9 --height-kpc 3.1" \
                "plotfile_cylindrical_flux_surface|--radius-kpc 7.7 --zmin-kpc 0.3 --zmax-kpc 30 --nbins 9"; do
        script="${case%%|*}"
        read -r -a options <<< "${case#*|}"
        rm -f "$work/real_ax.json" "$work/real_k.json"
        "$python" "$examples/$script.py" "$galaxy" "${options[@]}" \
            --output-json "$work/real_ax.json" > /dev/null 2>&1
        (cd "$kangaroo" && "$kangaroo_python" "scripts/$script.py" "$galaxy" \
            "${options[@]}" --output-json "$work/real_k.json" > /dev/null 2>&1)
        status=0
        "$python" "$here/compare_json.py" "$work/real_ax.json" "$work/real_k.json" \
            --rtol 1e-12 --scale-by-key > /dev/null || status=$?
        report $status "real galaxy $script vs Kangaroo, 1e-12 of each quantity's scale (${case#*|})"
    done
    rm -f "$work"/real_sfr_*.json
    sfr=(--rmax-kpc 30 --zmax-kpc 30 --nbins 10)
    "$python" "$examples/plotfile_enclosed_sfr.py" "$galaxy" "${sfr[@]}" \
        --output-json "$work/real_sfr_disk_ax.json" \
        --output-json-wind "$work/real_sfr_wind_ax.json" \
        --output-json-joint "$work/real_sfr_joint_ax.json" > /dev/null 2>&1
    (cd "$kangaroo" && "$kangaroo_python" scripts/plotfile_enclosed_sfr.py "$galaxy" \
        "${sfr[@]}" --output-json "$work/real_sfr_disk_k.json" \
        --output-json-wind "$work/real_sfr_wind_k.json" \
        --output-json-joint "$work/real_sfr_joint_k.json" > /dev/null 2>&1)
    for kind in disk wind joint; do
        status=0
        "$python" "$here/compare_json.py" "$work/real_sfr_${kind}_ax.json" \
            "$work/real_sfr_${kind}_k.json" --rtol 1e-12 > /dev/null || status=$?
        report $status "real galaxy enclosed SFR ($kind) vs Kangaroo, rtol 1e-12"
    done
    for axis in z x; do
        rm -f "$work/real_cic_ax.npz" "$work/real_cic_k.npz"
        "$python" "$examples/plotfile_projection_cic_stellar.py" "$galaxy" \
            --particle-type CIC_particles --axis "$axis" --output "$work/real_cic.png" \
            --npz "$work/real_cic_ax.npz" 2> /dev/null
        (cd "$kangaroo" && "$kangaroo_python" "$here/kangaroo_cic_array.py" "$galaxy" \
            "$work/real_cic_k.npz" --particle-type CIC_particles --axis "$axis" \
            > /dev/null 2>&1)
        status=0
        "$python" "$here/compare_cic.py" "$galaxy" "$work/real_cic_ax.npz" \
            "$work/real_cic_k.npz" --axis "$axis" > "$work/real_cic.txt" || status=$?
        report $status "real galaxy CIC projection ($axis) vs Kangaroo away from boundaries: $(cat "$work/real_cic.txt")"
    done
    rm -f "$work/real_hist_ax.txt" "$work/real_hist_k.txt"
    "$python" "$examples/plotfile_particle_mass_histogram.py" "$galaxy" \
        --particle-type CIC_particles --bins 24 --no-plot > "$work/real_hist_ax.txt"
    (cd "$kangaroo" && "$kangaroo_python" scripts/plotfile_particle_mass_histogram.py \
        "$galaxy" --particle-type CIC_particles --bins 24 --no-plot \
        > "$work/real_hist_k.txt" 2> /dev/null)
    status=0
    same_file "$work/real_hist_ax.txt" "$work/real_hist_k.txt" || status=1
    report $status "real galaxy CIC particle mass histogram vs Kangaroo, identical output"
    rm -f "$work/real_proj_ax.npz" "$work/real_proj_k.npz"
    "$python" "$examples/plotfile_projection.py" "$galaxy" --var gasDensity \
        --output "$work/real_proj.png" --npz "$work/real_proj_ax.npz"
    (cd "$kangaroo" && "$kangaroo_python" "$here/kangaroo_projection_array.py" \
        "$galaxy" "$work/real_proj_k.npz" --var gasDensity > /dev/null 2>&1)
    status=0
    "$python" - "$work/real_proj_ax.npz" "$work/real_proj_k.npz" <<'EOF2' || status=1
import sys
import numpy as np
a, b = np.load(sys.argv[1]), np.load(sys.argv[2])
x, y = a["surface_density_msun_pc2"], b["surface_density_msun_pc2"]
sys.exit(0 if x.shape == y.shape and np.all(np.abs(x - y) <= 1e-12 * np.abs(y)) else 1)
EOF2
    report $status "real galaxy projection vs Kangaroo, rtol 1e-12"
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
    # The galaxy stores no temperature, so the phase histogram bins density
    # against internal energy density: the same code path on real leaf
    # cells, with yt using the values unconverted as it does for any field
    # without temperature units.
    if have_incite; then
        for weight in mass volume; do
            phase=(--bins 48 --weight "$weight" --temperature-field gasInternalEnergy
                   --density-min 1e-30 --density-max 1e-22
                   --temperature-min 1e-20 --temperature-max 1e-8)
            rm -f "$work"/real_phase_*
            "$python" "$incite/postprocessing/plot_density_temperature_phase.py" \
                "$galaxy" "${phase[@]}" --output "$work/real_phase_yt.png" \
                --npz "$work/real_phase_yt.npz" > /dev/null 2>&1
            "$python" "$examples/plot_density_temperature_phase.py" "$galaxy" \
                "${phase[@]}" --output "$work/real_phase_ax.png" \
                --npz "$work/real_phase_ax.npz"
            status=0
            "$python" - "$work/real_phase_ax.npz" "$work/real_phase_yt.npz" <<'EOF2' || status=1
import sys
import numpy as np
a, b = np.load(sys.argv[1]), np.load(sys.argv[2])
ok = (all(np.array_equal(a[k], b[k]) for k in ("density_edges", "temperature_edges"))
      and b["hist"].sum() > 0
      and np.all(np.abs(a["hist"] - b["hist"]) <= 1e-14 * np.abs(b["hist"])))
sys.exit(0 if ok else 1)
EOF2
            report $status "real galaxy density-energy phase histogram vs yt, rtol 1e-14 ($weight)"
        done
    fi
else
    echo "SKIP  real galaxy comparisons (QUOKKA_GALAXY unset or no Kangaroo)"
fi

if [[ -n "${QUOKKA_DISK:-}" ]]; then
    disk="$QUOKKA_DISK"
    if have_kangaroo; then
        # Toomre Q: every column but the gravity-derived ones (Kangaroo's
        # gradient stencil is wrong at block faces), compared against each
        # column's scale -- the mean radial velocity is a near-zero mean.
        toomre=(--r-min-kpc 0.5 --r-max-kpc 12 --bins 23)
        rm -rf "$work/disk_toomre_ax" "$work/disk_toomre_k"
        "$python" "$examples/plotfile_toomre_q.py" "$disk" "${toomre[@]}" \
            --output-dir "$work/disk_toomre_ax" > /dev/null 2>&1
        (cd "$kangaroo" && "$kangaroo_python" scripts/plotfile_toomre_q.py "$disk" \
            "${toomre[@]}" --output-dir "$work/disk_toomre_k" > /dev/null 2>&1)
        status=0
        name="$(basename "$disk")_toomre_q.csv"
        "$python" - "$work/disk_toomre_ax/$name" "$work/disk_toomre_k/$name" <<'EOF2' || status=1
import csv, sys
import numpy as np
gravity = {"q_thermal_magnetic", "q_thermal_turbulent", "q_thermal_turbulent_magnetic",
           "radial_gravity_cm_s2", "circular_speed_km_s", "omega_myr_inv",
           "kappa_myr_inv", "valid"}
a = list(csv.DictReader(open(sys.argv[1])))
b = list(csv.DictReader(open(sys.argv[2])))
ok = len(a) == len(b) > 0 and list(a[0]) == list(b[0])
for key in a[0]:
    if key in gravity:
        continue
    x = np.array([float(r[key]) for r in a])
    y = np.array([float(r[key]) for r in b])
    scale = np.max(np.abs(y))
    ok = ok and np.all(np.abs(x - y) <= 1e-12 * scale)
sys.exit(0 if ok else 1)
EOF2
        report $status "real disk Toomre Q moments vs Kangaroo, 1e-12 of each column's scale (gravity-independent columns)"
        for script in plotfile_flux_surface plotfile_cylindrical_flux_surface; do
            # No passive scalar in this plotfile; entropy stands in for one.
            options=(--radius-kpc 8.3 --scalar entropy
                     --temperature-bins 1e3 1e4 1e5 1e6 1e8)
            [[ $script == plotfile_cylindrical_flux_surface ]] && options+=(--height-kpc 1.7)
            rm -f "$work/disk_ax.json" "$work/disk_k.json"
            "$python" "$examples/$script.py" "$disk" "${options[@]}" \
                --output-json "$work/disk_ax.json" > /dev/null 2>&1
            (cd "$kangaroo" && "$kangaroo_python" "scripts/$script.py" "$disk" \
                "${options[@]}" --output-json "$work/disk_k.json" > /dev/null 2>&1)
            status=0
            "$python" "$here/compare_json.py" "$work/disk_ax.json" "$work/disk_k.json" \
                --rtol 1e-12 --scale-by-key > /dev/null || status=$?
            report $status "real disk $script with temperature bins vs Kangaroo, 1e-12 of each quantity's scale"
        done
    fi
    if have_incite; then
        # Bins here hold up to ~1e6 cells, and both codes sum a bin's weights
        # in histogram2d's plain running sum, in different orders, so a
        # weighted bin is only good to its cell count times epsilon (both
        # stay well inside that bound against exactly rounded sums; see
        # docs/analysis-validation.md). The count histogram is exact and
        # supplies the per-bin counts for the others' tolerance.
        rm -f "$work"/disk_phase_*
        for weight in count mass volume; do
            phase=(--bins 64 --weight "$weight")
            "$python" "$incite/postprocessing/plot_density_temperature_phase.py" \
                "$disk" "${phase[@]}" --output "$work/disk_phase_yt.png" \
                --npz "$work/disk_phase_${weight}_yt.npz" > /dev/null 2>&1
            "$python" "$examples/plot_density_temperature_phase.py" "$disk" \
                "${phase[@]}" --output "$work/disk_phase_ax.png" \
                --npz "$work/disk_phase_${weight}_ax.npz"
            status=0
            "$python" - "$work/disk_phase_${weight}_ax.npz" \
                "$work/disk_phase_${weight}_yt.npz" "$work/disk_phase_count_yt.npz" <<'EOF2' || status=1
import sys
import numpy as np
a, b, c = (np.load(path) for path in sys.argv[1:])
counts = c["hist"]
tolerance = (2.0 * counts * np.finfo(float).eps + 1e-14) * np.abs(b["hist"])
ok = (all(np.array_equal(a[k], b[k]) for k in ("density_edges", "temperature_edges"))
      and b["hist"].sum() > 0 and np.all(np.abs(a["hist"] - b["hist"]) <= tolerance))
sys.exit(0 if ok else 1)
EOF2
            report $status "real disk density-temperature phase histogram vs yt, within 2 n eps per bin ($weight)"
        done
    fi
else
    echo "SKIP  real disk comparisons (QUOKKA_DISK unset)"
fi

if [[ -n "${QUOKKA_STARS:-}" ]]; then
    stars="$QUOKKA_STARS"
    if have_kangaroo; then
        rm -f "$work/real_shist_ax.txt" "$work/real_shist_k.txt"
        "$python" "$examples/plotfile_particle_mass_histogram.py" "$stars" \
            --bins 24 --no-plot > "$work/real_shist_ax.txt"
        (cd "$kangaroo" && "$kangaroo_python" scripts/plotfile_particle_mass_histogram.py \
            "$stars" --bins 24 --no-plot > "$work/real_shist_k.txt" 2> /dev/null)
        status=0
        same_file "$work/real_shist_ax.txt" "$work/real_shist_k.txt" || status=1
        report $status "real stars particle mass histogram vs Kangaroo, identical output"
        rm -f "$work/real_scic_ax.npz" "$work/real_scic_k.npz"
        "$python" "$examples/plotfile_projection_cic_stellar.py" "$stars" \
            --output "$work/real_scic.png" --npz "$work/real_scic_ax.npz" 2> /dev/null
        (cd "$kangaroo" && "$kangaroo_python" "$here/kangaroo_cic_array.py" "$stars" \
            "$work/real_scic_k.npz" > /dev/null 2>&1)
        status=0
        "$python" "$here/compare_cic.py" "$stars" "$work/real_scic_ax.npz" \
            "$work/real_scic_k.npz" > "$work/real_scic.txt" || status=$?
        report $status "real stars stellar CIC projection vs Kangaroo away from boundaries: $(cat "$work/real_scic.txt")"
    fi
    if have_incite; then
        for resolution in 24 40; do
            rm -f "$work/real_ks_ax.csv" "$work/real_ks_yt.csv"
            "$python" "$incite/postprocessing/compute_kennicutt_schmidt.py" "$stars" \
                --resolution "$resolution" --no-plot \
                --output-csv "$work/real_ks_yt.csv" > /dev/null 2>&1
            "$python" "$examples/compute_kennicutt_schmidt.py" "$stars" \
                --resolution "$resolution" --no-plot \
                --output-csv "$work/real_ks_ax.csv" > /dev/null
            status=0
            "$python" - "$work/real_ks_ax.csv" "$work/real_ks_yt.csv" <<'EOF2' || status=1
import sys
import numpy as np
a = np.loadtxt(sys.argv[1], delimiter=",", skiprows=1, ndmin=2)
b = np.loadtxt(sys.argv[2], delimiter=",", skiprows=1, ndmin=2)
ok = a.shape == b.shape and a.size > 0 and np.all(np.abs(a - b) <= 1e-14 * np.abs(b))
sys.exit(0 if ok else 1)
EOF2
            report $status "real stars Kennicutt-Schmidt CSV vs yt, rtol 1e-14 (resolution $resolution)"
        done
        for args in "" "--bins 12 --linear" "--print-stage-counts"; do
            read -r -a options <<< "$args"
            rm -f "$work/real_spop_ax.out" "$work/real_spop_yt.out"
            "$python" "$incite/postprocessing/stellar_pop_mass_hist.py" "$stars" \
                ${options[@]+"${options[@]}"} --png "$work/real_spop_yt.png" \
                > "$work/real_spop_yt.out" 2> /dev/null
            "$python" "$examples/stellar_pop_mass_hist.py" "$stars" \
                ${options[@]+"${options[@]}"} --png "$work/real_spop_ax.png" \
                > "$work/real_spop_ax.out"
            status=0
            same_file "$work/real_spop_ax.out" "$work/real_spop_yt.out" || status=1
            report $status "real stars population mass histogram vs yt, identical output (${args:-defaults})"
        done
    fi
else
    echo "SKIP  real stars comparisons (QUOKKA_STARS unset)"
fi

if [[ $failures -ne 0 ]]; then
    echo "$failures comparison(s) failed; outputs are in $work"
    exit 1
fi
echo "all comparisons passed"
