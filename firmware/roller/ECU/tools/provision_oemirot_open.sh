#!/usr/bin/env bash
set -euo pipefail

# Provision the reviewed STM32H563 ECU with an OEMiROT boot chain while the
# product remains OPEN for bench validation.  The final CLOSED transition is a
# separate, deliberately gated operation after Ethernet OTA acceptance and an
# offline PKI backup.

task_action="${1:-inspect}"
task_confirmation="${2:-}"
task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_cli="${STM32_PROGRAMMER_CLI:-/home/plac/.local/share/stm32cube/bundles/programmer/2.23.0/bin/STM32_Programmer_CLI}"
task_probe="${STLINK_SERIAL:-066BFF565456857187210935}"
task_version="${ECU_INITIAL_VERSION:-1.0.1}"
task_pki_dir="${ECU_PKI_DIR:-/home/plac/.local/share/roller-ecu-pki}"
task_backup_root="${ECU_BACKUP_DIR:-${task_project_dir}/artifacts/device-backups}"
task_firmware_dir="${task_project_dir}/artifacts/firmware/${task_version}"
task_provisioning_dir="${task_project_dir}/artifacts/security-provisioning"
task_boot_image="${task_project_dir}/Bootloader/OEMiROT/build/ReleaseOpen/ECU_OEMiROT.bin"
task_secure_image="${task_firmware_dir}/secure-initial.bin"
task_nonsecure_image="${task_firmware_dir}/nonsecure-initial.bin"
task_imgtool="/home/plac/STM32Cube/Repository/STM32Cube_FW_H5_V1.7.0/Middlewares/Third_Party/mcuboot/scripts/imgtool.py"
task_connect_hotplug=( -c port=SWD "sn=${task_probe}" ap=1 mode=Hotplug )
task_connect_reset=( -c port=SWD "sn=${task_probe}" ap=1 mode=UR reset=HWrst )
task_persistent_address="0x0C0E0000"
task_persistent_size="0x1E000"
task_backup_prefix=""
task_persistent_backup=""
task_phase_file=""

strip_terminal_sequences() {
  sed -E $'s/\x1B\[[0-9;]*[mK]//g'
}

record_phase() {
  if [[ -n "${task_phase_file}" ]]; then
    printf '%s\n' "$1" >> "${task_phase_file}"
  fi
  printf 'PHASE: %s\n' "$1"
}

failure_notice() {
  local task_status=$?
  if [[ ${task_status} -ne 0 && -n "${task_backup_prefix}" ]]; then
    printf '\nOEMiROT provisioning stopped safely. Do not power-cycle repeatedly.\n' >&2
    printf 'Recovery artifacts and the last completed phase are under:\n  %s-*\n' \
      "${task_backup_prefix}" >&2
  fi
  exit "${task_status}"
}
trap failure_notice EXIT

require_file() {
  if [[ ! -f "$1" ]]; then
    printf 'Missing required input: %s\n' "$1" >&2
    exit 2
  fi
}

require_size() {
  local task_actual
  task_actual="$(stat -c '%s' -- "$1")"
  if [[ "${task_actual}" -ne "$2" ]]; then
    printf 'Unexpected size for %s: %s (expected %s)\n' "$1" "${task_actual}" "$2" >&2
    exit 2
  fi
}

display_option_bytes() {
  "${task_cli}" "${task_connect_hotplug[@]}" -ob displ -rst
}

read_option_bytes_plain() {
  # Option-byte inspection under reset otherwise leaves this H563 halted.
  "${task_cli}" "${task_connect_reset[@]}" -ob displ -rst | strip_terminal_sequences
}

read_option_bytes_rss_plain() {
  # With BOOT0 held high the H563 is already running the ROM RSS/SFSP.  An
  # under-reset AP1 connection is not valid in that state; Hot Plug is the
  # connection sequence used by ST's H563 OBKey provisioning scripts.
  "${task_cli}" "${task_connect_hotplug[@]}" -ob displ | strip_terminal_sequences
}

