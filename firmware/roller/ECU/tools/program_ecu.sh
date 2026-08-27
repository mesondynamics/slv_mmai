#!/usr/bin/env bash
set -euo pipefail

task_action="${1:-inspect}"
task_confirmation="${2:-}"
task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_cli="${STM32_PROGRAMMER_CLI:-/home/plac/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI}"
task_probe="${STLINK_SERIAL:-066BFF565456857187210935}"
task_secure_image="${task_project_dir}/Secure/build/Debug/ECU_S.elf"
task_nonsecure_image="${task_project_dir}/NonSecure/build/Debug/ECU_NS.elf"
task_backup_root="${ECU_BACKUP_DIR:-${task_project_dir}/artifacts/device-backups}"
task_connect=( -c port=SWD "sn=${task_probe}" mode=UR reset=HWrst )
task_valve_parameter_address="0x080FA000"
task_valve_parameter_size="0x4000"
task_valve_parameter_backup=""

if [[ ! -x "${task_cli}" ]]; then
  echo "STM32CubeProgrammer CLI not found: ${task_cli}" >&2
  exit 2
fi

display_option_bytes() {
  "${task_cli}" "${task_connect[@]}" -ob displ
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
    echo "Debug images are missing; run ./tools/build.sh Debug first." >&2
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

backup_valve_parameters() {
  local task_timestamp
  task_timestamp="$(date -u +'%Y%m%dT%H%M%SZ')"
  umask 077
  mkdir -p -- "${task_backup_root}"
  task_valve_parameter_backup="${task_backup_root}/stm32h563-${task_probe}-${task_timestamp}-valve-parameters-before.bin"
  "${task_cli}" "${task_connect[@]}" -u \
    "${task_valve_parameter_address}" "${task_valve_parameter_size}" \
    "${task_valve_parameter_backup}"
  sha256sum "${task_valve_parameter_backup}" > \
    "${task_valve_parameter_backup}.sha256"
  printf 'Valve parameter sectors backed up before firmware update:\n  %s\n' \
    "${task_valve_parameter_backup}"
}

verify_valve_parameters_unchanged() {
  local task_after
  task_after="${task_valve_parameter_backup%-before.bin}-after.bin"
  "${task_cli}" "${task_connect[@]}" -u \
    "${task_valve_parameter_address}" "${task_valve_parameter_size}" \
    "${task_after}"
  sha256sum "${task_after}" > "${task_after}.sha256"
  if ! cmp -s -- "${task_valve_parameter_backup}" "${task_after}"; then
    printf 'ERROR: firmware update changed reserved valve parameter sectors.\nPre-update data remains at:\n  %s\n' \
      "${task_valve_parameter_backup}" >&2
    exit 5
  fi
  printf 'Valve parameter sectors verified unchanged after firmware update.\n'
}

flash_images() {
  local task_preserve_parameters="${1:-yes}"
  check_images
  if [[ "${task_preserve_parameters}" == "yes" ]]; then
    backup_valve_parameters
  fi
  "${task_cli}" "${task_connect[@]}" -d "${task_secure_image}" -v
  "${task_cli}" "${task_connect[@]}" -d "${task_nonsecure_image}" -v
  if [[ "${task_preserve_parameters}" == "yes" ]]; then
    verify_valve_parameters_unchanged
  fi
  "${task_cli}" "${task_connect[@]}" -rst
}

case "${task_action}" in
  inspect)
    display_option_bytes
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
