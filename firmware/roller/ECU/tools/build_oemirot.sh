#!/usr/bin/env bash
set -euo pipefail

task_profile="${1:-ReleaseOpen}"
task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_source_dir="${task_project_dir}/Bootloader/OEMiROT"
task_cmake_bin="${CMAKE_BIN:-/home/plac/.local/share/stm32cube/bundles/cmake/4.0.1+st.3/bin/cmake}"
task_ninja_dir="${NINJA_BIN_DIR:-/home/plac/.local/share/stm32cube/bundles/ninja/1.13.1+st.1/bin}"
task_toolchain_dir="${ARM_GNU_BIN_DIR:-/home/plac/.local/share/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin}"

case "${task_profile}" in
  Debug)
    task_build_type=Debug
    task_open_bench=ON
    ;;
  ReleaseOpen)
    task_build_type=Release
    task_open_bench=ON
    ;;
  ReleaseClosed)
    task_build_type=Release
    task_open_bench=OFF
    ;;
  *) echo "Usage: $0 {Debug|ReleaseOpen|ReleaseClosed}" >&2; exit 2 ;;
esac
task_binary_dir="${task_source_dir}/build/${task_profile}"

export PATH="${task_ninja_dir}:${task_toolchain_dir}:${PATH}"
"${task_cmake_bin}" \
  -S "${task_source_dir}" \
  -B "${task_binary_dir}" \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${task_project_dir}/gcc-arm-none-eabi.cmake" \
  -DCMAKE_BUILD_TYPE="${task_build_type}" \
  -DROLLER_OEMIROT_OPEN_BENCH="${task_open_bench}"
"${task_cmake_bin}" --build "${task_binary_dir}" --parallel
printf 'OEMiROT profile: %s (CMake %s, OPEN bench policy %s)\n' \
  "${task_profile}" "${task_build_type}" "${task_open_bench}"
