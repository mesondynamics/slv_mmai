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
task_secure_elf="${task_project_dir}/Secure/build/OEMiROT/${task_build_type}/ECU_S.elf"
task_oemirot_import_library="${task_project_dir}/Secure/build/OEMiROT/${task_build_type}/secure_nsclib.o"
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

# Validate all three release artifacts, including the absolute symbols the
# final NonSecure ELF actually resolved. This closes the build-race path where
# a development-layout import object could be consumed between S and NS links.
task_nonsecure_elf="${task_project_dir}/NonSecure/build/OEMiROT/${task_build_type}/ECU_NS.elf"
python3 "${task_script_dir}/check_nsc_abi.py" \
  --abi-source "${task_project_dir}/Secure_nsclib/secure_nsc_abi_v1.s" \
  --import-library "${task_oemirot_import_library}" \
  --secure-elf "${task_secure_elf}" \
  --nonsecure-elf "${task_nonsecure_elf}" \
  --readelf "${task_toolchain_dir}/arm-none-eabi-readelf"

# CLOSED disables the debug/trace domain after a power cycle.  Scan the final
# Secure application, not just source text, so an indirect or generated DWT
# dependency cannot reproduce a probe-dependent production boot.
task_secure_disassembly="$("${task_toolchain_dir}/arm-none-eabi-objdump" -d "${task_secure_elf}")"
if grep -Fq 'e0001000' <<<"${task_secure_disassembly}"; then
  printf 'Secure application contains the DWT debug-domain aperture; CLOSED autonomous boot would be probe-dependent.\n' >&2
  exit 1
fi
if grep -R -E -n --include='*.c' --include='*.h' \
     '(DWT->|CoreDebug->|DWT_|CoreDebug_)' \
     "${task_project_dir}/Secure/App"; then
  printf 'Secure application source contains a debug-domain timing dependency.\n' >&2
  exit 1
fi
printf 'Secure application is independent of DWT/CoreDebug and supports autonomous CLOSED boot.\n'
