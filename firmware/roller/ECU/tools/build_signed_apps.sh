#!/usr/bin/env bash
set -euo pipefail

task_build_type="${1:-Release}"
task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_cmake_bin="${CMAKE_BIN:-/home/plac/.local/share/stm32cube/bundles/cmake/4.0.1+st.3/bin/cmake}"
task_ninja_dir="${NINJA_BIN_DIR:-/home/plac/.local/share/stm32cube/bundles/ninja/1.13.1+st.1/bin}"
task_toolchain_dir="${ARM_GNU_BIN_DIR:-/home/plac/.local/share/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin}"

case "${task_build_type}" in
  Debug|Release) ;;
  *) echo "Usage: $0 {Debug|Release}" >&2; exit 2 ;;
esac

export PATH="${task_ninja_dir}:${task_toolchain_dir}:${PATH}"
for task_context in Secure NonSecure; do
  task_source_dir="${task_project_dir}/${task_context}"
  task_binary_dir="${task_source_dir}/build/OEMiROT/${task_build_type}"
  "${task_cmake_bin}" \
    -S "${task_source_dir}" \
    -B "${task_binary_dir}" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${task_project_dir}/gcc-arm-none-eabi.cmake" \
    -DCMAKE_BUILD_TYPE="${task_build_type}" \
    -DECU_OEMIROT_LAYOUT=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  "${task_cmake_bin}" --build "${task_binary_dir}" --parallel
done