require_option() {
  local task_option_text="$1"
  local task_pattern="$2"
  local task_description="$3"
  if ! printf '%s\n' "${task_option_text}" | grep -Eq "${task_pattern}"; then
    printf 'Option-byte verification failed: %s\n' "${task_description}" >&2
    return 1
  fi
}

check_open_target() {
  local task_options
  task_options="$(read_option_bytes_plain)"
  printf '%s\n' "${task_options}"
  require_option "${task_options}" 'Device ID[[:space:]]*: 0x484([[:space:]]|$)' \
    'target must be STM32H563 device ID 0x484'
  require_option "${task_options}" 'PRODUCT_STATE[[:space:]]*: 0xED[[:space:]]+[(]Open[)]' \
    'product must still be Open'
  require_option "${task_options}" 'TZEN[[:space:]]*: 0xB4[[:space:]]' \
    'TrustZone must be enabled'
}

check_open_rss_target() {
  local task_options
  task_options="$(read_option_bytes_rss_plain)"
  printf '%s\n' "${task_options}"
  require_option "${task_options}" 'Device ID[[:space:]]*: 0x484([[:space:]]|$)' \
    'target must be STM32H563 device ID 0x484'
  require_option "${task_options}" 'BL Version[[:space:]]*: 0xE4([[:space:]]|$)' \
    'BOOT0 must select the STM32H563 ROM bootloader'
  require_option "${task_options}" 'SFSP Version:[[:space:]]+v2[.]5[.]0' \
    'the RSS secure firmware service must be running'
  require_option "${task_options}" 'PRODUCT_STATE[[:space:]]*: 0xED[[:space:]]+[(]Open[)]' \
    'product must still be Open'
  require_option "${task_options}" 'TZEN[[:space:]]*: 0xB4[[:space:]]' \
    'TrustZone must be enabled'
}

verify_final_open_options() {
  local task_options
  task_options="$(read_option_bytes_plain)"
  printf '%s\n' "${task_options}"
  require_option "${task_options}" 'PRODUCT_STATE[[:space:]]*: 0xED[[:space:]]+[(]Open[)]' \
    'bench image must remain Open'
  require_option "${task_options}" 'BOOT_UBE[[:space:]]*: 0xB4[[:space:]]' \
    'OEM-iROT unique boot entry must be selected'
  require_option "${task_options}" 'SWAP_BANK[[:space:]]*: 0x0[[:space:]]' \
    'bank swap must be disabled'
  require_option "${task_options}" 'TZEN[[:space:]]*: 0xB4[[:space:]]' \
    'TrustZone must be enabled'
  require_option "${task_options}" 'SECBOOT_LOCK[[:space:]]*: 0xB4[[:space:]]' \
    'Secure boot address must be locked'
  require_option "${task_options}" 'SECBOOTADD[[:space:]]*: 0xC0000[[:space:]]+[(]0x[Cc]000000[)]' \
    'Secure boot address must be 0x0C000000'
  require_option "${task_options}" 'SECWM1_STRT[[:space:]]*: 0x0[[:space:]]' \
    'Bank 1 Secure watermark start must be sector 0'
  require_option "${task_options}" 'SECWM1_END[[:space:]]*: 0x7F[[:space:]]' \
    'all of Bank 1 must be Secure'
  require_option "${task_options}" 'SECWM2_STRT[[:space:]]*: 0x1[[:space:]]' \
    'Bank 2 Secure watermark must be disabled'
  require_option "${task_options}" 'SECWM2_END[[:space:]]*: 0x0[[:space:]]' \
    'Bank 2 Secure watermark must be disabled'
  require_option "${task_options}" 'WRPSGn1[[:space:]]*: 0xFFFFFFF0[[:space:]]' \
    'the four 32-KiB OEMiROT groups must be write-protected'
  require_option "${task_options}" 'HDP1_STRT[[:space:]]*: 0x0[[:space:]]' \
    'HDP must begin at the boot image'
  require_option "${task_options}" 'HDP1_END[[:space:]]*: 0x17[[:space:]]' \
    'HDP must cover the boot image and scratch sectors'
  require_option "${task_options}" 'SRAM2_RST[[:space:]]*: 0x0[[:space:]]' \
    'Secure SRAM2 must erase on reset'
  require_option "${task_options}" 'SRAM2_ECC[[:space:]]*: 0x0[[:space:]]' \
    'Secure SRAM2 ECC must be enabled'
}

