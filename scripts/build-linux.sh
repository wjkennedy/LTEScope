#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "${script_dir}/.." && pwd)"
build_dir="${BUILD_DIR:-${root_dir}/build-linux}"
build_type="${BUILD_TYPE:-RelWithDebInfo}"
jobs="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
c_compiler="${C_COMPILER:-cc}"
cxx_compiler="${CXX_COMPILER:-c++}"

if ! command -v pkg-config >/dev/null 2>&1; then
    echo "pkg-config is required for the Linux build" >&2
    exit 1
fi

if ! pkg-config --exists librtlsdr && ! pkg-config --exists rtl-sdr; then
    echo "RTL-SDR development files were not found via pkg-config" >&2
    exit 1
fi

cmake -S "${root_dir}" -B "${build_dir}" -G Ninja \
    -DHAS_LTE=ON \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DCMAKE_C_COMPILER="${c_compiler}" \
    -DCMAKE_CXX_COMPILER="${cxx_compiler}"

cmake --build "${build_dir}" -j"${jobs}"
