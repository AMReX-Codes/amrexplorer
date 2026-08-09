#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${AMREXPLORER_BUILD_DIR:-${repo_dir}/build}"

if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
    echo "error: ${build_dir} is not a configured AMReXplorer build" >&2
    echo "set AMREXPLORER_BUILD_DIR to the configured build directory" >&2
    exit 2
fi

target="amrexplorer_render_equivalence"
binary="${build_dir}/tools/render_equivalence/amrexplorer-render-equivalence"
if [[ "${1:-}" == "--ui-fixed-scale-repro" ]]; then
    shift
    target="amrexplorer_qt"
    probe_config_dir="$(mktemp -d "${TMPDIR:-/tmp}/amrexplorer-fixed-scale.XXXXXX")"
    trap 'rm -rf "${probe_config_dir}"' EXIT
    export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
    export XDG_CONFIG_HOME="${probe_config_dir}"
    if [[ -x "${build_dir}/src/qt/amrexplorer.app/Contents/MacOS/amrexplorer" ]]; then
        binary="${build_dir}/src/qt/amrexplorer.app/Contents/MacOS/amrexplorer"
    else
        binary="${build_dir}/src/qt/amrexplorer"
    fi
    set -- --fixed-scale-local-remote-repro "$@"
fi

cmake --build "${build_dir}" --target "${target}"

if [[ -t 0 ]]; then
    read -r -s -p "Session token: " session_token
    printf '\n' >&2
    printf '%s\n' "${session_token}" | "${binary}" "$@"
else
    "${binary}" "$@"
fi
