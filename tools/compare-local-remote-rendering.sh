#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${AMREXPLORER_BUILD_DIR:-${repo_dir}/build}"
binary="${build_dir}/tools/render_equivalence/amrexplorer-render-equivalence"

if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
    echo "error: ${build_dir} is not a configured AMReXplorer build" >&2
    echo "set AMREXPLORER_BUILD_DIR to the configured build directory" >&2
    exit 2
fi

cmake --build "${build_dir}" --target amrexplorer_render_equivalence

if [[ -t 0 ]]; then
    read -r -s -p "Session token: " session_token
    printf '\n' >&2
    printf '%s\n' "${session_token}" | "${binary}" "$@"
else
    "${binary}" "$@"
fi
