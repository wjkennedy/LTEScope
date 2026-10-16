#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "${script_dir}/.." && pwd)"
build_dir="${BUILD_DIR:-${root_dir}/build-emscripten}"
jobs="${JOBS:-4}"
em_cache="${EM_CACHE:-${build_dir}/emcache}"

export EM_CACHE="${em_cache}"

# Homebrew Emscripten 6 requires Python >=3.10. On macOS, /usr/bin/python3
# can still be 3.9 even when a current Homebrew Python is installed.
if [[ -x /opt/homebrew/opt/python@3.13/bin/python3 ]]; then
    export PATH="/opt/homebrew/opt/python@3.13/bin:${PATH}"
elif [[ -x /opt/homebrew/opt/python@3.14/bin/python3 ]]; then
    export PATH="/opt/homebrew/opt/python@3.14/bin:${PATH}"
fi

# The web target includes WebUSB bridges for RTL-SDR and ESP-SDR.
emcmake cmake -S "${root_dir}" -B "${build_dir}" -G Ninja \
    -DEMSCRIPTEN=ON \
    -DHAS_LTE=OFF \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "${build_dir}" -j"${jobs}"
