#!/usr/bin/env bash
set -euo pipefail

task_action="${1:-inspect}"
task_confirmation="${2:-}"
task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_cli="${STM32_PROGRAMMER_CLI:-/home/plac/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI}"
task_probe="${STLINK_SERIAL:-066BFF565456857187210935}"
task_build_type="${ECU_BUILD_TYPE:-Debug}"
task_build_variant="${ECU_BUILD_VARIANT:-${task_build_type}}"
task_secure_image="${task_project_dir}/Secure/build/${task_build_variant}/ECU_S.elf"
task_nonsecure_image="${task_project_dir}/NonSecure/build/${task_build_variant}/ECU_NS.elf"
task_backup_root="${ECU_BACKUP_DIR:-${task_project_dir}/artifacts/device-backups}"
task_connect=( -c port=SWD "sn=${task_probe}" mode=UR reset=HWrst )
task_inspect_connect=( -c port=SWD "sn=${task_probe}" mode=Hotplug )
task_persistent_address="0x080E0000"
# The development linker places its NSC veneer in the final Bank-1 sector at
# 0x080FE000. Identity, OTA journal, and valve records end immediately before
# that sector; signed/OEMiROT builds relocate NSC but use the same data range.
task_persistent_size="0x1E000"
task_persistent_backup=""

if [[ ! -x "${task_cli}" ]]; then
  echo "STM32CubeProgrammer CLI not found: ${task_cli}" >&2
  exit 2
fi

display_option_bytes() {
  "${task_cli}" "${task_connect[@]}" -ob displ
}

display_option_bytes_running() {
  # CubeProgrammer halts this H563 even in Hot Plug mode while reading option
  # bytes. Reset in the same transaction so inspection never leaves the ECU
  # paused. This is intentionally separate from the under-reset write path.
  "${task_cli}" "${task_inspect_connect[@]}" -ob displ -rst
}

strip_terminal_sequences() {
  sed -E $'s/\\x1B\\[[0-9;]*[mK]//g'
}

check_target_identity() {
  local task_output
  local task_plain
  task_output="$(display_option_bytes)"
  printf '%s\n' "${task_output}"
  task_plain="$(printf '%s\n' "${task_output}" | strip_terminal_sequences)"
  if ! printf '%s\n' "${task_plain}" | grep -Eq 'Device ID[[:space:]]*: 0x484([[:space:]]|$)'; then
    echo "Refusing to write: connected target is not the reviewed STM32H563 (ID 0x484)." >&2
    exit 3
  fi
  if ! printf '%s\n' "${task_plain}" | grep -Eq 'PRODUCT_STATE[[:space:]]*: 0xED[[:space:]]+[(]Open[)]'; then
    echo "Refusing to write: target Product State is not the reviewed Open state." >&2
    exit 3
  fi
}

backup_existing_flash() {
  local task_timestamp
  local task_prefix
  task_timestamp="$(date -u +'%Y%m%dT%H%M%SZ')"
  task_prefix="${task_backup_root}/stm32h563-${task_probe}-${task_timestamp}"
  umask 077
  mkdir -p -- "${task_backup_root}"
  display_option_bytes | strip_terminal_sequences > "${task_prefix}-option-bytes.txt"
  "${task_cli}" "${task_connect[@]}" \
    -u 0x08000000 0x200000 "${task_prefix}-flash.bin"
  sha256sum "${task_prefix}-flash.bin" > "${task_prefix}-flash.bin.sha256"
  printf 'Existing 2 MiB Flash and Option Bytes backed up under:\n  %s-*\n' "${task_prefix}"
}

check_images() {
  if [[ ! -f "${task_secure_image}" || ! -f "${task_nonsecure_image}" ]]; then
    echo "${task_build_type} images are missing; run ./tools/build.sh ${task_build_type} first." >&2
    exit 2
  fi
}

check_trustzone_layout() {
  local task_output
  local task_plain
  task_output="$(display_option_bytes)"
  printf '%s\n' "${task_output}"
  task_plain="$(printf '%s\n' "${task_output}" | strip_terminal_sequences)"
  if ! printf '%s\n' "${task_plain}" | grep -Eq 'TZEN[[:space:]]*: 0xB4[[:space:]]'; then
    echo "Refusing to flash: TZEN is not enabled (0xB4)." >&2
    exit 3
  fi
  if ! printf '%s\n' "${task_plain}" | grep -Eq 'SECBOOTADD[[:space:]]*: 0xC0000[[:space:]]+[(]0x[Cc]000000[)]'; then
    echo "Refusing to flash: SECBOOTADD is not the Secure image base 0x0C000000." >&2
    exit 3
  fi
  if ! printf '%s\n' "${task_plain}" | grep -Eq 'SECBOOT_LOCK[[:space:]]*: 0xC3[[:space:]]'; then
    echo "Refusing to flash: secure boot address is not left unlocked for development." >&2
    exit 3
  fi
  if ! printf '%s\n' "${task_plain}" | grep -Eq 'SECWM1_STRT[[:space:]]*: 0x0[[:space:]]' ||
     ! printf '%s\n' "${task_plain}" | grep -Eq 'SECWM1_END[[:space:]]*: 0x7F[[:space:]]' ||
     ! printf '%s\n' "${task_plain}" | grep -Eq 'SECWM2_STRT[[:space:]]*: 0x1[[:space:]]' ||
     ! printf '%s\n' "${task_plain}" | grep -Eq 'SECWM2_END[[:space:]]*: 0x0[[:space:]]'; then
    echo "Refusing to flash: secure watermarks do not match Bank1 Secure / Bank2 NonSecure." >&2
    exit 3
  fi
}

