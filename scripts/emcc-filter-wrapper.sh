#!/usr/bin/env bash
set -euo pipefail

real_cc="${EMSCRIPTEN_REAL_CC:-emcc}"
args=()
skip_next=0

for arg in "$@"; do
    if [[ $skip_next -eq 1 ]]; then
        skip_next=0
        continue
    fi

    if [[ $arg == "-isystem" || $arg == "-isysroot" ]]; then
        skip_next=1
        continue
    fi

    if [[ $arg == */sysroot/include ]]; then
        continue
    fi

    if [[ $arg == "-Wl,-search_paths_first" || $arg == "-Wl,-headerpad_max_install_names" ]]; then
        continue
    fi

    args+=("$arg")
done

exec "$real_cc" "${args[@]}"
