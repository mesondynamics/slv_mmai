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

# The paired-update policy is generated from a pinned upstream loader. Check
# the actual derivative, including ordering that cannot be proven by grepping
# the CMake template alone, before accepting any bootloader build artifact.
python3 - "${task_binary_dir}/loader_roller_ecu.c" <<'PY'
from pathlib import Path
import sys

loader_path = Path(sys.argv[1])
text = loader_path.read_text(encoding="utf-8")
required = (
    "Permanent swap is forbidden for paired releases",
    "Interrupted permanent swap is forbidden",
    "RollerPair_PartialSwapIsForbidden",
    "Conflicting paired TEST/REVERT state",
    "Paired image_ok read failed",
    "Paired confirmed primary validation failed",
    "Paired confirmed release identities do not match",
    "Unconfirmed primary cannot consume failed staging",
    "Paired countermeasure image_ok read failed",
    "Paired countermeasure release is not coherent",
    "Paired countermeasure update image_ok read failed",
    "RollerPair_AllImagesConfirmed",
    "RollerPair_ReleaseReadyToCommit",
    "RollerPair_ReleaseIdentitiesMatch",
    "Paired target release identities do not match",
    "Final paired primary double validation is incomplete",
    "Final paired primary release identities do not match",
    "fih_int_encode_zero_equality",
    "fih_int roller_primary_pair_preconfirmed = FIH_FAILURE;",
    "roller_primary_pair_preconfirmed = FIH_SUCCESS;",
    "fih_not_eq(roller_primary_pair_preconfirmed, FIH_SUCCESS)",
)
missing = [pattern for pattern in required if pattern not in text]
if missing:
    raise SystemExit(
        f"{loader_path}: paired loader derivative is incomplete: {missing}"
    )

partial_branch = text.find("if (!boot_status_is_reset(bs))")
partial_perm_guard = text.find(
    "Interrupted permanent swap is forbidden", partial_branch
)
partial_review = text.find(
    "boot_review_image_swap_types(state, true);", partial_perm_guard
)
partial_complete = text.find(
    "boot_complete_partial_swap(state, bs);", partial_review
)
preconfirmed_init = text.find(
    "fih_int roller_primary_pair_preconfirmed = FIH_FAILURE;"
)
initial_validation = text.find("Paired confirmed primary validation failed")
initial_release_ready = text.find(
    "RollerPair_ReleaseReadyToCommit(", initial_validation
)
initial_ready_fih_encode = text.find(
    "fih_rc = fih_int_encode_zero_equality(!roller_commit_ready);",
    initial_release_ready,
)
initial_ready_fih_check = text.find(
    "if (fih_not_eq(fih_rc, FIH_SUCCESS))", initial_ready_fih_encode
)
initial_identity = text.find(
    "Paired confirmed release identities do not match", initial_ready_fih_check
)
initial_counter = text.find(
    "boot_update_security_counter(", initial_identity
)
preconfirmed_set = text.find(
    "roller_primary_pair_preconfirmed = FIH_SUCCESS;", initial_identity
)
dependency = text.find("rc = boot_verify_dependencies(state);")
target = text.find("Treat the post-operation images as one signed target")
swap = text.find("switch (BOOT_SWAP_TYPE(state))", target)
fail_case = text.find("case BOOT_SWAP_TYPE_FAIL:", swap)
fail_guard = text.find(
    "Unconfirmed primary cannot consume failed staging", fail_case
)
fail_fih_gate = text.rfind(
    "fih_not_eq(roller_primary_pair_preconfirmed, FIH_SUCCESS)",
    fail_case,
    fail_guard,
)
fail_image_ok = text.find("swap_set_image_ok(BOOT_CURR_IMG(state));", fail_guard)
primary_validation = text.find("Starting validation of primary slot(s)", swap)
final_validation = text.find(
    "Final paired primary double validation is incomplete", primary_validation
)
final_gate = text.find("Final paired primary release identities do not match")
response = text.find("rsp->br_flash_dev_id", final_gate)
countermeasure = text.find("Paired countermeasure image_ok read failed", response)
countermeasure_identity = text.find(
    "Paired countermeasure release is not coherent", countermeasure
)
countermeasure_reread = text.find(
    "Paired countermeasure update image_ok read failed", countermeasure_identity
)
countermeasure_update = text.find(
    "boot_update_security_counter(", countermeasure_reread
)
if not (
    0 <= partial_branch < partial_perm_guard < partial_review < partial_complete
    < preconfirmed_init < initial_validation < initial_release_ready
    < initial_ready_fih_encode < initial_ready_fih_check < initial_identity
    < preconfirmed_set < initial_counter
    < dependency < target < swap
    < fail_case < fail_fih_gate < fail_guard < fail_image_ok
    < primary_validation < final_validation < final_gate < response
    < countermeasure < countermeasure_identity < countermeasure_reread
    < countermeasure_update
):
    raise SystemExit(
        f"{loader_path}: paired loader safety gates are in the wrong order"
    )
