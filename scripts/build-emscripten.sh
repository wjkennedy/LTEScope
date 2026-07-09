#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "${script_dir}/.." && pwd)"
build_dir="${BUILD_DIR:-${root_dir}/build-emscripten}"
jobs="${JOBS:-4}"
em_cache="${EM_CACHE:-${build_dir}/emcache}"

export EM_CACHE="${em_cache}"

# The web target uses the WebUSB RTL-SDR bridge in src/sdr/webrtlsdr_bridge.js.
emcmake cmake -S "${root_dir}" -B "${build_dir}" -G Ninja \
    -DEMSCRIPTEN=ON \
    -DHAS_LTE=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${root_dir}/scripts/emcc-filter-wrapper.sh" \
    -DCMAKE_CXX_COMPILER="${root_dir}/scripts/emxx-filter-wrapper.sh"

cmake --build "${build_dir}" -j"${jobs}"
