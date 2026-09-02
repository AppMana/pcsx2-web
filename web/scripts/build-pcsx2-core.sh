#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
emsdk_root="${PCSX2_WEB_EMSDK:-${XDG_CACHE_HOME:-${HOME}/.cache}/emsdk-6.0.8}"
build_jobs="${PCSX2_WEB_JOBS:-4}"
build_dir="${repo_root}/build-pcsx2-web"
stage_dir="${repo_root}/web/public/core"

if [[ -f "${emsdk_root}/emsdk_env.sh" ]]; then
  source "${emsdk_root}/emsdk_env.sh" >/dev/null
elif ! command -v emcmake >/dev/null 2>&1; then
  echo "Emscripten is not active and no SDK was found at ${emsdk_root}. Set PCSX2_WEB_EMSDK." >&2
  exit 1
fi

emcmake cmake -S "${repo_root}" -B "${build_dir}" \
  -DPCSX2_WEB=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -G Ninja
cmake --build "${build_dir}" --target pcsx2_web_runtime pcsx2_web_unit_tests --parallel "${build_jobs}"
mkdir -p "${stage_dir}"
for f in pcsx2-web.mjs pcsx2-web.wasm pcsx2-web-units.mjs pcsx2-web-units.wasm; do
  cmake -E copy_if_different "${build_dir}/bin/${f}" "${stage_dir}/${f}"
done
ls -l "${stage_dir}/pcsx2-web.wasm" "${stage_dir}/pcsx2-web-units.wasm"