wait_ecu_network() {
  local task_try
  for task_try in $(seq 1 20); do
    if ping -I enp2s0 -c 1 -W 1 172.16.0.11 >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.2
  done
  printf 'ECU Ethernet did not recover after reset.\n' >&2
  return 1
}

check_runtime_authentication() {
  local task_state_file="$1"
  local task_try
  local task_fresh=0
  wait_ecu_network
  for task_try in $(seq 1 25); do
    curl --max-time 3 -fsS \
      http://127.0.0.1:18088/api/state -o "${task_state_file}"
    if python3 - "${task_state_file}" <<'PY'
import json
import sys
state = json.load(open(sys.argv[1], encoding="utf-8"))
raise SystemExit(not (int(state.get("status_age_ms", 999999)) <= 500 and
                      int(state.get("diagnostic_age_ms", 999999)) <= 500))
PY
    then
      task_fresh=1
      break
    fi
    sleep 0.2
  done
  if [[ "${task_fresh}" -ne 1 ]]; then
    printf 'Debug UI did not receive fresh ECU status after reset.\n' >&2
    return 1
  fi
  python3 - "${task_state_file}" <<'PY'
import json
import sys

state = json.load(open(sys.argv[1], encoding="utf-8"))
security = state.get("security") or {}
diagnostic = state.get("diagnostic") or {}
flags = int(security.get("flags", 0))
if security.get("auth_result") != 0 or not flags & (1 << 4) or flags & (1 << 5):
    raise SystemExit("ECU ATECC startup authentication is not healthy")
if security.get("config_locked") != 1 or security.get("data_locked") != 1:
    raise SystemExit("ATECC zones are not locked")
if int(diagnostic.get("requested_relay_mask", -1)) != 0:
    raise SystemExit("refusing provisioning while an actuator is requested")
if int(diagnostic.get("applied_relay_mask", -1)) != 0:
    raise SystemExit("refusing provisioning while an actuator is applied")
print("ATECC startup authentication and zero-output state verified")
PY
}

check_inputs() {
  local task_boot_size
  for task_input in \
    "${task_cli}" "${task_imgtool}" "${task_boot_image}" \
    "${task_secure_image}" "${task_nonsecure_image}" \
    "${task_pki_dir}/oemirot-auth-s-public.pem" \
    "${task_pki_dir}/oemirot-auth-ns-public.pem" \
    "${task_provisioning_dir}/DA_Config.obk" \
    "${task_provisioning_dir}/OEMiRoT_Config.obk" \
    "${task_provisioning_dir}/OEMiRoT_Data.obk" \
    "${task_provisioning_dir}/security-assets.json"; do
    require_file "${task_input}"
  done
  require_size "${task_secure_image}" 196608
  require_size "${task_nonsecure_image}" 327680
  require_size "${task_provisioning_dir}/DA_Config.obk" 108
  require_size "${task_provisioning_dir}/OEMiRoT_Config.obk" 300
  require_size "${task_provisioning_dir}/OEMiRoT_Data.obk" 204
  task_boot_size="$(stat -c '%s' -- "${task_boot_image}")"
  if [[ "${task_boot_size}" -le 0 || "${task_boot_size}" -gt 131072 ]]; then
    printf 'OEMiROT boot image does not fit its 128-KiB partition: %s bytes\n' \
      "${task_boot_size}" >&2
    exit 2
  fi
  PYTHONPATH="${task_project_dir}/tools/imgtool_compat" \
    python3 "${task_imgtool}" verify \
      -k "${task_pki_dir}/oemirot-auth-s-public.pem" "${task_secure_image}"
  PYTHONPATH="${task_project_dir}/tools/imgtool_compat" \
    python3 "${task_imgtool}" verify \
      -k "${task_pki_dir}/oemirot-auth-ns-public.pem" "${task_nonsecure_image}"
  python3 - "${task_provisioning_dir}" <<'PY'
import hashlib
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
manifest = json.loads((root / "security-assets.json").read_text())
for name in ("DA_Config.obk", "OEMiRoT_Config.obk", "OEMiRoT_Data.obk"):
    actual = hashlib.sha256((root / name).read_bytes()).hexdigest()
    if manifest["files"].get(name) != actual:
        raise SystemExit(f"security asset hash mismatch: {name}")
if (manifest.get("debug_reopening") is not True or
        manifest.get("debug_scope") != "HDPL3 secure and nonsecure" or
        manifest.get("full_regression") is not True or
        manifest.get("partial_regression") is not False or
        manifest.get("permission_mask") != "0x00004040"):
    raise SystemExit("DA field-service policy is not the reviewed minimum")
print("OEMiROT images, OBKeys, and field-service DA policy verified")
PY
}

