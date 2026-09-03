#!/usr/bin/env bash
# Renders every fixture GS dump with the native WebGPU (Dawn) backend of pcsx2-gsrunner and
# stores the PNGs under tests/fixtures/<name>/expected/webgpu-native/<dump>/, the reference the
# browser lane (tests/e2e/gsdump-webgpu.spec.ts) compares its captured frames against by MD5.
# The runner replays each dump twice (-loop 2) and writes the frames of the last replay, so the
# first replay has warmed the texture cache the way it is in the browser run.
#
#   web/scripts/webgpu-native-baseline.sh [path/to/pcsx2-gsrunner] [fixture ...]
set -euo pipefail

web_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "${web_root}/.." && pwd)"
runner="${PCSX2_GSRUNNER:-${repo_root}/build-native/bin/pcsx2-gsrunner}"
if [[ $# -gt 0 && -x "$1" ]]; then
  runner="$1"
  shift
fi
if [[ ! -x "${runner}" ]]; then
  echo "pcsx2-gsrunner not found at ${runner}; build with -DUSE_WEBGPU=ON -DUSE_WEBGPU_NATIVE=ON or set PCSX2_GSRUNNER" >&2
  exit 1
fi

fixtures=("$@")
if [[ ${#fixtures[@]} -eq 0 ]]; then
  for dir in "${web_root}"/tests/fixtures/*/; do
    [[ -d "${dir}expected/dumps" ]] && fixtures+=("$(basename "${dir}")")
  done
fi

for fixture in "${fixtures[@]}"; do
  dumps_dir="${web_root}/tests/fixtures/${fixture}/expected/dumps"
  for dump in "${dumps_dir}"/*.gs.zst; do
    [[ -f "${dump}" ]] || continue
    name="$(basename "${dump}" .gs.zst)"
    out="${web_root}/tests/fixtures/${fixture}/expected/webgpu-native/${name}"
    rm -rf "${out}"
    mkdir -p "${out}"
    echo "${fixture}/${name}"
    "${runner}" -renderer webgpu -surfaceless -noshadercache -loop 2 -dumpdir "${out}" -- "${dump}" > "${out}/gsrunner.log" 2>&1
    grep -E "WebGPU: Using adapter|@HWSTAT@" "${out}/gsrunner.log" | sed 's/^\[[ 0-9.]*\] //' > "${out}/summary.txt"
    rm -f "${out}/gsrunner.log"
    ls "${out}"/*.png | xargs -n1 basename | sed 's/^/  /'
  done
done
