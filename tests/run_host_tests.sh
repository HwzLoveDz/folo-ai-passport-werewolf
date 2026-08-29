#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/.." && pwd)"
build_dir="${WW_HOST_BUILD_DIR:-${repo_dir}/build-host}"

cmake -S "${script_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Debug
cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --output-on-failure