create_backup() {
  local task_timestamp
  task_timestamp="$(date -u +'%Y%m%dT%H%M%SZ')"
  umask 077
  mkdir -p -- "${task_backup_root}"
  task_backup_prefix="${task_backup_root}/stm32h563-${task_probe}-${task_timestamp}-before-oemirot"
  task_phase_file="${task_backup_prefix}-phase.txt"
  task_persistent_backup="${task_backup_prefix}-persistent.bin"
  read_option_bytes_plain > "${task_backup_prefix}-option-bytes.txt"
  "${task_cli}" "${task_connect_reset[@]}" \
    -u 0x08000000 0x200000 "${task_backup_prefix}-flash.bin"
  "${task_cli}" "${task_connect_reset[@]}" \
    -u 0x080E0000 "${task_persistent_size}" "${task_persistent_backup}"
  sha256sum "${task_backup_prefix}-flash.bin" "${task_persistent_backup}" \
    > "${task_backup_prefix}-sha256.txt"
  # Upload operations under hardware reset leave the core stopped.
  "${task_cli}" "${task_connect_reset[@]}" -rst
  check_runtime_authentication "${task_backup_prefix}-runtime-security.json"
  record_phase backup_complete
}

program_obkey() {
  local task_name="$1"
  local task_file="$2"
  if grep -Fqx "${task_name}_provisioned" "${task_phase_file}"; then
    printf 'OBKey phase already complete, skipping: %s\n' "${task_name}"
    return 0
  fi
  # In OPEN product state the H563 RSS provisioning service is selected by
  # sampling BOOT0 high.  Match ST's H563 sequence: hard reset in Hot Plug,
  # then invoke SDP without another reset.
  "${task_cli}" "${task_connect_hotplug[@]}" -hardRst
  "${task_cli}" "${task_connect_hotplug[@]}" -sdp "${task_file}"
  record_phase "${task_name}_provisioned"
}

select_resume_backup() {
  local task_latest
  if [[ -n "${ECU_PROVISION_BACKUP_PREFIX:-}" ]]; then
    task_backup_prefix="${ECU_PROVISION_BACKUP_PREFIX}"
  else
    task_latest="$(find "${task_backup_root}" -maxdepth 1 -type f \
      -name 'stm32h563-*-before-oemirot-phase.txt' -printf '%T@ %p\n' \
      | sort -n | tail -n 1 | cut -d' ' -f2-)"
    if [[ -z "${task_latest}" ]]; then
      printf 'No resumable OEMiROT phase journal was found.\n' >&2
      exit 2
    fi
    task_backup_prefix="${task_latest%-phase.txt}"
  fi
  task_phase_file="${task_backup_prefix}-phase.txt"
  task_persistent_backup="${task_backup_prefix}-persistent.bin"
  require_file "${task_phase_file}"
  require_file "${task_persistent_backup}"
}

require_completed_phase() {
  if ! grep -Fqx "$1" "${task_phase_file}"; then
    printf 'Resume journal has not completed required phase: %s\n' "$1" >&2
    exit 3
  fi
}