countermeasure_region = text[countermeasure:countermeasure_update]
if "assert(rc == 0);" in countermeasure_region:
    raise SystemExit(
        f"{loader_path}: Release countermeasure still relies on assert for I/O"
    )
if text.count("RollerPair_AllImagesConfirmed") != 2:
    raise SystemExit(
        f"{loader_path}: both security-counter paths must use the paired gate"
    )
if text.count("RollerPair_ReleaseReadyToCommit") != 2:
    raise SystemExit(
        f"{loader_path}: both security-counter paths must require verified "
        "matching releases"
    )
if text.count("roller_primary_pair_preconfirmed") != 3:
    raise SystemExit(
        f"{loader_path}: failed-staging health gate is not single-source"
    )
if "bool roller_primary_pair_preconfirmed" in text or \
        "if (!roller_primary_pair_preconfirmed)" in text:
    raise SystemExit(
        f"{loader_path}: failed-staging authorization is not FIH encoded"
    )
print("Paired loader derivative order and fail-closed gates verified.")
PY

if [[ "${task_profile}" == "ReleaseOpen" ||
      "${task_profile}" == "ReleaseClosed" ]]; then
  task_manifest_disassembly="$(
    "${task_toolchain_dir}/arm-none-eabi-objdump" -d \
      --disassemble=Roller_ManufacturingManifestInit \
      "${task_binary_dir}/ECU_OEMiROT.elf"
  )"
  task_platform_disassembly="$(
    "${task_toolchain_dir}/arm-none-eabi-objdump" -d \
      --disassemble=boot_platform_init \
      "${task_binary_dir}/ECU_OEMiROT.elf"
  )"
  task_reset_disassembly="$(
    "${task_toolchain_dir}/arm-none-eabi-objdump" -d \
      --disassemble=Reset_Handler \
      "${task_binary_dir}/ECU_OEMiROT.elf"
  )"
  if grep -Eiq '08fff[0-9a-f]{3}' <<<"${task_manifest_disassembly}"; then
    printf '%s manifest path must not read the unreliable HDPL1 UID aperture.\n' "${task_profile}" >&2
    exit 1
  fi
  if grep -Eiq '44024000' <<<"${task_manifest_disassembly}"; then
    printf '%s manifest path must not depend on DBGMCU.\n' "${task_profile}" >&2
    exit 1
  fi
  task_reset_first_call="$(grep -Em1 '[[:space:]]bl(x)?[[:space:]]' \
      <<<"${task_reset_disassembly}" || true)"
  if [[ "${task_reset_first_call}" != *'<SystemInit>'* ]]; then
    printf '%s Reset_Handler must not make a C call before ST reaches the post-ECC SystemInit boundary.\n' "${task_profile}" >&2
    exit 1
  fi
  if grep -Fq 'Roller_ManufacturingManifestInit' <<<"${task_reset_disassembly}"; then
    printf '%s Reset_Handler must not initialize the manufacturing manifest before ECC SRAM is ready.\n' "${task_profile}" >&2
    exit 1
  fi
  if grep -Eiq 'e0001000' <<<"${task_reset_disassembly}"; then
    printf '%s Reset_Handler must not access DWT/CoreDebug.\n' "${task_profile}" >&2
    exit 1
  fi
  if ! grep -Fq 'Roller_ManufacturingManifestInit' <<<"${task_platform_disassembly}"; then
    printf '%s boot_platform_init does not initialize the reviewed manufacturing manifest.\n' "${task_profile}" >&2
    exit 1
  fi
  if ! grep -Eiq '10076484' <<<"${task_manifest_disassembly}"; then
    printf '%s manifest path does not bind the reviewed RSS manifest.\n' "${task_profile}" >&2
    exit 1
  fi
  if ! grep -Fq 'handoff->mcu_uid[0] = ROLLER_DEVICE_UID0;' \
      "${task_binary_dir}/boot_hal_roller_ecu.c"; then
    printf '%s manifest path does not publish the reviewed per-device identity.\n' "${task_profile}" >&2
    exit 1
  fi
  task_full_disassembly="$(
    "${task_toolchain_dir}/arm-none-eabi-objdump" -d \
      "${task_binary_dir}/ECU_OEMiROT.elf"
  )"
  if grep -Eiq 'e0001000|44024000' <<<"${task_full_disassembly}"; then
    printf '%s final ELF contains a debug/trace peripheral aperture.\n' \
      "${task_profile}" >&2
    exit 1
  fi
  printf '%s autonomous boot has no project call before SystemInit and no DWT/DBGMCU dependency; its post-RAM manifest binds the protected manufacturing identity.\n' "${task_profile}"
fi
printf 'OEMiROT profile: %s (CMake %s, OPEN bench policy %s)\n' \
  "${task_profile}" "${task_build_type}" "${task_open_bench}"
