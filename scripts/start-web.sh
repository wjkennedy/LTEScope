#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "${script_dir}/.." && pwd)"
build_dir="${BUILD_DIR:-${root_dir}/build-web}"
port="${PORT:-8000}"

BUILD_DIR="${build_dir}" "${script_dir}/build-emscripten.sh"

echo "LTEScope: http://localhost:${port}/LTEScope.html"
echo "Board flasher: http://localhost:${port}/flash.html"
echo "Press Ctrl-C to stop the server."
exec python3 -m http.server "${port}" --bind 127.0.0.1 --directory "${build_dir}"