provision_obkeys_with_boot0_high() {
  check_inputs
  select_resume_backup
  require_completed_phase boot_write_and_hide_protection_enabled
  check_open_rss_target
  program_obkey da_config "${task_provisioning_dir}/DA_Config.obk"
  program_obkey oemirot_config "${task_provisioning_dir}/OEMiRoT_Config.obk"
  program_obkey oemirot_data "${task_provisioning_dir}/OEMiRoT_Data.obk"
  record_phase obkeys_complete_awaiting_boot0_open
  printf '\nOBKeys provisioned. Open JP1 (BOOT0_SET) before resetting the ECU.\n'
}

complete_after_boot0_open() {
  select_resume_backup
  require_completed_phase obkeys_complete_awaiting_boot0_open
  "${task_cli}" "${task_connect_reset[@]}" -rst
  sleep 2
  verify_final_open_options
  check_runtime_authentication "${task_backup_prefix}-runtime-security-after.json"
  record_phase open_oemirot_provisioning_complete
  printf '\nOEMiROT provisioned in OPEN bench state. Full Flash backup:\n  %s-flash.bin\n' \
    "${task_backup_prefix}"
}

repair_oemirot_layout() {
  check_inputs
  select_resume_backup
  require_completed_phase obkeys_complete_awaiting_boot0_open
  check_open_target

  # The first bench image was compiled before the product headers were forced
  # ahead of ST's quoted reference-board includes.  Replace only OEMiROT; the
  # application slots, persistent state, and provisioned OBKeys are untouched.
  "${task_cli}" "${task_connect_reset[@]}" \
    -ob WRPSGn1=0xFFFFFFFF WRPSGn2=0xFFFFFFFF \
        HDP1_STRT=0x1 HDP1_END=0x0 HDP2_STRT=0x1 HDP2_END=0x0
  record_phase boot_protection_temporarily_removed_for_layout_repair

  "${task_cli}" "${task_connect_reset[@]}" \
    -d "${task_boot_image}" 0x0C000000 -v
  record_phase canonical_layout_boot_programmed

  "${task_cli}" "${task_connect_reset[@]}" \
    -ob WRPSGn1=0xFFFFFFF0 WRPSGn2=0xFFFFFFFF \
        HDP1_STRT=0x0 HDP1_END=0x17 HDP2_STRT=0x1 HDP2_END=0x0
  record_phase canonical_boot_write_and_hide_protection_enabled
  verify_final_open_options
  printf '\nCanonical-layout OEMiROT programmed. Run complete-after-boot0-open.\n'
}

repair_application_vectors() {
  check_inputs
  select_resume_backup
  require_completed_phase canonical_boot_write_and_hide_protection_enabled
  check_open_target

  # Replace only the paired primary images.  The signed files are fixed-size
  # MCUboot slot images and check_inputs has verified both ECDSA signatures.
  "${task_cli}" "${task_connect_reset[@]}" \
    -d "${task_secure_image}" 0x0C030000 -v
  record_phase secure_primary_vtor_repair_programmed
  "${task_cli}" "${task_connect_reset[@]}" \
    -d "${task_nonsecure_image}" 0x08100000 -v
  record_phase nonsecure_primary_vtor_repair_programmed
  printf '\nPaired application vectors repaired. Run complete-after-boot0-open.\n'
}