backup_persistent_storage() {
  local task_timestamp
  task_timestamp="$(date -u +'%Y%m%dT%H%M%SZ')"
  umask 077
  mkdir -p -- "${task_backup_root}"
  task_persistent_backup="${task_backup_root}/stm32h563-${task_probe}-${task_timestamp}-secure-persistent-before.bin"
  "${task_cli}" "${task_connect[@]}" -u \
    "${task_persistent_address}" "${task_persistent_size}" \
    "${task_persistent_backup}"
  sha256sum "${task_persistent_backup}" > \
    "${task_persistent_backup}.sha256"
  printf 'Secure identity, OTA journal, and valve parameters backed up before firmware update:\n  %s\n' \
    "${task_persistent_backup}"
}

verify_persistent_storage_unchanged() {
  local task_after
  task_after="${task_persistent_backup%-before.bin}-after.bin"
  "${task_cli}" "${task_connect[@]}" -u \
    "${task_persistent_address}" "${task_persistent_size}" \
    "${task_after}"
  sha256sum "${task_after}" > "${task_after}.sha256"
  if ! cmp -s -- "${task_persistent_backup}" "${task_after}"; then
    printf 'ERROR: firmware update changed Secure persistent storage.\nPre-update data remains at:\n  %s\n' \
      "${task_persistent_backup}" >&2
    exit 5
  fi
  printf 'Secure persistent storage verified unchanged after firmware update.\n'
}

flash_images() {
  local task_preserve_parameters="${1:-yes}"
  check_images
  if [[ "${task_preserve_parameters}" == "yes" ]]; then
    backup_persistent_storage
  fi
  "${task_cli}" "${task_connect[@]}" -d "${task_secure_image}" -v
  "${task_cli}" "${task_connect[@]}" -d "${task_nonsecure_image}" -v
  if [[ "${task_preserve_parameters}" == "yes" ]]; then
    verify_persistent_storage_unchanged
  fi
  "${task_cli}" "${task_connect[@]}" -rst
}

case "${task_action}" in
  inspect)
    display_option_bytes_running
    ;;
  backup)
    check_target_identity
    backup_existing_flash
    ;;
  flash)
    check_target_identity
    check_trustzone_layout
    flash_images yes
    ;;
  provision-and-flash)
    if [[ "${task_confirmation}" != "--accept-existing-flash-erase" ]]; then
      cat >&2 <<'EOF'
This action enables TrustZone and changes secure watermarks. It can erase or
invalidate all existing MCU Flash content. Re-run only after authorization:
  ./tools/program_ecu.sh provision-and-flash --accept-existing-flash-erase
EOF
      exit 4
    fi
    check_images
    check_target_identity
    # This read-back is mandatory and completes before the first destructive
    # Option Bytes transaction. Backups are private and excluded from Git.
    backup_existing_flash
    # RM0481: when TZEN is enabled the core boots from SECBOOTADD.  H563 rejects
    # writes to Secure boot fields while TZEN is disabled, so use ST's official
    # ordering under hardware reset: enable TZEN, then immediately program the
    # Secure alias and Flash partition before releasing/resetting the target.
    "${task_cli}" "${task_connect[@]}" -ob TZEN=0xB4
    "${task_cli}" "${task_connect[@]}" \
      -ob SECBOOTADD=0xC0000 SECBOOT_LOCK=0xC3 \
      SECWM1_STRT=0x0 SECWM1_END=0x7F \
      SECWM2_STRT=0x1 SECWM2_END=0x0
    check_trustzone_layout
    # Provisioning is explicitly authorized to invalidate all prior Flash;
    # the complete pre-migration backup above remains the recovery artifact.
    flash_images no
    ;;
  *)
    echo "Usage: $0 {inspect|backup|flash|provision-and-flash [--accept-existing-flash-erase]}" >&2
    exit 2
    ;;
esac
