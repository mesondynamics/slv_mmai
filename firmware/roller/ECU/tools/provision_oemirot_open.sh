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
task_version="${ECU_INITIAL_VERSION:-1.0.15}"
task_pki_dir="${ECU_PKI_DIR:-/home/plac/.local/share/roller-ecu-pki}"
task_backup_root="${ECU_BACKUP_DIR:-${task_project_dir}/artifacts/device-backups}"
task_firmware_dir="${task_project_dir}/artifacts/firmware/${task_version}"
task_provisioning_dir="${task_project_dir}/artifacts/security-provisioning"
task_boot_image="${task_project_dir}/Bootloader/OEMiROT/build/ReleaseOpen/ECU_OEMiROT.bin"
task_closed_boot_image="${task_project_dir}/Bootloader/OEMiROT/build/ReleaseClosed/ECU_OEMiROT.bin"
task_secure_image="${task_firmware_dir}/secure-initial.bin"
task_nonsecure_image="${task_firmware_dir}/nonsecure-initial.bin"
task_recovery_ota_package="${task_firmware_dir}/roller-ecu-${task_version}.recu"
task_imgtool="/home/plac/STM32Cube/Repository/STM32Cube_FW_H5_V1.7.0/Middlewares/Third_Party/mcuboot/scripts/imgtool.py"
task_connect_hotplug=( -c port=SWD "sn=${task_probe}" ap=1 mode=Hotplug )
task_connect_reset=( -c port=SWD "sn=${task_probe}" ap=1 mode=UR reset=HWrst )
task_persistent_address="0x0C0E0000"
task_persistent_size="0x20000"
task_backup_prefix=""
task_persistent_backup=""
task_phase_file=""
task_recovery_dir=""
task_recovery_manifest=""
task_open_loader_dir=""
task_open_loader_phase=""
task_open_loader_manifest=""
task_open_loader_staged_boot=""
task_open_loader_staged_package=""
task_open_loader_package="${task_project_dir}/artifacts/firmware/1.0.14/roller-ecu-1.0.14.recu"
task_open_loader_metadata="${task_project_dir}/artifacts/firmware/1.0.14/metadata.json"
task_runtime_preflight_verifier="${task_script_dir}/verify_closed_preflight.py"
task_pairing_verifier="${task_script_dir}/verify_pairing_store.py"
task_expected_mcu_uid="003800613434511232383537"
task_expected_atecc_serial="0123d47eb2ee0e9bee"

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
  if [[ ${task_status} -ne 0 && -n "${task_open_loader_dir}" ]]; then
    if [[ -f "${task_open_loader_manifest}" ]]; then
      printf '\nOPEN OEMiROT replacement stopped. Do not remove ST-Link or cold-start.\n' >&2
      printf 'Inspect the phase journal and resume this exact transaction:\n' >&2
      printf '  ECU_OPEN_LOADER_TRANSACTION_DIR=%q %q resume-open-oemirot-replacement --accept-resume-open-boot-replacement\n' \
        "${task_open_loader_dir}" "$0" >&2
    else
      printf '\nOPEN OEMiROT pre-write checks stopped before a loader was staged.\n' >&2
      printf 'No boot mutation phase is recorded; inspect: %s\n' \
        "${task_open_loader_dir}" >&2
    fi
  elif [[ ${task_status} -ne 0 && -n "${task_backup_prefix}" ]]; then
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

require_persistent_backup_size() {
  local task_actual
  task_actual="$(stat -c '%s' -- "$1")"
  case "${task_actual}" in
    122880|131072) ;;
    *)
      printf 'Unexpected persistent backup size for %s: %s\n' \
        "$1" "${task_actual}" >&2
      exit 2
      ;;
  esac
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

check_regressed_open_target() {
  local task_options
  task_options="$(read_option_bytes_plain)"
  printf '%s\n' "${task_options}"
  require_option "${task_options}" 'Device ID[[:space:]]*: 0x484([[:space:]]|$)' \
    'recovery target must be STM32H563 device ID 0x484'
  require_option "${task_options}" 'PRODUCT_STATE[[:space:]]*: 0xED[[:space:]]+[(]Open[)]' \
    'Full Regression must have returned the target to OPEN'
}