provision_open() {
  check_inputs
  check_open_target
  create_backup

  # This sequence is the STM32CubeH5 v1.7.0 NUCLEO-H563ZI OEMiROT
  # provisioning order, with the reviewed Roller ECU Flash geometry.
  "${task_cli}" "${task_connect_hotplug[@]}" -ob TZEN=0xB4
  "${task_cli}" "${task_connect_reset[@]}"
  "${task_cli}" "${task_connect_reset[@]}" \
    -ob SECWM1_STRT=0x1 SECWM1_END=0x0 \
        WRPSGn1=0xFFFFFFFF WRPSGn2=0xFFFFFFFF \
        SECWM2_STRT=0x1 SECWM2_END=0x0 \
        HDP1_STRT=0x1 HDP1_END=0x0 HDP2_STRT=0x1 HDP2_END=0x0 \
        SECBOOT_LOCK=0xC3 SECBOOTADD=0xC0000 SWAP_BANK=0 \
        SRAM2_RST=0 SRAM2_ECC=0 SRAM3_ECC=1 BOOT_UBE=0xB4 \
    -e all
  record_phase flash_erased_and_protections_removed

  "${task_cli}" "${task_connect_reset[@]}" \
    -ob SECWM1_STRT=0x0 SECWM1_END=0x7F \
        SECWM2_STRT=0x1 SECWM2_END=0x0
  record_phase secure_watermarks_configured

  "${task_cli}" "${task_connect_reset[@]}" \
    -d "${task_secure_image}" 0x0C030000 -v
  "${task_cli}" "${task_connect_reset[@]}" \
    -d "${task_nonsecure_image}" 0x08100000 -v
  "${task_cli}" "${task_connect_reset[@]}" \
    -d "${task_persistent_backup}" "${task_persistent_address}" -v
  "${task_cli}" "${task_connect_reset[@]}" \
    -d "${task_boot_image}" 0x0C000000 -v
  record_phase images_and_persistent_state_programmed

  "${task_cli}" "${task_connect_reset[@]}" \
    -ob WRPSGn1=0xFFFFFFF0 WRPSGn2=0xFFFFFFFF \
        HDP1_STRT=0x0 HDP1_END=0x17 HDP2_STRT=0x1 HDP2_END=0x0 \
        SECBOOT_LOCK=0xB4
  record_phase boot_write_and_hide_protection_enabled
  record_phase awaiting_boot0_bridge_for_obkeys
  printf '\nFlash provisioning complete. Temporarily close JP1 (BOOT0_SET), then run:\n'
  printf '  %s provision-obkeys --accept-boot0-high\n' "$0"
}

case "${task_action}" in
  inspect)
    display_option_bytes
    ;;
  preflight)
    check_inputs
    check_open_target
    check_runtime_authentication /tmp/roller-ecu-oemirot-preflight.json
    ;;
  provision-open)
    if [[ "${task_confirmation}" != "--accept-full-flash-erase" ]]; then
      printf '%s\n' \
        'This operation backs up, mass-erases, and rebuilds MCU Flash, provisions' \
        'OEMiROT/DA OBKeys, and enables boot WRP/HDP. It deliberately leaves the' \
        'product OPEN. Re-run with:' \
        '  ./tools/provision_oemirot_open.sh provision-open --accept-full-flash-erase' >&2
      exit 4
    fi
    provision_open
    ;;
  provision-obkeys)
    if [[ "${task_confirmation}" != "--accept-boot0-high" ]]; then
      printf '%s\n' \
        'Temporarily close the PCB solder jumper JP1 (BOOT0_SET) so BOOT0 is' \
        'sampled high, then re-run with:' \
        '  ./tools/provision_oemirot_open.sh provision-obkeys --accept-boot0-high' >&2
      exit 4
    fi
    provision_obkeys_with_boot0_high
    ;;
  complete-after-boot0-open)
    complete_after_boot0_open
    ;;
  repair-boot-layout)
    if [[ "${task_confirmation}" != "--accept-replace-oemirot" ]]; then
      printf '%s\n' \
        'This replaces only the OPEN-state OEMiROT boot partition after temporarily' \
        'removing and restoring its WRP/HDP protection. Re-run with:' \
        '  ./tools/provision_oemirot_open.sh repair-boot-layout --accept-replace-oemirot' >&2
      exit 4
    fi
    repair_oemirot_layout
    ;;
  repair-app-vectors)
    if [[ "${task_confirmation}" != "--accept-replace-primary-images" ]]; then
      printf '%s\n' \
        'This replaces the signed Secure and NonSecure primary slot images.' \
        'Re-run with:' \
        '  ./tools/provision_oemirot_open.sh repair-app-vectors --accept-replace-primary-images' >&2
      exit 4
    fi
    repair_application_vectors
    ;;
  *)
    printf 'Usage: %s {inspect|preflight|provision-open|provision-obkeys|repair-boot-layout|repair-app-vectors|complete-after-boot0-open}\n' "$0" >&2
    exit 2
    ;;
esac

trap - EXIT
