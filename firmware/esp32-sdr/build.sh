#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "${script_dir}/../.." && pwd)"
target="${1:-t-display-s3}"
source_dir="${ESP_SDR_SOURCE_DIR:-${root_dir}/build-firmware/esp-sdr-s2-range-src}"
upstream="https://github.com/Kelsidavis/esp-sdr-s2-range.git"
revision="11d6dfeb9542d26b5fd227d9ec027e00c7f1b046"
idf_revision="25fe69f946311abdaf9ad56591f25fedbc20ac98"

case "${target}" in
  heltec-v3|t-display-s3)
    chip="esp32s3"
    ;;
  cyd)
    chip="esp32"
    ;;
  *)
    echo "Usage: $0 {heltec-v3|t-display-s3|cyd}" >&2
    exit 2
    ;;
esac

if [[ -z "${IDF_PATH:-}" || ! -x "${IDF_PATH}/tools/idf.py" ]]; then
  echo "Activate ESP-IDF and set IDF_PATH before building." >&2
  echo "Required ESP-IDF commit: ${idf_revision}" >&2
  exit 2
fi
actual_idf_revision="$(git -C "${IDF_PATH}" rev-parse HEAD)"
if [[ "${actual_idf_revision}" != "${idf_revision}" ]]; then
  echo "ESP-IDF ${idf_revision} is required; found ${actual_idf_revision}." >&2
  exit 2
fi

if [[ ! -d "${source_dir}/.git" ]]; then
  mkdir -p "$(dirname "${source_dir}")"
  git clone "${upstream}" "${source_dir}"
fi
git -C "${source_dir}" fetch --quiet origin "${revision}"
git -C "${source_dir}" checkout --quiet --detach "${revision}"
git -C "${source_dir}" submodule update --init --recursive

build_dir="${ESP_SDR_BUILD_DIR:-${root_dir}/build-firmware/${target}}"
output_dir="${root_dir}/web/firmware/${target}"
sdkconfig="${build_dir}/sdkconfig"
sdkconfig_defaults="${source_dir}/sdkconfig.defaults.${chip}"
if [[ "${target}" == "heltec-v3" ]]; then
  sdkconfig="${build_dir}/sdkconfig.heltec-v3"
  sdkconfig_defaults="${sdkconfig_defaults};${script_dir}/sdkconfig.heltec-v3.defaults"
fi

"${IDF_PATH}/tools/idf.py" -C "${source_dir}" -B "${build_dir}" \
  -DIDF_TARGET="${chip}" \
  -DSDKCONFIG="${sdkconfig}" \
  -DSDKCONFIG_DEFAULTS="${sdkconfig_defaults}" \
  -DESP_SDR_STREAMING=OFF \
  -DRING_PROBE=OFF -DSAMPLE_RATE_PROBE=OFF \
  -DFILTER_REGISTER_PROBE=OFF -DS3_RF_PROBE=OFF \
  -DC5_TUNE_PROBE=OFF -DS2_RF_PROBE=OFF \
  build

mkdir -p "${output_dir}"
pushd "${build_dir}" >/dev/null
esptool --chip "${chip}" merge-bin \
  -o "${output_dir}/ltescope-${target}.bin" \
  @flash_args
popd >/dev/null
cp "${source_dir}/LICENSE" "${output_dir}/LICENSE.GPL-3.0-or-later.txt"

echo "Firmware ready: ${output_dir}/ltescope-${target}.bin"