check_blank_flash_sample() {
  local task_sample_dir
  local task_sample
  task_sample_dir="$(mktemp -d)"
  task_sample="${task_sample_dir}/flash-sample.bin"
  if ! "${task_cli}" "${task_connect_reset[@]}" \
      -u 0x08000000 0x1000 "${task_sample}"; then
    rm -rf -- "${task_sample_dir}"
    printf 'Cannot read the post-regression Flash blank sample.\n' >&2
    return 1
  fi
  if ! python3 - "${task_sample}" <<'PY'
import pathlib
import sys

sample = pathlib.Path(sys.argv[1]).read_bytes()
if len(sample) != 4096 or any(value != 0xFF for value in sample):
    raise SystemExit("Flash is not blank; refusing the recovery mass-erase path")
print("Post-regression Flash blank sample verified (4096 bytes of 0xFF)")
PY
  then
    rm -rf -- "${task_sample_dir}"
    return 1
  fi
  rm -rf -- "${task_sample_dir}"
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

check_open_identity_during_recovery() {
  local task_options
  task_options="$(read_option_bytes_plain)"
  printf '%s\n' "${task_options}"
  require_option "${task_options}" 'Device ID[[:space:]]*: 0x484([[:space:]]|$)' \
    'OPEN loader transaction is bound to STM32H563 device ID 0x484'
  require_option "${task_options}" 'PRODUCT_STATE[[:space:]]*: 0xED[[:space:]]+[(]Open[)]' \
    'OPEN loader transaction cannot run in PROVISIONING or CLOSED'
  require_option "${task_options}" 'TZEN[[:space:]]*: 0xB4[[:space:]]' \
    'TrustZone must remain enabled during recovery'
  require_option "${task_options}" 'SECBOOTADD[[:space:]]*: 0xC0000[[:space:]]+[(]0x[Cc]000000[)]' \
    'Secure boot address changed during the OPEN loader transaction'
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
    if curl --max-time 3 -fsS \
        http://127.0.0.1:18088/api/state -o "${task_state_file}" &&
       python3 "${task_runtime_preflight_verifier}" runtime-state \
         "${task_state_file}" >/dev/null 2>&1
    then
      task_fresh=1
      break
    fi
    sleep 0.2
  done
  if [[ "${task_fresh}" -ne 1 ]]; then
    printf 'Debug UI did not receive a fresh exact ECU/ATECC/confirmation state after reset.\n' >&2
    return 1
  fi
  python3 "${task_runtime_preflight_verifier}" runtime-state \
    "${task_state_file}"
}

check_open_loader_replacement_inputs() {
  local task_boot_size
  local task_boot_hash
  require_file "${task_cli}"
  require_file "${task_boot_image}"
  require_file "${task_imgtool}"
  require_file "${task_pki_dir}/oemirot-auth-s-public.pem"
  require_file "${task_pki_dir}/oemirot-auth-ns-public.pem"
  require_file "${task_pki_dir}/ota-transport-public.pem"
  require_file "${task_script_dir}/verify_open_loader_backup.py"
  require_file "${task_runtime_preflight_verifier}"
  require_file "${task_open_loader_package}"
  require_file "${task_open_loader_metadata}"
  task_boot_size="$(stat -c '%s' -- "${task_boot_image}")"
  task_boot_hash="$(sha256sum "${task_boot_image}" | awk '{print $1}')"
  if [[ "${task_boot_size}" -ne 52276 ||
        "${task_boot_hash}" != \
        "5fdaaa3f5e2d2a6c01ed527282ba08b3d8d5e579c61ec3c4e097cdf2c0fd7761" ]]; then
    printf 'ReleaseOpen OEMiROT is not the reviewed 52276-byte artifact: %s %s\n' \
      "${task_boot_size}" "${task_boot_hash}" >&2
    return 1
  fi
  python3 - "${task_open_loader_metadata}" <<'PY'
import json
import sys

metadata = json.load(open(sys.argv[1], encoding="utf-8"))
if (metadata.get("format") != "roller-ecu-ota-v1" or
        metadata.get("version") != "1.0.14" or
        metadata.get("update_sequence") != 14 or
        metadata.get("security_counter") != 14):
    raise SystemExit("the mandatory post-loader OTA package is not release 1.0.14/sequence 14/counter 14")
print("Mandatory post-loader OTA release 1.0.14 identity verified")
PY
  PYTHONPATH="${task_script_dir}" python3 - \
      "${task_open_loader_package}" \
      "${task_pki_dir}/ota-transport-public.pem" <<'PY'
import sys
from pathlib import Path

from ethernet_ota import load_package

_begin, secure, nonsecure, metadata = load_package(
    Path(sys.argv[1]), Path(sys.argv[2]))
if (metadata.get("version"), metadata.get("update_sequence"),
        metadata.get("security_counter")) != ("1.0.14", 14, 14):
    raise SystemExit("signed OTA package identity is not release 1.0.14/14/14")
if len(secure) != 196608 or len(nonsecure) != 327680:
    raise SystemExit("signed OTA package does not use the reviewed paired slot sizes")
print("Mandatory OTA package signature, metadata, and paired payload hashes verified")
PY
  printf 'ReleaseOpen candidate (offline only):\n'
  sha256sum "${task_boot_image}" "${task_open_loader_package}"
  printf 'No target connection was attempted. Before the confirmed action, keep JP1 open and connect SWD/GND/VTref/NRST.\n'
}

open_loader_phase_done() {
  [[ -f "${task_open_loader_phase}" ]] && grep -Fqx "$1" "${task_open_loader_phase}"
}

record_open_loader_phase() {
  printf '%s\n' "$1" >> "${task_open_loader_phase}"
  printf 'OPEN-LOADER PHASE: %s\n' "$1"
}

create_open_loader_transaction() {
  local task_timestamp
  task_timestamp="$(date -u +'%Y%m%dT%H%M%SZ')"
  umask 077
  mkdir -p -- "${task_backup_root}"
  task_open_loader_dir="${task_backup_root}/stm32h563-${task_probe}-${task_timestamp}-open-loader-replacement"
  if [[ -e "${task_open_loader_dir}" ]]; then
    printf 'OPEN loader transaction already exists: %s\n' "${task_open_loader_dir}" >&2
    return 1
  fi
  mkdir -- "${task_open_loader_dir}"
  task_open_loader_phase="${task_open_loader_dir}/phase.txt"
  task_open_loader_manifest="${task_open_loader_dir}/manifest.json"
  task_open_loader_staged_boot="${task_open_loader_dir}/ReleaseOpen-ECU_OEMiROT.bin"
  task_open_loader_staged_package="${task_open_loader_dir}/roller-ecu-1.0.14.recu"
  : > "${task_open_loader_phase}"
  record_open_loader_phase transaction_created
}

select_open_loader_transaction() {
  if [[ -z "${ECU_OPEN_LOADER_TRANSACTION_DIR:-}" ]]; then
    printf 'Set ECU_OPEN_LOADER_TRANSACTION_DIR to the exact OPEN loader transaction directory.\n' >&2
    return 1
  fi
  task_open_loader_dir="$(realpath -e -- "${ECU_OPEN_LOADER_TRANSACTION_DIR}")"
  task_open_loader_phase="${task_open_loader_dir}/phase.txt"
  task_open_loader_manifest="${task_open_loader_dir}/manifest.json"
  task_open_loader_staged_boot="${task_open_loader_dir}/ReleaseOpen-ECU_OEMiROT.bin"
  task_open_loader_staged_package="${task_open_loader_dir}/roller-ecu-1.0.14.recu"
  require_file "${task_open_loader_phase}"
  require_file "${task_open_loader_manifest}"
  require_file "${task_open_loader_staged_boot}"
  require_file "${task_open_loader_staged_package}"
}

verify_programmer_mcu_uid() {
  local task_output="$1"
  "${task_cli}" "${task_connect_reset[@]}" -u 0x08FFF800 0xC "${task_output}"
  python3 - "${task_output}" "${task_expected_mcu_uid}" <<'PY'
import struct
import sys

data = open(sys.argv[1], "rb").read()
if len(data) != 12:
    raise SystemExit("Programmer MCU UID upload is not exactly 12 bytes")
actual = "".join(f"{word:08x}" for word in struct.unpack("<III", data))
if actual != sys.argv[2]:
    raise SystemExit(f"Programmer MCU UID mismatch: {actual}")
print(f"Programmer MCU UID verified: {actual}")
PY
}

check_ota_idle() {
  local task_output="$1"
  python3 "${task_script_dir}/ethernet_ota.py" --status > "${task_output}"
  python3 - "${task_output}" <<'PY'
import json
import sys

status = json.load(open(sys.argv[1], encoding="utf-8"))
if status.get("result") != 0 or status.get("ota_result") != 0 or status.get("state") != 0:
    raise SystemExit("ECU OTA service is not idle and healthy")
print(f"OTA idle verified; accepted_sequence={status.get('accepted_sequence')}")
PY
}

read_runtime_uptime() {
  python3 - "$1" <<'PY'
import json
import sys
state = json.load(open(sys.argv[1], encoding="utf-8"))
print(int((state.get("diagnostic") or {}).get("secure_uptime_ms", -1)))
PY
}

check_open_loader_nrst_and_normal_boot() {
  local task_before="${task_open_loader_dir}/nrst-before.json"
  local task_after="${task_open_loader_dir}/nrst-after.json"
  local task_before_uptime
  local task_after_uptime
  local task_try

  check_runtime_authentication "${task_before}"
  for task_try in $(seq 1 30); do
    curl --max-time 3 -fsS http://127.0.0.1:18088/api/state -o "${task_before}"
    task_before_uptime="$(read_runtime_uptime "${task_before}")"
    [[ "${task_before_uptime}" -ge 5000 ]] && break
    sleep 0.2
  done
  if [[ "${task_before_uptime}" -lt 5000 ]]; then
    printf 'Cannot establish a stable Secure uptime before the NRST proof.\n' >&2
    return 1
  fi
  "${task_cli}" -c port=SWD "sn=${task_probe}" mode=HWRSTPULSE
  check_runtime_authentication "${task_after}"
  task_after_uptime="$(read_runtime_uptime "${task_after}")"
  if [[ "${task_after_uptime}" -lt 0 || "${task_after_uptime}" -ge "${task_before_uptime}" ]]; then
    printf 'Physical NRST did not reset Secure uptime (%s -> %s).\n' \
      "${task_before_uptime}" "${task_after_uptime}" >&2
    return 1
  fi
  record_open_loader_phase nrst_and_jp1_normal_boot_verified
  printf 'NRST propagated and JP1-low normal application boot was proven (%s -> %s ms).\n' \
    "${task_before_uptime}" "${task_after_uptime}"
}

backup_and_validate_open_loader_target() {
  read_option_bytes_plain > "${task_open_loader_dir}/option-bytes-before.txt"
  "${task_cli}" "${task_connect_reset[@]}" \
    -u 0x08000000 0x200000 "${task_open_loader_dir}/full-flash.bin"
  "${task_cli}" "${task_connect_reset[@]}" \
    -u 0x080E0000 0x20000 "${task_open_loader_dir}/secure-persistent.bin"
  require_size "${task_open_loader_dir}/full-flash.bin" 2097152
  require_size "${task_open_loader_dir}/secure-persistent.bin" 131072
  verify_programmer_mcu_uid "${task_open_loader_dir}/programmer-mcu-uid.bin"
  sha256sum "${task_open_loader_dir}/full-flash.bin" \
    "${task_open_loader_dir}/secure-persistent.bin" \
    "${task_open_loader_dir}/programmer-mcu-uid.bin" \
    "${task_open_loader_dir}/option-bytes-before.txt" \
    > "${task_open_loader_dir}/backup-sha256.txt"
  "${task_cli}" "${task_connect_reset[@]}" -rst
  check_runtime_authentication "${task_open_loader_dir}/runtime-before.json"
  check_ota_idle "${task_open_loader_dir}/ota-before.json"
  record_open_loader_phase fresh_full_flash_persistent_ob_runtime_backup_complete

  python3 "${task_script_dir}/verify_open_loader_backup.py" \
    --flash "${task_open_loader_dir}/full-flash.bin" \
    --output-dir "${task_open_loader_dir}"
  PYTHONPATH="${task_project_dir}/tools/imgtool_compat" \
    python3 "${task_imgtool}" verify \
      -k "${task_pki_dir}/oemirot-auth-s-public.pem" \
      "${task_open_loader_dir}/secure-primary.bin"
  PYTHONPATH="${task_project_dir}/tools/imgtool_compat" \
    python3 "${task_imgtool}" verify \
      -k "${task_pki_dir}/oemirot-auth-ns-public.pem" \
      "${task_open_loader_dir}/nonsecure-primary.bin"
  sha256sum "${task_open_loader_dir}/secure-primary.bin" \
    "${task_open_loader_dir}/nonsecure-primary.bin" \
    "${task_open_loader_dir}/primary-audit.json" \
    > "${task_open_loader_dir}/primary-sha256.txt"
  record_open_loader_phase paired_primary_signature_identity_trailer_verified
}

stage_open_loader_transaction_inputs() {
  cp -- "${task_boot_image}" "${task_open_loader_staged_boot}"
  cp -- "${task_open_loader_package}" "${task_open_loader_staged_package}"
  python3 - "${task_open_loader_dir}" "${task_probe}" \
      "${task_open_loader_staged_boot}" "${task_open_loader_staged_package}" <<'PY'
import datetime
import hashlib
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
probe = sys.argv[2]
boot = pathlib.Path(sys.argv[3])
package = pathlib.Path(sys.argv[4])

def entry(path):
    data = path.read_bytes()
    return {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}

files = {}
for name in (
        "full-flash.bin", "secure-persistent.bin", "option-bytes-before.txt",
        "programmer-mcu-uid.bin",
        "runtime-before.json", "ota-before.json", "secure-primary.bin",
        "nonsecure-primary.bin", "primary-audit.json",
        "ReleaseOpen-ECU_OEMiROT.bin", "roller-ecu-1.0.14.recu"):
    files[name] = entry(root / name)
manifest = {
    "schema": "roller-ecu-open-loader-replacement-v1",
    "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "target": {
        "probe_serial": probe,
        "device_id": "0x484",
        "product_state": "OPEN",
        "mcu_uid": "003800613434511232383537",
        "atecc608_serial": "0123d47eb2ee0e9bee",
    },
    "files": files,
    "mandatory_next_ota": {
        "version": "1.0.14",
        "update_sequence": 14,
        "security_counter": 14,
        "package_name": "roller-ecu-1.0.14.recu",
        **files["roller-ecu-1.0.14.recu"],
    },
    "physical_hold": {
        "jp1": "OPEN/BOOT0 low",
        "stlink": "must remain connected through confirmed OTA 1.0.14",
        "cold_start_embargo": "do not cold-start before paired 1.0.14 image_ok confirmation",
    },
}
(root / "manifest.json").write_text(
    json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(f"Pinned ReleaseOpen: {files['ReleaseOpen-ECU_OEMiROT.bin']['size']} bytes, "
      f"sha256={files['ReleaseOpen-ECU_OEMiROT.bin']['sha256']}")
print(f"Pinned mandatory OTA 1.0.14: sha256={manifest['mandatory_next_ota']['sha256']}")
PY
  record_open_loader_phase releaseopen_hash_size_and_ota_1_0_14_pinned
  # The next phase may rewrite the only bootable loader.  Force the complete
  # recovery transaction to stable storage before any Option Byte or Flash
  # mutation so a simultaneous host/ECU power loss still leaves resumable
  # inputs rather than page-cache-only evidence.
  sync -f "${task_open_loader_dir}"
  printf 'OPEN loader recovery transaction synchronized to stable storage\n'
}

verify_open_loader_transaction_manifest() {
  python3 - "${task_open_loader_manifest}" "${task_open_loader_dir}" \
      "${task_probe}" <<'PY'
import hashlib
import json
import pathlib
import sys

manifest_path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
probe = sys.argv[3]
def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise SystemExit(f"duplicate OPEN loader manifest field: {key}")
        result[key] = value
    return result

manifest = json.loads(manifest_path.read_text(encoding="utf-8"),
                      object_pairs_hook=no_duplicates)
if type(manifest) is not dict or set(manifest) != {
        "schema", "created_utc", "target", "files", "mandatory_next_ota",
        "physical_hold"}:
    raise SystemExit("OPEN loader replacement manifest fields are not canonical")
if manifest.get("schema") != "roller-ecu-open-loader-replacement-v1":
    raise SystemExit("unsupported OPEN loader replacement manifest")
target = manifest.get("target") or {}
if target != {
        "probe_serial": probe,
        "device_id": "0x484",
        "product_state": "OPEN",
        "mcu_uid": "003800613434511232383537",
        "atecc608_serial": "0123d47eb2ee0e9bee"}:
    raise SystemExit("OPEN loader transaction target identity mismatch")
expected_names = {
    "full-flash.bin", "secure-persistent.bin", "option-bytes-before.txt",
    "programmer-mcu-uid.bin", "runtime-before.json", "ota-before.json",
    "secure-primary.bin", "nonsecure-primary.bin", "primary-audit.json",
    "ReleaseOpen-ECU_OEMiROT.bin", "roller-ecu-1.0.14.recu",
}
files = manifest.get("files")
if type(files) is not dict or set(files) != expected_names:
    raise SystemExit("OPEN loader transaction file set is not canonical")
for name, expected in files.items():
    if type(expected) is not dict or set(expected) != {"size", "sha256"} or \
       type(expected.get("size")) is not int or expected["size"] <= 0 or \
       type(expected.get("sha256")) is not str or \
       len(expected["sha256"]) != 64 or \
       any(character not in "0123456789abcdef" for character in expected["sha256"]):
        raise SystemExit(f"transaction file entry is malformed: {name}")
    path = root / name
    if path.is_symlink() or not path.is_file():
        raise SystemExit(f"transaction file is missing or a symbolic link: {name}")
    data = path.read_bytes()
    if len(data) != expected["size"]:
        raise SystemExit(f"transaction file size mismatch: {name}")
    if hashlib.sha256(data).hexdigest() != expected["sha256"]:
        raise SystemExit(f"transaction file hash mismatch: {name}")
if files["ReleaseOpen-ECU_OEMiROT.bin"] != {
        "size": 52276,
        "sha256": "5fdaaa3f5e2d2a6c01ed527282ba08b3d8d5e579c61ec3c4e097cdf2c0fd7761"}:
    raise SystemExit("transaction ReleaseOpen is not the reviewed exact artifact")
if files["roller-ecu-1.0.14.recu"] != {
        "size": 525371,
        "sha256": "a052f51a7ef14bbbec57f412950edad7f44f92a2323227d74c606d8dd866324d"}:
    raise SystemExit("transaction OTA package is not the reviewed exact 1.0.14 release")
next_ota = manifest.get("mandatory_next_ota") or {}
if next_ota != {
        "version": "1.0.14", "update_sequence": 14,
        "security_counter": 14, "package_name": "roller-ecu-1.0.14.recu",
        **files["roller-ecu-1.0.14.recu"]}:
    raise SystemExit("transaction lost its mandatory OTA 1.0.14 hold")
if manifest.get("physical_hold") != {
        "jp1": "OPEN/BOOT0 low",
        "stlink": "must remain connected through confirmed OTA 1.0.14",
        "cold_start_embargo":
            "do not cold-start before paired 1.0.14 image_ok confirmation"}:
    raise SystemExit("transaction physical hold policy is not canonical")
print("Fresh backup and pinned OPEN loader transaction manifest verified")
PY
}

continue_open_loader_replacement() {
  local task_readback="${task_open_loader_dir}/ReleaseOpen-readback.bin"
  local task_boot_size

  verify_open_loader_transaction_manifest
  check_open_identity_during_recovery
  verify_programmer_mcu_uid "${task_open_loader_dir}/programmer-mcu-uid-current.bin"
  task_boot_size="$(stat -c '%s' -- "${task_open_loader_staged_boot}")"
  if ! open_loader_phase_done boot_protection_options_verified; then
    "${task_cli}" "${task_connect_reset[@]}" \
      -ob WRPSGn1=0xFFFFFFFF WRPSGn2=0xFFFFFFFF \
          HDP1_STRT=0x1 HDP1_END=0x0 HDP2_STRT=0x1 HDP2_END=0x0
    record_open_loader_phase boot_wrp_hdp_temporarily_removed
    if ! open_loader_phase_done boot_program_readback_verified; then
      "${task_cli}" "${task_connect_reset[@]}" \
        -d "${task_open_loader_staged_boot}" 0x0C000000 -v
    fi
    # Always repeat an independent read before restoring protection. This also
    # covers a resume whose journal says the earlier readback completed.
    "${task_cli}" "${task_connect_reset[@]}" \
      -u 0x0C000000 "${task_boot_size}" "${task_readback}"
    cmp -- "${task_open_loader_staged_boot}" "${task_readback}"
    sha256sum "${task_readback}" > "${task_open_loader_dir}/ReleaseOpen-readback.sha256"
    if ! open_loader_phase_done boot_program_readback_verified; then
      record_open_loader_phase boot_program_readback_verified
    else
      record_open_loader_phase resumed_boot_readback_reverified
    fi
    "${task_cli}" "${task_connect_reset[@]}" \
      -ob WRPSGn1=0xFFFFFFF0 WRPSGn2=0xFFFFFFFF \
          HDP1_STRT=0x0 HDP1_END=0x17 HDP2_STRT=0x1 HDP2_END=0x0
    record_open_loader_phase boot_wrp_hdp_restore_command_complete
    verify_final_open_options > "${task_open_loader_dir}/option-bytes-after.txt"
    record_open_loader_phase boot_protection_options_verified
  fi

  if ! open_loader_phase_done replacement_runtime_safe_verified; then
    "${task_cli}" "${task_connect_reset[@]}" -rst
    check_runtime_authentication "${task_open_loader_dir}/runtime-after.json"
    check_ota_idle "${task_open_loader_dir}/ota-after.json"
    record_open_loader_phase replacement_runtime_safe_verified
  fi
  if ! open_loader_phase_done replacement_complete_stlink_cold_start_embargo_active; then
    # Recheck the pinned boot, backup evidence, and staged 1.0.14 package at
    # completion as well as at entry/resume.
    verify_open_loader_transaction_manifest
    record_open_loader_phase replacement_complete_stlink_cold_start_embargo_active
  fi
  printf '\nLatest ReleaseOpen OEMiROT replacement is complete and protected.\n'
  printf 'MANDATORY HOLD: keep ST-Link/NRST connected and do not cold-start the ECU.\n'
  printf '%s\n' \
    'Next install and confirm exactly:' \
    "  python3 tools/ethernet_ota.py ${task_open_loader_staged_package}"
  printf 'Only after accepted_sequence=14, paired confirmation is clear, and runtime-safe checks pass may ST-Link be removed for cold-start regression.\n'
}

replace_open_loader() {
  check_open_loader_replacement_inputs
  create_open_loader_transaction
  verify_final_open_options
  check_open_loader_nrst_and_normal_boot
  backup_and_validate_open_loader_target
  stage_open_loader_transaction_inputs
  continue_open_loader_replacement
}

resume_open_loader_replacement() {
  select_open_loader_transaction
  require_completed_phase_for_open_loader() {
    if ! open_loader_phase_done "$1"; then
      printf 'OPEN loader transaction has not completed required phase: %s\n' "$1" >&2
      return 1
    fi
  }
  require_completed_phase_for_open_loader nrst_and_jp1_normal_boot_verified
  require_completed_phase_for_open_loader fresh_full_flash_persistent_ob_runtime_backup_complete
  require_completed_phase_for_open_loader paired_primary_signature_identity_trailer_verified
  require_completed_phase_for_open_loader releaseopen_hash_size_and_ota_1_0_14_pinned
  continue_open_loader_replacement
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
if (manifest.get("obk_package_device_bound") is not False or
        manifest.get("debug_reopening") is not True or
        manifest.get("debug_scope") != "HDPL3 secure and nonsecure" or
        manifest.get("full_regression") is not True or
        manifest.get("partial_regression") is not False or
        manifest.get("permission_mask") != "0x00004040"):
    raise SystemExit("DA field-service policy is not the reviewed minimum")
print("OEMiROT images, OBKeys, and field-service DA policy verified")
PY
}

verify_reproducible_security_inputs() {
  local task_regenerated
  local task_name
  task_regenerated="$(mktemp -d)"
  if ! python3 "${task_script_dir}/generate_security_provisioning.py" \
      --pki-dir "${task_pki_dir}" --output-dir "${task_regenerated}"; then
    rm -rf -- "${task_regenerated}"
    return 1
  fi
  for task_name in DA_Config.obk OEMiRoT_Config.obk OEMiRoT_Data.obk; do
    if ! cmp -s -- "${task_regenerated}/${task_name}" \
        "${task_provisioning_dir}/${task_name}"; then
      rm -rf -- "${task_regenerated}"
      printf 'OBK is not a reproducible provisioning input: %s\n' \
        "${task_name}" >&2
      return 1
    fi
  done
  rm -rf -- "${task_regenerated}"
  printf 'OBKs reproduced byte-for-byte from PKI/TPC inputs (not a DHUK export)\n'
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
  # wait until AP1/SFSP is observable, then invoke SDP without another reset.
  # CubeProgrammer may return from -hardRst before the RSS has reopened AP1;
  # a readiness probe avoids treating that normal window as an SDP failure.
  "${task_cli}" "${task_connect_hotplug[@]}" -hardRst
  local task_ready=0
  local task_try
  local task_options
  for task_try in $(seq 1 10); do
    if task_options="$(read_option_bytes_rss_plain 2>/dev/null)" &&
        printf '%s\n' "${task_options}" |
          grep -Eq 'SFSP Version:[[:space:]]+v2[.]5[.]0' &&
        printf '%s\n' "${task_options}" |
          grep -Eq 'PRODUCT_STATE[[:space:]]*: 0xED[[:space:]]+[(]Open[)]'; then
      task_ready=1
      break
    fi
    sleep 0.2
  done
  if [[ "${task_ready}" -ne 1 ]]; then
    printf 'ROM RSS did not reopen AP1 after reset; %s was not submitted.\n' \
      "${task_name}" >&2
    return 1
  fi
  printf 'ROM RSS ready for %s after reset (attempt %s).\n' \
    "${task_name}" "${task_try}"
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
  require_file "${task_phase_file}"
  task_persistent_backup="$(sed -n 's/^persistent_backup=//p' \
    "${task_phase_file}" | tail -n 1)"
  if [[ -z "${task_persistent_backup}" ]]; then
    task_persistent_backup="${task_backup_prefix}-persistent.bin"
  fi
  require_file "${task_persistent_backup}"
  # Legacy journals captured the 120-KiB used portion; current recovery
  # journals capture the complete canonical 128-KiB persistent partition.
  require_persistent_backup_size "${task_persistent_backup}"
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

program_open_layout() {
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

provision_open() {
  check_inputs
  check_open_target
  create_backup
  program_open_layout
}

load_full_regression_recovery() {
  if [[ -z "${ECU_RECOVERY_BACKUP_DIR:-}" ]]; then
    printf 'Set ECU_RECOVERY_BACKUP_DIR to the reviewed v4 recovery package directory.\n' >&2
    exit 2
  fi
  task_recovery_dir="$(realpath -e -- "${ECU_RECOVERY_BACKUP_DIR}")"
  task_recovery_manifest="${task_recovery_dir}/manifest.json"
  task_persistent_backup="${task_recovery_dir}/secure-persistent.bin"
  require_file "${task_recovery_manifest}"
  if [[ -L "${task_recovery_manifest}" ]]; then
    printf 'Recovery manifest must not be a symbolic link: %s\n' \
      "${task_recovery_manifest}" >&2
    exit 2
  fi

  local task_recovery_schema
  task_recovery_schema="$(python3 - "${task_recovery_manifest}" <<'PY'
import json
import sys
try:
    value = json.load(open(sys.argv[1], encoding="utf-8"))
except (OSError, UnicodeError, json.JSONDecodeError) as error:
    raise SystemExit(f"cannot read recovery manifest: {error}")
print(value.get("schema", "") if type(value) is dict else "")
PY
)"
  if [[ "${task_recovery_schema}" == "roller-ecu-full-regression-backup-v1" ||
        "${task_recovery_schema}" == "roller-ecu-full-regression-backup-v2" ||
        "${task_recovery_schema}" == "roller-ecu-full-regression-backup-v3" ]]; then
    printf '%s\n' \
      'Legacy v1/v2/v3 recovery packages remain evidence but cannot drive the current' \
      'destructive 1.0.15 rebuild. Regenerate a v4 package from the exact immutable' \
      'CLOSED transaction directory with tools/create_full_regression_recovery.sh.' >&2
    exit 2
  fi

  local task_file
  for task_file in \
      "${task_closed_boot_image}" "${task_boot_image}" \
      "${task_secure_image}" "${task_nonsecure_image}" \
      "${task_recovery_ota_package}" "${task_persistent_backup}" \
      "${task_recovery_dir}/full-flash.bin" \
      "${task_recovery_dir}/before-closed-option-bytes.txt" \
      "${task_recovery_dir}/closed-transition-phase.txt" \
      "${task_recovery_dir}/closed-transaction-manifest.json" \
      "${task_recovery_dir}/programmer-mcu-uid.bin" \
      "${task_recovery_dir}/pairing-store.bin" \
      "${task_recovery_dir}/primary-audit.json" \
      "${task_runtime_preflight_verifier}" "${task_pairing_verifier}"; do
    if [[ ! -f "${task_file}" || -L "${task_file}" ]]; then
      printf 'Missing, non-regular, or symbolic-link recovery input: %s\n' \
        "${task_file}" >&2
      exit 2
    fi
  done
  require_size "${task_persistent_backup}" 131072
  require_size "${task_recovery_dir}/full-flash.bin" 2097152
  require_size "${task_recovery_dir}/pairing-store.bin" 16384

  python3 - "${task_recovery_manifest}" "${task_recovery_dir}" \
      "${task_probe}" "${task_version}" "${task_boot_image}" \
      "${task_closed_boot_image}" "${task_secure_image}" \
      "${task_nonsecure_image}" "${task_recovery_ota_package}" \
      "${task_script_dir}" <<'PY'
import datetime
import hashlib
import json
import pathlib
import re
import sys
import uuid

manifest_path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
probe = sys.argv[3]
version = sys.argv[4]
boot_image = pathlib.Path(sys.argv[5])
closed_boot_image = pathlib.Path(sys.argv[6])
secure_image = pathlib.Path(sys.argv[7])
nonsecure_image = pathlib.Path(sys.argv[8])
ota_package = pathlib.Path(sys.argv[9])
sys.path.insert(0, sys.argv[10])
from verify_closed_preflight import (
    CLOSED_PHASES,
    EXPECTED_ACCEPTED_SEQUENCE,
    EXPECTED_ATECC_CONFIG_CRC32C,
    EXPECTED_ATECC_SERIAL,
    EXPECTED_MCU_UID,
    EXPECTED_PAIRING_GENERATION,
    EXPECTED_RELEASE_CLOSED_SHA256,
    EXPECTED_RELEASE_CLOSED_SIZE,
    EXPECTED_RELEASE_IDENTITY,
    TRANSACTION_FILE_SIZES,
    validate_phase_journal,
    validate_installed_primary_audit,
    validate_programmer_uid,
)
from verify_open_loader_backup import (
    NONSECURE_OFFSET,
    NONSECURE_SIZE,
    SECURE_OFFSET,
    SECURE_SIZE,
    ReleaseIdentity,
    parse_primary,
    validate_installed_flash,
)
from verify_pairing_store import VerificationError, verify_dual_store

def reject(message):
    raise SystemExit(message)

def exact_int(value, expected, name):
    if type(value) is not int or value != expected:
        reject(f"{name} is not exactly {expected}")

def load_json(path):
    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                reject(f"duplicate JSON field in {path}: {key}")
            result[key] = value
        return result
    try:
        value = json.loads(path.read_text(encoding="utf-8"),
                           object_pairs_hook=no_duplicates)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        reject(f"cannot read recovery JSON {path}: {error}")
    if type(value) is not dict:
        reject(f"recovery JSON is not an object: {path}")
    return value

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

if version != "1.0.15":
    reject("Full Regression recovery requires ECU_INITIAL_VERSION exactly 1.0.15")

manifest = load_json(manifest_path)
if set(manifest) != {
        "schema", "created_utc", "purpose", "persistent_format", "target",
        "capture", "installed_firmware", "files", "recovery_inputs"}:
    reject("v4 recovery manifest fields are not canonical")
if manifest.get("schema") != "roller-ecu-full-regression-backup-v4":
    reject("unsupported recovery backup schema; regenerate the reviewed v4 package")
try:
    created = datetime.datetime.strptime(
        manifest.get("created_utc"), "%Y-%m-%dT%H:%M:%SZ")
except (TypeError, ValueError):
    reject("recovery creation timestamp is not canonical UTC")
if type(manifest.get("purpose")) is not str or not manifest["purpose"].strip():
    reject("recovery purpose is empty")
if manifest.get("persistent_format") != (
        "Application-owned Secure Flash records; not hardware DHUK-wrapped "
        "secure storage"):
    reject("recovery persistent format is not canonical")

target = manifest.get("target")
expected_target = {
    "probe_serial": probe,
    "device_id": "0x484",
    "mcu_uid": EXPECTED_MCU_UID,
    "atecc608_serial": EXPECTED_ATECC_SERIAL,
    "product_state": "0x72 CLOSED",
    "da_permission": "0x00004040",
}
if type(target) is not dict or target != expected_target:
    reject("recovery package target identity is not exact")

capture = manifest.get("capture")
if type(capture) is not dict or set(capture) != {
        "source_schema", "transaction_uuid", "transaction_manifest_sha256",
        "product_state", "relationship", "final_phase"}:
    reject("recovery capture provenance fields are not canonical")
if (capture.get("source_schema") != "roller-ecu-closed-transition-v2" or
        capture.get("product_state") != "0xED OPEN" or
        capture.get("relationship") !=
        "immutable evidence captured before this verified CLOSED transition" or
        capture.get("final_phase") != "product_state_closed_verified"):
    reject("recovery capture provenance is not the reviewed CLOSED transaction")
try:
    transaction_uuid = str(uuid.UUID(capture.get("transaction_uuid"), version=4))
except (AttributeError, TypeError, ValueError):
    reject("recovery transaction UUID is invalid")
if transaction_uuid != capture.get("transaction_uuid"):
    reject("recovery transaction UUID is not canonical UUIDv4")
if re.fullmatch(r"[0-9a-f]{64}",
                str(capture.get("transaction_manifest_sha256"))) is None:
    reject("source transaction manifest hash is invalid")

installed = manifest.get("installed_firmware")
if type(installed) is not dict or set(installed) != {
        "version", "build", "security_counter", "accepted_sequence",
        "layout_version", "ota_package_sha256"}:
    reject("installed firmware identity fields are not canonical")
if installed.get("version") != "1.0.15":
    reject("recovery installed version is not exactly 1.0.15")
for name, expected in (("build", 0), ("security_counter", 15),
                       ("accepted_sequence", EXPECTED_ACCEPTED_SEQUENCE),
                       ("layout_version", 65536)):
    exact_int(installed.get(name), expected, f"installed firmware {name}")
if installed.get("ota_package_sha256") != \
        "9980acbf248f242c77c7044f48d627ca862d6714221f5835c0a21b7a24bf9e54":
    reject("installed OTA package hash is not the reviewed release")

file_specs = {
    "full-flash.bin": (2097152, "0x08000000"),
    "secure-persistent.bin": (131072, "0x0C0E0000"),
    "before-closed-option-bytes.txt": (None, None),
    "closed-transition-phase.txt": (None, None),
    "closed-transaction-manifest.json": (None, None),
    "programmer-mcu-uid.bin": (12, None),
    "pairing-store.bin": (16384, None),
    "primary-audit.json": (None, None),
}
files = manifest.get("files")
if type(files) is not dict or set(files) != set(file_specs):
    reject("recovery package file set is incomplete or contains unknown files")
for name, (exact_size, address) in file_specs.items():
    entry = files.get(name)
    expected_fields = {"size", "sha256"} | ({"address"} if address else set())
    if type(entry) is not dict or set(entry) != expected_fields:
        reject(f"recovery file entry is malformed: {name}")
    size = entry.get("size")
    if type(size) is not int or size <= 0 or \
       (exact_size is not None and size != exact_size):
        reject(f"recovery file size is invalid: {name}")
    if address and entry.get("address") != address:
        reject(f"recovery restore address is invalid: {name}")
    if re.fullmatch(r"[0-9a-f]{64}", str(entry.get("sha256"))) is None:
        reject(f"recovery file hash is invalid: {name}")
    path = root / name
    if path.is_symlink() or not path.is_file():
        reject(f"recovery file is missing or a symbolic link: {name}")
    data = path.read_bytes()
    if len(data) != size or hashlib.sha256(data).hexdigest() != entry["sha256"]:
        reject(f"recovery file changed: {name}")

source_manifest_path = root / "closed-transaction-manifest.json"
if digest(source_manifest_path) != capture["transaction_manifest_sha256"]:
    reject("copied CLOSED transaction manifest does not match capture provenance")
source = load_json(source_manifest_path)
if set(source) != {"schema", "transaction_uuid", "created_utc", "target",
                  "release", "tool", "files"} or \
   source.get("schema") != "roller-ecu-closed-transition-v2" or \
   source.get("transaction_uuid") != transaction_uuid:
    reject("copied CLOSED transaction manifest identity is not canonical")
source_target = source.get("target")
expected_source_target = {
    "probe_serial": probe,
    "device_id": "0x484",
    "product_state": "OPEN",
    "mcu_uid": EXPECTED_MCU_UID,
    "atecc608_serial": EXPECTED_ATECC_SERIAL,
    "atecc_config_crc32c": EXPECTED_ATECC_CONFIG_CRC32C,
    "pairing_generation": EXPECTED_PAIRING_GENERATION,
}
if type(source_target) is not dict or set(source_target) != set(expected_source_target):
    reject("copied CLOSED transaction target identity differs")
for name, expected in expected_source_target.items():
    if name in ("atecc_config_crc32c", "pairing_generation"):
        exact_int(source_target.get(name), expected,
                  f"source transaction target {name}")
    elif source_target.get(name) != expected:
        reject(f"source transaction target {name} differs")
source_release = source.get("release")
if type(source_release) is not dict or set(source_release) != {
        "version", "build", "security_counter", "accepted_sequence",
        "oemirot_size", "oemirot_sha256"} or \
   source_release.get("version") != "1.0.15" or \
   source_release.get("oemirot_sha256") != EXPECTED_RELEASE_CLOSED_SHA256:
    reject("copied CLOSED transaction release identity differs")
for name, expected in (("build", 0), ("security_counter", 15),
                       ("accepted_sequence", EXPECTED_ACCEPTED_SEQUENCE),
                       ("oemirot_size", EXPECTED_RELEASE_CLOSED_SIZE)):
    exact_int(source_release.get(name), expected, f"source transaction release {name}")
source_files = source.get("files")
if type(source_files) is not dict or set(source_files) != set(TRANSACTION_FILE_SIZES):
    reject("copied CLOSED transaction file set is not canonical")
for name, (minimum, maximum) in TRANSACTION_FILE_SIZES.items():
    entry = source_files.get(name)
    if type(entry) is not dict or set(entry) != {"size", "sha256"}:
        reject(f"source transaction file entry is malformed: {name}")
    size = entry.get("size")
    if type(size) is not int or size < minimum or \
       (maximum is not None and size > maximum):
        reject(f"source transaction file size is invalid: {name}")
    if re.fullmatch(r"[0-9a-f]{64}", str(entry.get("sha256"))) is None:
        reject(f"source transaction file hash is invalid: {name}")
source_tool = source.get("tool")
if type(source_tool) is not dict or set(source_tool) != {
        "stm32_programmer_realpath", "stm32_programmer_sha256",
        "stm32_programmer_version"} or \
   type(source_tool.get("stm32_programmer_realpath")) is not str or \
   not pathlib.Path(source_tool["stm32_programmer_realpath"]).is_absolute() or \
   re.fullmatch(r"[0-9a-f]{64}",
                str(source_tool.get("stm32_programmer_sha256"))) is None or \
   source_tool.get("stm32_programmer_version") != "2.23.0":
    reject("copied CLOSED transaction Programmer identity is not canonical")
for package_name, source_name in {
        "full-flash.bin": "full-flash.bin",
        "secure-persistent.bin": "secure-persistent.bin",
        "before-closed-option-bytes.txt": "option-bytes-before.txt",
        "programmer-mcu-uid.bin": "programmer-mcu-uid.bin",
        "pairing-store.bin": "pairing-store.bin",
        "primary-audit.json": "primary-audit.json",
}.items():
    source_entry = source_files.get(source_name)
    package_entry = files[package_name]
    if type(source_entry) is not dict or set(source_entry) != {"size", "sha256"} or \
       source_entry.get("size") != package_entry["size"] or \
       source_entry.get("sha256") != package_entry["sha256"]:
        reject(f"recovery evidence differs from source transaction: {package_name}")
closed_entry = source_files.get("ReleaseClosed-ECU_OEMiROT.bin")
if closed_entry != {"size": EXPECTED_RELEASE_CLOSED_SIZE,
                    "sha256": EXPECTED_RELEASE_CLOSED_SHA256}:
    reject("source transaction does not bind the reviewed ReleaseClosed loader")

phases = validate_phase_journal(
    (root / "closed-transition-phase.txt").read_text(encoding="utf-8"))
if phases != CLOSED_PHASES:
    reject("source CLOSED transaction phase journal is not complete")
validate_programmer_uid((root / "programmer-mcu-uid.bin").read_bytes())
validate_installed_primary_audit(load_json(root / "primary-audit.json"))
full_flash = (root / "full-flash.bin").read_bytes()
persistent = (root / "secure-persistent.bin").read_bytes()
pairing_store = (root / "pairing-store.bin").read_bytes()
if pairing_store != full_flash[0xE0000:0xE0000 + 0x4000]:
    reject("recovery pairing-store differs from full Flash offset 0xE0000")
if pairing_store != persistent[:0x4000]:
    reject("recovery pairing-store differs from persistent prefix 0x4000")
try:
    verify_dual_store(pairing_store)
except VerificationError as error:
    reject(f"Pairing-store verification failed: {error}")
options = (root / "before-closed-option-bytes.txt").read_text(encoding="utf-8")
if (re.search(r"Device ID\s*: 0x484(?:\s|$)", options) is None or
        re.search(r"PRODUCT_STATE\s*: 0xED\s+\(Open\)", options) is None):
    reject("pre-CLOSED option bytes do not prove the reviewed OPEN target")

reviewed_identity = ReleaseIdentity(1, 0, 15, 0, 15)
flash_s, flash_ns, _, _ = validate_installed_flash(
    full_flash, secure_image.parent)
if flash_s.identity != reviewed_identity or flash_ns.identity != reviewed_identity:
    reject("captured Flash is not paired confirmed 1.0.15+0/counter15")

expected_inputs = {
    "release_open_oemirot": (boot_image, 52276,
        "5fdaaa3f5e2d2a6c01ed527282ba08b3d8d5e579c61ec3c4e097cdf2c0fd7761"),
    "release_closed_oemirot": (closed_boot_image, 52292,
        EXPECTED_RELEASE_CLOSED_SHA256),
    "secure_initial": (secure_image, 196608,
        "1a50b9106f798fb64c1a766d81d194b8829e07497941b4af7efe41bb38b98d75"),
    "nonsecure_initial": (nonsecure_image, 327680,
        "4f96d510f06cda3de1a88de8be5a7516b09f1ed539b05e0d6be2eeaaa571d8ed"),
    "ota_package": (ota_package, 525371,
        "9980acbf248f242c77c7044f48d627ca862d6714221f5835c0a21b7a24bf9e54"),
}
inputs = manifest.get("recovery_inputs")
if type(inputs) is not dict or set(inputs) != set(expected_inputs):
    reject("recovery input set is incomplete or contains unknown artifacts")
for name, (path, size, expected_digest) in expected_inputs.items():
    entry = inputs.get(name)
    if entry != {"size": size, "sha256": expected_digest}:
        reject(f"recovery manifest does not pin reviewed input: {name}")
    data = path.read_bytes()
    if len(data) != size or hashlib.sha256(data).hexdigest() != expected_digest:
        reject(f"current recovery input is stale or unreviewed: {path}")

secure_report = parse_primary(
    "secure-initial", secure_image.read_bytes(), SECURE_OFFSET, SECURE_SIZE)
nonsecure_report = parse_primary(
    "nonsecure-initial", nonsecure_image.read_bytes(),
    NONSECURE_OFFSET, NONSECURE_SIZE)
if secure_report.identity != reviewed_identity or \
   nonsecure_report.identity != reviewed_identity:
    reject("recovery initial pair identity is not 1.0.15+0/counter15")
print("v4 recovery target, CLOSED provenance, dual pairing, paired images, and pinned inputs verified")
PY
}

preflight_full_regression_recovery() {
  load_full_regression_recovery
  check_inputs
  verify_reproducible_security_inputs
  printf 'Recovery preflight complete; target Flash was not accessed.\n'
}

recover_open_after_full_regression() {
  load_full_regression_recovery
  check_inputs
  verify_reproducible_security_inputs

  check_regressed_open_target
  check_blank_flash_sample

  task_backup_prefix="${task_recovery_dir}/after-full-regression"
  task_phase_file="${task_backup_prefix}-phase.txt"
  if [[ -e "${task_phase_file}" ]]; then
    printf 'Recovery journal already exists; refusing to restart a destructive phase:\n  %s\n' \
      "${task_phase_file}" >&2
    exit 3
  fi
  umask 077
  printf 'persistent_backup=%s\n' "${task_persistent_backup}" > "${task_phase_file}"
  printf 'firmware_version=%s\n' "${task_version}" >> "${task_phase_file}"
  record_phase pre_regression_backup_verified
  record_phase regressed_open_blank_target_verified
  program_open_layout
  printf '\nContinue with this exact version and journal after closing JP1:\n'
  printf '  ECU_INITIAL_VERSION=%q ECU_PROVISION_BACKUP_PREFIX=%q %q provision-obkeys --accept-boot0-high\n' \
    "${task_version}" "${task_backup_prefix}" "$0"
}

case "${task_action}" in
  inspect)
    display_option_bytes
    ;;
  preflight-open-oemirot-replacement)
    check_open_loader_replacement_inputs
    ;;
  replace-open-oemirot)
    if [[ "${task_confirmation}" != "--accept-open-boot-replacement" ]]; then
      printf '%s\n' \
        'This OPEN-only transaction proves physical NRST and normal JP1-low boot,' \
        'captures and validates fresh full-Flash/persistent/runtime backups, then' \
        'temporarily removes boot WRP/HDP to replace only ReleaseOpen OEMiROT.' \
        'It requires ST-Link to remain connected until Ethernet OTA 1.0.14 is confirmed.' \
        'Re-run with:' \
        '  ./tools/provision_oemirot_open.sh replace-open-oemirot --accept-open-boot-replacement' >&2
      exit 4
    fi
    replace_open_loader
    ;;
  resume-open-oemirot-replacement)
    if [[ "${task_confirmation}" != "--accept-resume-open-boot-replacement" ]]; then
      printf '%s\n' \
        'Resume requires the exact fresh transaction directory and explicit acceptance:' \
        '  ECU_OPEN_LOADER_TRANSACTION_DIR=/absolute/...-open-loader-replacement \' \
        '  ./tools/provision_oemirot_open.sh resume-open-oemirot-replacement \' \
        '    --accept-resume-open-boot-replacement' >&2
      exit 4
    fi
    resume_open_loader_replacement
    ;;
  preflight)
    check_inputs
    check_open_target
    check_runtime_authentication /tmp/roller-ecu-oemirot-preflight.json
    ;;
  preflight-full-regression-recovery)
    preflight_full_regression_recovery
    ;;
  validate-full-regression-package)
    load_full_regression_recovery
    printf 'Recovery package validation complete; no target or private PKI was accessed.\n'
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
  recover-open-after-full-regression)
    if [[ "${task_confirmation}" != "--accept-blank-open-rebuild" ]]; then
      printf '%s\n' \
        'This action is only for the blank OPEN target produced by an authenticated' \
        'Full Regression. It validates the explicit backup, rebuilds all Flash, and' \
        'leaves OBKey provisioning for the JP1-gated next phase. Re-run with:' \
        '  ECU_INITIAL_VERSION=1.0.15 ECU_RECOVERY_BACKUP_DIR=/absolute/backup/dir \' \
        '  ./tools/provision_oemirot_open.sh recover-open-after-full-regression \' \
        '    --accept-blank-open-rebuild' >&2
      exit 4
    fi
    recover_open_after_full_regression
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
    printf 'Usage: %s {inspect|preflight|preflight-open-oemirot-replacement|replace-open-oemirot|resume-open-oemirot-replacement|validate-full-regression-package|preflight-full-regression-recovery|provision-open|recover-open-after-full-regression|provision-obkeys|repair-boot-layout|repair-app-vectors|complete-after-boot0-open}\n' "$0" >&2
    exit 2
    ;;
esac

trap - EXIT
