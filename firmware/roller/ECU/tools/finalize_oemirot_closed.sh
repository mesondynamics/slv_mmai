#!/usr/bin/env bash
set -euo pipefail
umask 077

# Irreversible STM32H563 OPEN -> PROVISIONING -> CLOSED transition. This uses
# ST's reviewed H563 OEMiROT ordering and never invokes Full Regression.

task_action="${1:-preflight}"
task_confirmation="${2:-}"
task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_cli="${STM32_PROGRAMMER_CLI:-/home/plac/.local/share/stm32cube/bundles/programmer/2.23.0/bin/STM32_Programmer_CLI}"
task_probe="${STLINK_SERIAL:-066BFF565456857187210935}"
task_assets="${task_project_dir}/artifacts/security-provisioning"
task_pki_dir="${ECU_PKI_DIR:-/home/plac/.local/share/roller-ecu-pki}"
task_backup_root="${ECU_BACKUP_DIR:-${task_project_dir}/artifacts/device-backups}"
task_boot_image="${task_project_dir}/Bootloader/OEMiROT/build/ReleaseClosed/ECU_OEMiROT.bin"
task_expected_boot_size=52292
task_expected_boot_sha256="4073a9faa15eed7ca6014f168ae91efc35ba1e26d8e74651552c9a0a3d8ef884"
task_imgtool="/home/plac/STM32Cube/Repository/STM32Cube_FW_H5_V1.7.0/Middlewares/Third_Party/mcuboot/scripts/imgtool.py"
task_preflight_verifier="${task_script_dir}/verify_closed_preflight.py"
task_backup_verifier="${task_script_dir}/verify_open_loader_backup.py"
task_pairing_verifier="${task_script_dir}/verify_pairing_store.py"
task_installed_release_dir="${task_project_dir}/artifacts/firmware/1.0.15"
task_connect_hotplug=( -c port=SWD "sn=${task_probe}" ap=1 mode=Hotplug )
task_connect_reset=( -c port=SWD "sn=${task_probe}" ap=1 mode=UR reset=HWrst )
task_connect_da=( -c port=SWD "sn=${task_probe}" speed=fast )
# CubeProgrammer 2.23.0 expands the H563 0x17 label after RSS starts to
# "Provisioning, Debug partially opened (only non-secure)".  Accept that
# reviewed spelling as well as the compact label, while keeping the numeric
# lifecycle value and the complete status line exact.
task_provisioning_state_pattern='PRODUCT_STATE[[:space:]]*: 0x17[[:space:]]+[(]Provisioning(,[[:space:]]+Debug partially opened[[:space:]]+[(]only non-secure[)])?[)][[:space:]]*$'
task_transaction_dir=""
task_phase_file=""
task_manifest=""
task_staged_boot=""
task_fresh_preclose_log=""

strip_terminal_sequences() {
  sed -E $'s/\x1B\[[0-9;]*[mK]//g'
}

record_phase() {
  python3 - "${task_phase_file}" "$1" "${task_script_dir}" <<'PY'
import os
import pathlib
import sys
import tempfile
sys.path.insert(0, sys.argv[3])
from verify_closed_preflight import CLOSED_PHASES, validate_phase_journal

path = pathlib.Path(sys.argv[1])
if path.is_symlink() or not path.is_file():
    raise SystemExit("CLOSED phase journal must be an existing regular file")
current = validate_phase_journal(path.read_text(encoding="utf-8"))
if len(current) >= len(CLOSED_PHASES) or CLOSED_PHASES[len(current)] != sys.argv[2]:
    raise SystemExit(f"out-of-order CLOSED phase append: {sys.argv[2]}")
next_text = "\n".join((*current, sys.argv[2])) + "\n"
temporary_fd, temporary_name = tempfile.mkstemp(
    prefix=".phase.txt.next-", dir=path.parent)
try:
    with os.fdopen(temporary_fd, "w", encoding="utf-8") as stream:
        stream.write(next_text)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary_name, path)
    directory_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)
except BaseException:
    try:
        os.unlink(temporary_name)
    except FileNotFoundError:
        pass
    raise
PY
  printf 'PHASE: %s\n' "$1"
}

phase_done() {
  grep -Fqx -- "$1" "${task_phase_file}"
}

set_transaction_paths() {
  task_phase_file="${task_transaction_dir}/phase.txt"
  task_manifest="${task_transaction_dir}/manifest.json"
  task_staged_boot="${task_transaction_dir}/ReleaseClosed-ECU_OEMiROT.bin"
}

failure_notice() {
  local task_status=$?
  if [[ ${task_status} -ne 0 && -n "${task_transaction_dir}" ]]; then
    printf '\nCLOSED transition stopped fail-closed. Keep this ECU, probe, and power physically continuous.\n' >&2
    printf 'Evidence and phase journal: %s\n' "${task_transaction_dir}" >&2
    printf 'Resume only this exact transaction with:\n' >&2
    printf '  ECU_CLOSED_TRANSACTION_DIR=%q %q resume-closed-transition --accept-same-physical-target\n' \
      "${task_transaction_dir}" "$0" >&2
  fi
  exit "${task_status}"
}
trap failure_notice EXIT

# Serialize even read-only preflight with the irreversible writer. A single
# host must never interleave lifecycle or OBKey commands from two processes.
exec 9>"/tmp/roller-ecu-closed-transition.lock"
if ! flock -n 9; then
  printf 'Another ECU CLOSED preflight/finalize/resume process is active.\n' >&2
  exit 5
fi

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
    printf 'Unexpected size for %s: %s (expected %s)\n' \
      "$1" "${task_actual}" "$2" >&2
    exit 2
  fi
}

require_option() {
  local task_text="$1"
  local task_pattern="$2"
  local task_description="$3"
  if ! grep -Eq -- "${task_pattern}" <<<"${task_text}"; then
    printf 'Option-byte check failed: %s\n' "${task_description}" >&2
    exit 3
  fi
}

read_open_options() {
  "${task_cli}" "${task_connect_reset[@]}" -ob displ -rst |
    strip_terminal_sequences
}

check_open_target() {
  local task_options
  task_options="$(read_open_options)"
  printf '%s\n' "${task_options}"
  require_option "${task_options}" 'Device ID[[:space:]]*: 0x484([[:space:]]|$)' \
    'target must be STM32H563 device ID 0x484'
  require_option "${task_options}" 'SFSP Version:[[:space:]]+v2[.]5[.]0' \
    'target must expose reviewed RSS SFSP version 2.5.0'
  require_option "${task_options}" 'PRODUCT_STATE[[:space:]]*: 0xED[[:space:]]+[(]Open[)]' \
    'target must still be OPEN'
  require_option "${task_options}" 'BOOT_UBE[[:space:]]*: 0xB4[[:space:]]' \
    'OEMiROT unique boot entry must remain selected'
  require_option "${task_options}" 'SWAP_BANK[[:space:]]*: 0x0[[:space:]]' \
    'bank swap must remain disabled'
  require_option "${task_options}" 'TZEN[[:space:]]*: 0xB4[[:space:]]' \
    'TrustZone must be enabled'
  require_option "${task_options}" 'SECBOOT_LOCK[[:space:]]*: 0xB4[[:space:]]' \
    'secure boot address must be locked'
  require_option "${task_options}" 'SECBOOTADD[[:space:]]*: 0xC0000[[:space:]]+[(]0x[Cc]000000[)]' \
    'secure boot address must be 0x0C000000'
  require_option "${task_options}" 'SECWM1_STRT[[:space:]]*: 0x0[[:space:]]' \
    'Bank 1 Secure watermark start must be sector 0'
  require_option "${task_options}" 'SECWM1_END[[:space:]]*: 0x7F[[:space:]]' \
    'all Bank 1 must remain Secure'
  require_option "${task_options}" 'SECWM2_STRT[[:space:]]*: 0x1[[:space:]]' \
    'Bank 2 Secure watermark must remain disabled'
  require_option "${task_options}" 'SECWM2_END[[:space:]]*: 0x0[[:space:]]' \
    'Bank 2 Secure watermark must remain disabled'
  require_option "${task_options}" 'WRPSGn1[[:space:]]*: 0xFFFFFFF0[[:space:]]' \
    'OEMiROT WRP groups must be active'
  require_option "${task_options}" 'WRPSGn2[[:space:]]*: 0xFFFFFFFF[[:space:]]' \
    'Bank 2 write-protection groups must remain disabled'
  require_option "${task_options}" 'HDP1_STRT[[:space:]]*: 0x0[[:space:]]' \
    'HDP start must protect OEMiROT'
  require_option "${task_options}" 'HDP1_END[[:space:]]*: 0x17[[:space:]]' \
    'HDP end must cover boot and scratch'
  require_option "${task_options}" 'HDP2_STRT[[:space:]]*: 0x1[[:space:]]' \
    'Bank 2 HDP must remain disabled'
  require_option "${task_options}" 'HDP2_END[[:space:]]*: 0x0[[:space:]]' \
    'Bank 2 HDP must remain disabled'
  require_option "${task_options}" 'SRAM2_RST[[:space:]]*: 0x0[[:space:]]' \
    'Secure SRAM2 must erase on reset'
  require_option "${task_options}" 'SRAM2_ECC[[:space:]]*: 0x0[[:space:]]' \
    'Secure SRAM2 ECC must remain enabled'
  require_option "${task_options}" 'IWDG_SW[[:space:]]*: 0x1[[:space:]]' \
    'ES0565 requires software IWDG selection for reliable Debug Authentication'
}

wait_ecu_network() {
  local task_try
  for task_try in $(seq 1 30); do
    if ping -I enp2s0 -c 1 -W 1 172.16.0.11 >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.2
  done
  printf 'ECU Ethernet did not recover.\n' >&2
  return 1
}

check_runtime_safe() {
  local task_state="$1"
  local task_try
  local task_fresh=0
  wait_ecu_network
  # Programmer's option-byte display resets the MCU.  ICMP can recover before
  # the UI backend has received fresh periodic status/diagnostic messages, so
  # wait for both protocol streams instead of treating that normal window as a
  # failed safety check.
  for task_try in $(seq 1 25); do
    if curl --max-time 3 -fsS \
      http://127.0.0.1:18088/api/state -o "${task_state}" && \
      python3 "${task_preflight_verifier}" runtime-state "${task_state}" \
        >/dev/null 2>&1
    then
      task_fresh=1
      break
    fi
    sleep 0.2
  done
  if [[ "${task_fresh}" -ne 1 ]]; then
    printf 'Debug UI did not receive a fresh exact ECU/ATECC/confirmation state.\n' >&2
    return 1
  fi
  python3 "${task_preflight_verifier}" runtime-state "${task_state}"
}

check_programmer_mcu_uid() {
  local task_output="$1"
  "${task_cli}" "${task_connect_reset[@]}" -u 0x08FFF800 0xC "${task_output}"
  python3 "${task_preflight_verifier}" programmer-uid "${task_output}"
}

check_ota_idle() {
  local task_output="$1"
  python3 "${task_script_dir}/ethernet_ota.py" --status > "${task_output}"
  python3 "${task_preflight_verifier}" ota-status "${task_output}"
}

read_secure_uptime() {
  curl --max-time 3 -fsS http://127.0.0.1:18088/api/state |
    python3 -c 'import json,sys; print(int((json.load(sys.stdin).get("diagnostic") or {}).get("secure_uptime_ms", -1)))'
}

check_nrst_path() {
  local task_before
  local task_after
  local task_try
  local task_state
  task_state="$(mktemp)"

  # check_open_target() resets the MCU immediately before this test.  Wait
  # until the old boot is unambiguously older than a normal Ethernet startup;
  # otherwise two valid reads can both land at (for example) 2011 ms and
  # produce a false failure even though the physical NRST pulse propagated.
  for task_try in $(seq 1 30); do
    task_before="$(read_secure_uptime)"
    if [[ "${task_before}" -ge 5000 ]]; then
      break
    fi
    sleep 0.2
  done
  if [[ "${task_before}" -lt 5000 ]]; then
    rm -f -- "${task_state}"
    printf 'Cannot establish a stable pre-reset Secure uptime (%s ms).\n' \
      "${task_before}" >&2
    return 1
  fi
  # HWRSTPULSE drives the probe's physical target-reset pin without relying on
  # a software reset. The fresh, smaller Secure uptime proves that NRST really
  # reaches the MCU; DA cannot be serviced without this signal.
  set +e
  "${task_cli}" -c port=SWD "sn=${task_probe}" mode=HWRSTPULSE
  local task_reset_status=$?
  set -e
  if [[ "${task_reset_status}" -ne 0 ]]; then
    rm -f -- "${task_state}"
    printf 'ST-Link HWRSTPULSE failed; verify the NRST wire.\n' >&2
    return 1
  fi
  wait_ecu_network
  if ! check_runtime_safe "${task_state}"; then
    rm -f -- "${task_state}"
    return 1
  fi
  task_after="$(read_secure_uptime)"
  rm -f -- "${task_state}"
  if [[ "${task_after}" -lt 0 || "${task_after}" -ge "${task_before}" ]]; then
    printf 'NRST did not reset Secure uptime (%s -> %s).\n' \
      "${task_before}" "${task_after}" >&2
    return 1
  fi
  printf 'Physical ST-Link NRST propagation verified (%s -> %s ms).\n' \
    "${task_before}" "${task_after}"
}

check_inputs() {
  local task_boot_hash
  local task_version_output
  local task_input
  for task_input in \
    "${task_cli}" "${task_boot_image}" "${task_imgtool}" \
    "${task_preflight_verifier}" "${task_backup_verifier}" \
    "${task_pairing_verifier}" \
    "${task_assets}/DA_Config.obk" \
    "${task_assets}/OEMiRoT_Config.obk" \
    "${task_assets}/OEMiRoT_Data.obk" \
    "${task_assets}/security-assets.json" \
    "${task_assets}/cert-root.b64" \
    "${task_assets}/cert-intermediate.b64" \
    "${task_assets}/cert-leaf.b64" \
    "${task_assets}/cert-leaf-chain.b64" \
    "${task_pki_dir}/oemirot-auth-s-public.pem" \
    "${task_pki_dir}/oemirot-auth-ns-public.pem" \
    "${task_installed_release_dir}/metadata.json" \
    "${task_installed_release_dir}/secure-initial.bin" \
    "${task_installed_release_dir}/nonsecure-initial.bin" \
    "${task_installed_release_dir}/roller-ecu-1.0.15.recu"; do
    require_file "${task_input}"
  done
  require_size "${task_assets}/DA_Config.obk" 108
  require_size "${task_assets}/OEMiRoT_Config.obk" 300
  require_size "${task_assets}/OEMiRoT_Data.obk" 204
  require_size "${task_boot_image}" "${task_expected_boot_size}"
  task_boot_hash="$(sha256sum "${task_boot_image}" | awk '{print $1}')"
  if [[ "${task_boot_hash}" != "${task_expected_boot_sha256}" ]]; then
    printf 'ReleaseClosed OEMiROT is not the reviewed artifact: %s\n' \
      "${task_boot_hash}" >&2
    exit 2
  fi
  task_version_output="$("${task_cli}" --version 2>&1 | strip_terminal_sequences)"
  if ! grep -Eq 'STM32CubeProgrammer version:[[:space:]]*2[.]23[.]0' \
      <<<"${task_version_output}"; then
    printf 'CLOSED transition requires STM32CubeProgrammer exactly 2.23.0.\n' >&2
    return 1
  fi
  python3 "${task_script_dir}/ecu_debug_auth.py" verify-assets \
    --programmer "${task_cli}" --probe "${task_probe}" \
    --pki-dir "${task_pki_dir}" --assets-dir "${task_assets}"
  python3 - "${task_assets}/DA_Config.obk" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
if len(data) != 108 or data[76:92] != bytes.fromhex("40400000000000000000000000000000"):
    raise SystemExit("DA_Config.obk permission payload is not 0x00004040")
print("DA_Config raw permission payload verified: 0x00004040")
PY
  sha256sum "${task_boot_image}" "${task_assets}/"*.obk \
    "${task_assets}/cert-leaf-chain.b64"
  verify_reproducible_security_inputs
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
  for task_name in \
    DA_Config.obk OEMiRoT_Config.obk OEMiRoT_Data.obk \
    cert-root.b64 cert-intermediate.b64 cert-leaf.b64 \
    cert-leaf-chain.b64 security-assets.json; do
    if ! cmp -s -- "${task_regenerated}/${task_name}" \
        "${task_assets}/${task_name}"; then
      rm -rf -- "${task_regenerated}"
      printf 'Security asset is not reproducible from the reviewed PKI: %s\n' \
        "${task_name}" >&2
      return 1
    fi
  done
  rm -rf -- "${task_regenerated}"
  printf 'OBKeys and complete DA certificate chain reproduced byte-for-byte from reviewed PKI\n'
}

verify_staged_against_reviewed_inputs() {
  local task_name

  # A transaction manifest detects accidental changes, but it is not itself a
  # trust anchor.  On every fresh/resumed entry, independently regenerate the
  # reviewed provisioning set from the offline PKI and require the immutable
  # transaction copies to match it byte-for-byte before any SDP command.
  verify_reproducible_security_inputs
  for task_name in \
    DA_Config.obk OEMiRoT_Config.obk OEMiRoT_Data.obk \
    security-assets.json cert-root.b64 cert-intermediate.b64 \
    cert-leaf.b64 cert-leaf-chain.b64; do
    cmp -- "${task_assets}/${task_name}" \
      "${task_transaction_dir}/${task_name}"
  done
  cmp -- "${task_pki_dir}/oemirot-auth-s-public.pem" \
    "${task_transaction_dir}/oemirot-auth-s-public.pem"
  cmp -- "${task_pki_dir}/oemirot-auth-ns-public.pem" \
    "${task_transaction_dir}/oemirot-auth-ns-public.pem"
  printf 'Staged CLOSED mutation inputs match the independently reproduced reviewed PKI set\n'
}

create_closed_transaction() {
  local task_timestamp
  task_timestamp="$(date -u +'%Y%m%dT%H%M%SZ')"
  mkdir -p -- "${task_backup_root}"
  task_transaction_dir="${task_backup_root}/stm32h563-${task_probe}-${task_timestamp}-closed-transition"
  if [[ -e "${task_transaction_dir}" ]]; then
    printf 'CLOSED transaction directory already exists: %s\n' \
      "${task_transaction_dir}" >&2
    return 1
  fi
  mkdir -- "${task_transaction_dir}"
  set_transaction_paths
  printf 'transaction_created\n' > "${task_phase_file}"
  sync -f "${task_phase_file}"
  sync -f "${task_transaction_dir}"
  sync -f "${task_backup_root}"
  printf 'PHASE: transaction_created\n'
}

extract_pairing_store_evidence() {
  python3 - "${task_transaction_dir}/full-flash.bin" \
      "${task_transaction_dir}/pairing-store.bin" <<'PY'
import pathlib
import sys

flash_path = pathlib.Path(sys.argv[1])
pairing_path = pathlib.Path(sys.argv[2])
flash = flash_path.read_bytes()
if len(flash) != 0x200000:
    raise SystemExit("full Flash evidence is not exactly 2 MiB")
pairing = flash[0xE0000:0xE0000 + 0x4000]
if len(pairing) != 0x4000:
    raise SystemExit("pairing-store extraction is not exactly 0x4000 bytes")
pairing_path.write_bytes(pairing)
PY
  require_size "${task_transaction_dir}/pairing-store.bin" 16384
}

verify_pairing_store_evidence() {
  require_size "${task_transaction_dir}/pairing-store.bin" 16384
  python3 - "${task_transaction_dir}/full-flash.bin" \
      "${task_transaction_dir}/secure-persistent.bin" \
      "${task_transaction_dir}/pairing-store.bin" <<'PY'
import pathlib
import sys

flash, persistent, pairing = (
    pathlib.Path(value).read_bytes() for value in sys.argv[1:])
if len(flash) != 0x200000:
    raise SystemExit("full Flash evidence is not exactly 2 MiB")
if len(persistent) != 0x20000:
    raise SystemExit("Secure persistent evidence is not exactly 0x20000 bytes")
if len(pairing) != 0x4000:
    raise SystemExit("pairing-store evidence is not exactly 0x4000 bytes")
if flash[0xE0000:0xE0000 + 0x4000] != pairing:
    raise SystemExit(
        "pairing-store evidence differs from physical full-Flash offset 0xE0000")
if persistent[:0x4000] != pairing:
    raise SystemExit(
        "pairing-store evidence differs from the independent persistent readback")
PY
  python3 "${task_pairing_verifier}" dual \
    "${task_transaction_dir}/pairing-store.bin"
}

capture_closed_transaction_evidence() {
  local task_name
  cp -- "${task_boot_image}" "${task_staged_boot}"
  for task_name in \
    DA_Config.obk OEMiRoT_Config.obk OEMiRoT_Data.obk \
    security-assets.json cert-root.b64 cert-intermediate.b64 \
    cert-leaf.b64 cert-leaf-chain.b64; do
    cp -- "${task_assets}/${task_name}" "${task_transaction_dir}/${task_name}"
  done
  cp -- "${task_pki_dir}/oemirot-auth-s-public.pem" \
    "${task_transaction_dir}/oemirot-auth-s-public.pem"
  cp -- "${task_pki_dir}/oemirot-auth-ns-public.pem" \
    "${task_transaction_dir}/oemirot-auth-ns-public.pem"

  read_open_options > "${task_transaction_dir}/option-bytes-before.txt"
  "${task_cli}" "${task_connect_reset[@]}" \
    -u 0x08000000 0x200000 "${task_transaction_dir}/full-flash.bin"
  "${task_cli}" "${task_connect_reset[@]}" \
    -u 0x080E0000 0x20000 "${task_transaction_dir}/secure-persistent.bin"
  require_size "${task_transaction_dir}/full-flash.bin" 2097152
  require_size "${task_transaction_dir}/secure-persistent.bin" 131072
  extract_pairing_store_evidence
  verify_pairing_store_evidence
  check_programmer_mcu_uid "${task_transaction_dir}/programmer-mcu-uid.bin"

  "${task_cli}" "${task_connect_reset[@]}" -rst
  check_runtime_safe "${task_transaction_dir}/runtime-before.json"
  check_ota_idle "${task_transaction_dir}/ota-before.json"

  python3 "${task_backup_verifier}" \
    --flash "${task_transaction_dir}/full-flash.bin" \
    --output-dir "${task_transaction_dir}" \
    --installed-exact-1.0.15 "${task_installed_release_dir}"
  python3 "${task_preflight_verifier}" installed-primary-audit \
    "${task_transaction_dir}/primary-audit.json"
  PYTHONPATH="${task_project_dir}/tools/imgtool_compat" \
    python3 "${task_imgtool}" verify \
      -k "${task_transaction_dir}/oemirot-auth-s-public.pem" \
      "${task_transaction_dir}/secure-primary.bin"
  PYTHONPATH="${task_project_dir}/tools/imgtool_compat" \
    python3 "${task_imgtool}" verify \
      -k "${task_transaction_dir}/oemirot-auth-ns-public.pem" \
      "${task_transaction_dir}/nonsecure-primary.bin"
  create_closed_manifest
  sync -f "${task_transaction_dir}"
  verify_closed_transaction
  record_phase pairing_store_dual_verified
  record_phase pre_mutation_evidence_verified
}

create_closed_manifest() {
  local task_cli_realpath
  local task_cli_sha256
  task_cli_realpath="$(realpath -e -- "${task_cli}")"
  task_cli_sha256="$(sha256sum "${task_cli_realpath}" | awk '{print $1}')"
  python3 - "${task_transaction_dir}" "${task_probe}" \
      "${task_cli_realpath}" "${task_cli_sha256}" "${task_script_dir}" <<'PY'
import datetime
import hashlib
import json
import os
import pathlib
import sys
import uuid

root = pathlib.Path(sys.argv[1])
probe = sys.argv[2]
sys.path.insert(0, sys.argv[5])
from verify_closed_preflight import TRANSACTION_FILE_SIZES

files = {}
for name in TRANSACTION_FILE_SIZES:
    data = (root / name).read_bytes()
    files[name] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
manifest = {
    "schema": "roller-ecu-closed-transition-v2",
    "transaction_uuid": str(uuid.uuid4()),
    "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "target": {
        "probe_serial": probe,
        "device_id": "0x484",
        "product_state": "OPEN",
        "mcu_uid": "003800613434511232383537",
        "atecc608_serial": "0123d47eb2ee0e9bee",
        "atecc_config_crc32c": 0xEBB326F3,
        "pairing_generation": 1,
    },
    "release": {
        "version": "1.0.15", "build": 0,
        "security_counter": 15, "accepted_sequence": 15,
        "oemirot_size": 52292,
        "oemirot_sha256": "4073a9faa15eed7ca6014f168ae91efc35ba1e26d8e74651552c9a0a3d8ef884",
    },
    "tool": {
        "stm32_programmer_realpath": sys.argv[3],
        "stm32_programmer_sha256": sys.argv[4],
        "stm32_programmer_version": "2.23.0",
    },
    "files": files,
}
temporary = root / ".manifest.json.tmp"
temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
os.replace(temporary, root / "manifest.json")
PY
  sync -f "${task_manifest}"
  sync -f "${task_transaction_dir}"
}

select_closed_transaction() {
  if [[ -z "${ECU_CLOSED_TRANSACTION_DIR:-}" ]]; then
    printf 'Set ECU_CLOSED_TRANSACTION_DIR to the exact CLOSED transaction directory.\n' >&2
    return 1
  fi
  task_transaction_dir="$(realpath -e -- "${ECU_CLOSED_TRANSACTION_DIR}")"
  set_transaction_paths
  require_file "${task_phase_file}"
  require_file "${task_manifest}"
}

verify_staged_security_policy() {
  python3 - "${task_transaction_dir}" <<'PY'
import hashlib
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
manifest = json.loads((root / "security-assets.json").read_text(encoding="utf-8"))
if (manifest.get("obk_package_device_bound") is not False or
        manifest.get("debug_reopening") is not True or
        manifest.get("debug_scope") != "HDPL3 secure and nonsecure" or
        manifest.get("full_regression") is not True or
        manifest.get("partial_regression") is not False or
        manifest.get("permission_mask") != "0x00004040"):
    raise SystemExit("staged DA policy is not the reviewed 0x00004040 policy")
expected_names = {
    "DA_Config.obk", "OEMiRoT_Config.obk", "OEMiRoT_Data.obk",
    "cert-root.b64", "cert-intermediate.b64", "cert-leaf.b64",
    "cert-leaf-chain.b64",
}
if set(manifest.get("files") or {}) != expected_names:
    raise SystemExit("staged security-assets file set is not exact")
for name, expected in manifest["files"].items():
    if hashlib.sha256((root / name).read_bytes()).hexdigest() != expected:
        raise SystemExit(f"staged security asset hash mismatch: {name}")
data = (root / "DA_Config.obk").read_bytes()
if len(data) != 108 or data[76:92] != bytes.fromhex("40400000000000000000000000000000"):
    raise SystemExit("staged DA_Config raw permission is not 0x00004040")
print("Staged OBKeys and complete DA chain policy verified")
PY
}

verify_closed_transaction() {
  local task_temp
  local task_cli_realpath
  local task_cli_sha256
  python3 "${task_preflight_verifier}" transaction-manifest \
    "${task_transaction_dir}" --probe "${task_probe}"
  python3 "${task_preflight_verifier}" phase-journal "${task_phase_file}"
  task_cli_realpath="$(realpath -e -- "${task_cli}")"
  task_cli_sha256="$(sha256sum "${task_cli_realpath}" | awk '{print $1}')"
  python3 - "${task_manifest}" "${task_cli_realpath}" "${task_cli_sha256}" <<'PY'
import json
import sys
manifest = json.load(open(sys.argv[1], encoding="utf-8"))
tool = manifest.get("tool") or {}
if tool != {"stm32_programmer_realpath": sys.argv[2],
           "stm32_programmer_sha256": sys.argv[3],
           "stm32_programmer_version": "2.23.0"}:
    raise SystemExit("STM32CubeProgrammer binary/version changed during CLOSED transaction")
PY
  check_programmer_version
  verify_staged_against_reviewed_inputs
  verify_staged_security_policy
  python3 "${task_preflight_verifier}" programmer-uid \
    "${task_transaction_dir}/programmer-mcu-uid.bin"
  python3 "${task_preflight_verifier}" runtime-state \
    "${task_transaction_dir}/runtime-before.json"
  python3 "${task_preflight_verifier}" ota-status \
    "${task_transaction_dir}/ota-before.json"
  python3 "${task_preflight_verifier}" installed-primary-audit \
    "${task_transaction_dir}/primary-audit.json"
  verify_pairing_store_evidence
  task_temp="$(mktemp -d)"
  python3 "${task_backup_verifier}" \
    --flash "${task_transaction_dir}/full-flash.bin" --output-dir "${task_temp}" \
    --installed-exact-1.0.15 "${task_installed_release_dir}"
  cmp -- "${task_temp}/secure-primary.bin" "${task_transaction_dir}/secure-primary.bin"
  cmp -- "${task_temp}/nonsecure-primary.bin" "${task_transaction_dir}/nonsecure-primary.bin"
  cmp -- "${task_temp}/primary-audit.json" "${task_transaction_dir}/primary-audit.json"
  rm -rf -- "${task_temp}"
  PYTHONPATH="${task_project_dir}/tools/imgtool_compat" \
    python3 "${task_imgtool}" verify \
      -k "${task_transaction_dir}/oemirot-auth-s-public.pem" \
      "${task_transaction_dir}/secure-primary.bin"
  PYTHONPATH="${task_project_dir}/tools/imgtool_compat" \
    python3 "${task_imgtool}" verify \
      -k "${task_transaction_dir}/oemirot-auth-ns-public.pem" \
      "${task_transaction_dir}/nonsecure-primary.bin"
}

check_programmer_version() {
  local task_version_output
  task_version_output="$("${task_cli}" --version 2>&1 | strip_terminal_sequences)"
  grep -Eq 'STM32CubeProgrammer version:[[:space:]]*2[.]23[.]0' \
    <<<"${task_version_output}"
}

check_open_resume_identity() {
  local task_options
  local task_uid
  task_options="$(read_open_options)"
  printf '%s\n' "${task_options}"
  require_option "${task_options}" 'Device ID[[:space:]]*: 0x484([[:space:]]|$)' \
    'resume target must be STM32H563 device ID 0x484'
  require_option "${task_options}" 'SFSP Version:[[:space:]]+v2[.]5[.]0' \
    'resume target must expose reviewed RSS SFSP version 2.5.0'
  require_option "${task_options}" 'PRODUCT_STATE[[:space:]]*: 0xED[[:space:]]+[(]Open[)]' \
    'OPEN boot recovery cannot run after PROVISIONING'
  require_option "${task_options}" 'BOOT_UBE[[:space:]]*: 0xB4[[:space:]]' \
    'OEMiROT unique boot entry changed'
  require_option "${task_options}" 'TZEN[[:space:]]*: 0xB4[[:space:]]' \
    'TrustZone changed during CLOSED transaction'
  require_option "${task_options}" 'SECBOOT_LOCK[[:space:]]*: 0xB4[[:space:]]' \
    'secure boot lock changed during CLOSED transaction'
  require_option "${task_options}" 'SECBOOTADD[[:space:]]*: 0xC0000[[:space:]]+[(]0x[Cc]000000[)]' \
    'secure boot address changed during CLOSED transaction'
  require_option "${task_options}" 'IWDG_SW[[:space:]]*: 0x1[[:space:]]' \
    'IWDG option changed during CLOSED transaction'
  # CubeProgrammer selects the upload encoder from the filename extension and
  # rejects an otherwise valid temporary path without a supported suffix.
  task_uid="$(mktemp --suffix=.bin)"
  if ! check_programmer_mcu_uid "${task_uid}"; then
    rm -f -- "${task_uid}"
    return 1
  fi
  rm -f -- "${task_uid}"
}

boot_options_are_unprotected() {
  grep -Eq 'WRPSGn1[[:space:]]*: 0xFFFFFFFF[[:space:]]' <<<"$1" &&
    grep -Eq 'WRPSGn2[[:space:]]*: 0xFFFFFFFF[[:space:]]' <<<"$1" &&
    grep -Eq 'HDP1_STRT[[:space:]]*: 0x1[[:space:]]' <<<"$1" &&
    grep -Eq 'HDP1_END[[:space:]]*: 0x0[[:space:]]' <<<"$1" &&
    grep -Eq 'HDP2_STRT[[:space:]]*: 0x1[[:space:]]' <<<"$1" &&
    grep -Eq 'HDP2_END[[:space:]]*: 0x0[[:space:]]' <<<"$1"
}

continue_open_boot_install() {
  local task_options
  local task_boot_size
  local task_readback="${task_transaction_dir}/ReleaseClosed-readback.bin"
  check_open_resume_identity

  if ! phase_done boot_unprotected; then
    if ! phase_done boot_unprotect_started; then
      record_phase boot_unprotect_started
    fi
    task_options="$(read_open_options)"
    if ! boot_options_are_unprotected "${task_options}"; then
      "${task_cli}" "${task_connect_reset[@]}" \
        -ob WRPSGn1=0xFFFFFFFF WRPSGn2=0xFFFFFFFF \
            HDP1_STRT=0x1 HDP1_END=0x0 HDP2_STRT=0x1 HDP2_END=0x0
      task_options="$(read_open_options)"
    fi
    if ! boot_options_are_unprotected "${task_options}"; then
      printf 'Boot WRP/HDP removal could not be verified.\n' >&2
      return 1
    fi
    record_phase boot_unprotected
  fi

  task_boot_size="$(stat -c '%s' -- "${task_staged_boot}")"
  if ! phase_done boot_program_readback_verified; then
    if ! phase_done boot_program_started; then
      record_phase boot_program_started
    fi
    task_options="$(read_open_options)"
    if ! boot_options_are_unprotected "${task_options}"; then
      "${task_cli}" "${task_connect_reset[@]}" \
        -ob WRPSGn1=0xFFFFFFFF WRPSGn2=0xFFFFFFFF \
            HDP1_STRT=0x1 HDP1_END=0x0 HDP2_STRT=0x1 HDP2_END=0x0
    fi
    set +e
    "${task_cli}" "${task_connect_reset[@]}" \
      -u 0x0C000000 "${task_boot_size}" "${task_readback}"
    local task_read_status=$?
    set -e
    if [[ "${task_read_status}" -ne 0 ]] || \
       ! cmp -s -- "${task_staged_boot}" "${task_readback}"; then
      "${task_cli}" "${task_connect_reset[@]}" \
        -d "${task_staged_boot}" 0x0C000000 -v
    fi
    "${task_cli}" "${task_connect_reset[@]}" \
      -u 0x0C000000 "${task_boot_size}" "${task_readback}"
    cmp -- "${task_staged_boot}" "${task_readback}"
    sync -f "${task_readback}"
    record_phase boot_program_readback_verified
  fi

  if ! phase_done boot_protection_verified; then
    if ! phase_done boot_protection_restore_started; then
      record_phase boot_protection_restore_started
    fi
    "${task_cli}" "${task_connect_reset[@]}" \
      -ob WRPSGn1=0xFFFFFFF0 WRPSGn2=0xFFFFFFFF \
          HDP1_STRT=0x0 HDP1_END=0x17 HDP2_STRT=0x1 HDP2_END=0x0
    check_open_target > "${task_transaction_dir}/option-bytes-boot-protected.txt"
    sync -f "${task_transaction_dir}/option-bytes-boot-protected.txt"
    record_phase boot_protection_verified
  fi

  # A journal proves which command completed, never the current hardware OB
  # state. Re-read and validate every lifecycle-critical invariant on every
  # fresh or resumed entry immediately before halting/PROVISIONING.
  check_open_target > "${task_transaction_dir}/option-bytes-pre-provisioning.txt"
  sync -f "${task_transaction_dir}/option-bytes-pre-provisioning.txt"

  if ! phase_done release_closed_halt_started; then
    record_phase release_closed_halt_started
  fi
  # Halt is volatile. The preceding full OB check deliberately resets the
  # target, and power loss also clears halt, so a durable phase can never
  # substitute for rebuilding the actual halt immediately before 0x17.
  "${task_cli}" "${task_connect_reset[@]}" -halt -score
  if ! phase_done release_closed_halted; then
    record_phase release_closed_halted
  fi
}

provisioning_rss_options_are_exact() {
  local task_options="$1"
  grep -Eq 'Device ID[[:space:]]*: 0x484([[:space:]]|$)' \
      <<<"${task_options}" &&
    grep -Eq 'SFSP Version:[[:space:]]+v2[.]5[.]0' \
      <<<"${task_options}" &&
    grep -Eq "${task_provisioning_state_pattern}" <<<"${task_options}"
}

da_discovery_common_is_exact() {
  local task_output="$1"
  grep -Eq 'discovery:[[:space:]]+target ID[.]*:[[:space:]]*0x484[[:space:]]*$' \
      <<<"${task_output}" &&
    grep -Eq 'discovery:[[:space:]]+SDA version[.]*:[[:space:]]*2[.]4[.]0[[:space:]]*$' \
      <<<"${task_output}" &&
    grep -Eq 'discovery:[[:space:]]+Vendor ID[.]*:[[:space:]]*STMicroelectronics[[:space:]]*$' \
      <<<"${task_output}" &&
    grep -Eq 'discovery:[[:space:]]+cryptosystems[.]*:[[:space:]]*Ecdsa-P256 SHA256[[:space:]]*$' \
      <<<"${task_output}" &&
    grep -Eq 'discovery:[[:space:]]+ST provisioning integrity status:[[:space:]]*0xEAEAEAEA[[:space:]]*$' \
      <<<"${task_output}" &&
    grep -Eq 'discovery:[[:space:]]+ST provisioning integrity status message:[[:space:]]*VALID[[:space:]]*$' \
      <<<"${task_output}"
}

provisioning_da_discovery_is_exact() {
  local task_output="$1"
  da_discovery_common_is_exact "${task_output}" &&
    grep -Eq 'discovery:[[:space:]]+PSA lifecycle[.]*:[[:space:]]*ST_LIFECYCLE_PROVISIONING[[:space:]]*$' \
      <<<"${task_output}"
}

closed_da_discovery_is_exact() {
  local task_output="$1"
  da_discovery_common_is_exact "${task_output}" &&
    grep -Eq 'discovery:[[:space:]]+PSA lifecycle[.]*:[[:space:]]*ST_LIFECYCLE_CLOSED[[:space:]]*$' \
      <<<"${task_output}"
}

detect_lifecycle() {
  local task_output
  local task_status
  set +e
  task_output="$("${task_cli}" "${task_connect_hotplug[@]}" -ob displ 2>&1 | \
    strip_terminal_sequences)"
  task_status=$?
  set -e
  if [[ "${task_status}" -eq 0 ]] &&
     grep -Eq 'Device ID[[:space:]]*: 0x484([[:space:]]|$)' <<<"${task_output}" &&
     grep -Eq 'PRODUCT_STATE[[:space:]]*: 0xED[[:space:]]+[(]Open[)]' <<<"${task_output}"; then
    printf 'OPEN\n'
    return 0
  fi
  if [[ "${task_status}" -eq 0 ]] &&
     provisioning_rss_options_are_exact "${task_output}"; then
    printf 'PROVISIONING\n'
    return 0
  fi
  set +e
  task_output="$("${task_cli}" "${task_connect_da[@]}" debugauth=2 2>&1 | \
    strip_terminal_sequences)"
  task_status=$?
  set -e
  if [[ "${task_status}" -eq 0 ]] &&
     provisioning_da_discovery_is_exact "${task_output}"; then
    printf 'PROVISIONING\n'
    return 0
  fi
  if [[ "${task_status}" -eq 0 ]] &&
     closed_da_discovery_is_exact "${task_output}"; then
    printf 'CLOSED\n'
    return 0
  fi
  printf 'UNKNOWN\n'
}

wait_provisioning_rss_ready() {
  local task_context="$1"
  local task_options
  local task_try
  for task_try in $(seq 1 10); do
    if task_options="$("${task_cli}" "${task_connect_hotplug[@]}" \
        -ob displ 2>/dev/null | strip_terminal_sequences)" &&
        provisioning_rss_options_are_exact "${task_options}"; then
      printf '%s\n' "${task_options}" > \
        "${task_transaction_dir}/provisioning-rss-last.txt"
      sync -f "${task_transaction_dir}/provisioning-rss-last.txt"
      printf 'Provisioning RSS 0x17 ready for %s after reset (attempt %s).\n' \
        "${task_context}" "${task_try}"
      return 0
    fi
    sleep 0.2
  done
  printf 'Provisioning RSS did not prove Device 0x484/SFSP 2.5/state 0x17 for %s.\n' \
    "${task_context}" >&2
  return 1
}

enter_provisioning() {
  local task_state
  if phase_done product_state_provisioning; then
    return 0
  fi
  if ! phase_done provisioning_request_started; then
    record_phase provisioning_request_started
  fi
  task_state="$(detect_lifecycle)"
  if [[ "${task_state}" == "OPEN" ]]; then
    # Actual OPEN proves a previous request did not take effect. Reissuing the
    # same monotonic target is safe; no OBKey is submitted here.
    "${task_cli}" "${task_connect_hotplug[@]}" -ob PRODUCT_STATE=0x17
    "${task_cli}" "${task_connect_hotplug[@]}" -hardRst
  elif [[ "${task_state}" != "PROVISIONING" ]]; then
    printf 'Cannot enter/resume PROVISIONING from actual lifecycle %s.\n' \
      "${task_state}" >&2
    return 1
  fi
  wait_provisioning_rss_ready "first OBKey"
  record_phase product_state_provisioning
}

verify_obkey_evidence() {
  local task_stem="$1"
  local task_obk="$2"
  local task_log="${task_transaction_dir}/${task_stem}-sdp.log"
  local task_result="${task_transaction_dir}/${task_stem}-sdp.result"
  python3 - "${task_log}" "${task_result}" "${task_obk}" <<'PY'
import hashlib
import pathlib
import sys

log_path, result_path, obk_path = map(pathlib.Path, sys.argv[1:])
if not log_path.is_file() or not result_path.is_file():
    raise SystemExit("OBKey submission is ambiguous: synchronized result evidence is missing")
fields = {}
for line in result_path.read_text(encoding="utf-8").splitlines():
    key, separator, value = line.partition("=")
    if not separator or key in fields:
        raise SystemExit("OBKey result evidence is malformed")
    fields[key] = value
if set(fields) != {"exit_code", "log_sha256", "obk_sha256"} or fields["exit_code"] != "0":
    raise SystemExit("OBKey submission did not have an authoritative zero exit result")
log = log_path.read_bytes()
if not log:
    raise SystemExit("OBKey Programmer log is empty")
if hashlib.sha256(log).hexdigest() != fields["log_sha256"]:
    raise SystemExit("OBKey Programmer log hash changed")
if hashlib.sha256(obk_path.read_bytes()).hexdigest() != fields["obk_sha256"]:
    raise SystemExit("OBKey bytes differ from synchronized submission evidence")
print(f"Synchronized OBKey success evidence verified: {obk_path.name}")
PY
}

submit_obkey() {
  local task_stem="$1"
  local task_started_phase="$2"
  local task_complete_phase="$3"
  local task_obk="${task_transaction_dir}/$4"
  local task_log="${task_transaction_dir}/${task_stem}-sdp.log"
  local task_result="${task_transaction_dir}/${task_stem}-sdp.result"
  local task_log_tmp="${task_log}.tmp"
  local task_result_tmp="${task_result}.tmp"
  local task_status
  local task_log_hash
  local task_obk_hash

  if phase_done "${task_complete_phase}"; then
    verify_obkey_evidence "${task_stem}" "${task_obk}"
    return 0
  fi
  if phase_done "${task_started_phase}"; then
    if verify_obkey_evidence "${task_stem}" "${task_obk}"; then
      record_phase "${task_complete_phase}"
      return 0
    fi
    printf 'AMBIGUOUS SDP: %s started without durable success evidence.\n' \
      "${task_stem}" >&2
    printf 'Do not resubmit another OBKey or request CLOSED; preserve this exact transaction for controlled recovery.\n' >&2
    return 1
  fi

  record_phase "${task_started_phase}"
  "${task_cli}" "${task_connect_hotplug[@]}" -hardRst
  wait_provisioning_rss_ready "${task_stem}"
  set +e
  "${task_cli}" "${task_connect_hotplug[@]}" -sdp "${task_obk}" 2>&1 | \
    tee "${task_log_tmp}"
  task_status=${PIPESTATUS[0]}
  set -e
  mv -- "${task_log_tmp}" "${task_log}"
  sync -f "${task_log}"
  task_log_hash="$(sha256sum "${task_log}" | awk '{print $1}')"
  task_obk_hash="$(sha256sum "${task_obk}" | awk '{print $1}')"
  printf 'exit_code=%s\nlog_sha256=%s\nobk_sha256=%s\n' \
    "${task_status}" "${task_log_hash}" "${task_obk_hash}" > "${task_result_tmp}"
  mv -- "${task_result_tmp}" "${task_result}"
  sync -f "${task_result}"
  sync -f "${task_transaction_dir}"
  if ! verify_obkey_evidence "${task_stem}" "${task_obk}"; then
    printf 'OBKey result is ambiguous; exact-package replay is intentionally disabled.\n' >&2
    return 1
  fi
  record_phase "${task_complete_phase}"
}

verify_preclose_da() {
  local task_output="${task_transaction_dir}/preclose-da-discovery.log"
  local task_canonical_temp
  if ! phase_done preclose_da_discovery_started; then
    record_phase preclose_da_discovery_started
  fi
  run_fresh_preclose_da
  if phase_done preclose_da_integrity_verified; then
    # Keep the first committed discovery as immutable write-ahead evidence,
    # while also requiring the newly captured actual-target attempt above.
    verify_preclose_da_log "${task_output}"
    return 0
  fi
  task_canonical_temp="$(mktemp --suffix=.log \
    "${task_transaction_dir}/.preclose-da-commit-XXXXXXXX")"
  cp -- "${task_fresh_preclose_log}" "${task_canonical_temp}"
  sync -f "${task_canonical_temp}"
  mv -- "${task_canonical_temp}" "${task_output}"
  sync -f "${task_output}"
  sync -f "${task_transaction_dir}"
  verify_preclose_da_log "${task_output}"
  record_phase preclose_da_integrity_verified
}

run_fresh_preclose_da() {
  local task_status
  local task_reset_status
  task_fresh_preclose_log="$(mktemp --suffix=.log \
    "${task_transaction_dir}/preclose-da-attempt-XXXXXXXX")"
  "${task_cli}" "${task_connect_hotplug[@]}" -hardRst
  wait_provisioning_rss_ready "pre-close DA discovery"
  set +e
  "${task_cli}" "${task_connect_da[@]}" debugauth=2 2>&1 | \
    tee "${task_fresh_preclose_log}"
  task_status=${PIPESTATUS[0]}
  set -e
  sync -f "${task_fresh_preclose_log}"
  sync -f "${task_transaction_dir}"
  if [[ "${task_status}" -ne 0 ]] || \
     ! verify_preclose_da_log "${task_fresh_preclose_log}"; then
    printf 'Pre-close DA discovery did not prove PROVISIONING integrity.\n' >&2
    return 1
  fi
  # DA discovery leaves AP1 unavailable until reset.  A Hotplug -hardRst
  # cannot issue that reset because CubeProgrammer first tries to reopen AP1.
  # HWRSTPULSE drives the already-proven physical NRST path without requiring
  # an access-port connection. CubeProgrammer 2.23.0 can still return
  # DEV_CONNECT_ERR after delivering the pulse because its post-pulse probe
  # finds AP1 closed by discovery. Do not treat that ambiguous transport
  # status as success: re-prove the resulting RSS state authoritatively before
  # any lifecycle write.
  set +e
  "${task_cli}" -c port=SWD "sn=${task_probe}" mode=HWRSTPULSE
  task_reset_status=$?
  set -e
  printf 'Post-discovery HWRSTPULSE Programmer exit code: %s; validating actual RSS state.\n' \
    "${task_reset_status}"
  wait_provisioning_rss_ready "CLOSED lifecycle write"
}

verify_preclose_da_log() {
  local task_output="$1"
  local task_normalized
  task_normalized="$(strip_terminal_sequences < "${task_output}")"
  if ! provisioning_da_discovery_is_exact "${task_normalized}"; then
    return 1
  fi
  # CubeProgrammer discovery describes symbolic authorized actions rather than
  # echoing the raw 0x00004040 OBKey mask. The exact policy is proven earlier
  # by deterministic DA_Config/chain regeneration; discovery is authoritative
  # here only for the target lifecycle and provisioning-integrity word.
  printf 'Pre-close DA lifecycle/integrity evidence verified\n'
}

verify_closed_da() {
  local task_output="${task_transaction_dir}/closed-da-discovery.log"
  local task_temp="${task_output}.tmp"
  local task_normalized
  local task_try
  local task_status
  for task_try in $(seq 1 5); do
    set +e
    "${task_cli}" "${task_connect_da[@]}" debugauth=2 2>&1 | tee "${task_temp}"
    task_status=${PIPESTATUS[0]}
    set -e
    task_normalized="$(strip_terminal_sequences < "${task_temp}")"
    if [[ "${task_status}" -eq 0 ]] &&
       closed_da_discovery_is_exact "${task_normalized}"; then
      mv -- "${task_temp}" "${task_output}"
      sync -f "${task_output}"
      sync -f "${task_transaction_dir}"
      if ! phase_done product_state_closed_verified; then
        record_phase product_state_closed_verified
      fi
      return 0
    fi
    sleep 0.2
  done
  printf 'CLOSED DA discovery or provisioning integrity is not authoritative.\n' >&2
  return 1
}

request_closed() {
  local task_state
  local task_attempt
  if phase_done product_state_closed_verified; then
    verify_closed_da
    return 0
  fi
  if ! phase_done closed_request_started; then
    record_phase closed_request_started
  fi
  task_state="$(detect_lifecycle)"
  if [[ "${task_state}" == "CLOSED" ]]; then
    verify_closed_da
    return 0
  fi
  if [[ "${task_state}" != "PROVISIONING" ]]; then
    printf 'CLOSED request recovery cannot classify actual lifecycle: %s\n' \
      "${task_state}" >&2
    return 1
  fi
  # Reissuing 0x72 is allowed only after authoritative actual 0x17 plus all
  # three durable SDP completions and pre-close DA integrity evidence.
  for task_attempt in da_config_complete oemirot_config_complete \
      oemirot_data_complete preclose_da_integrity_verified; do
    if ! phase_done "${task_attempt}"; then
      printf 'CLOSED request lacks required durable phase: %s\n' \
        "${task_attempt}" >&2
      return 1
    fi
  done
  verify_obkey_evidence da_config "${task_transaction_dir}/DA_Config.obk"
  verify_obkey_evidence oemirot_config "${task_transaction_dir}/OEMiRoT_Config.obk"
  verify_obkey_evidence oemirot_data "${task_transaction_dir}/OEMiRoT_Data.obk"
  # Historical phase/log evidence is necessary but never substitutes for a
  # fresh discovery of the currently connected PROVISIONING target. This is
  # repeated on every 0x72 attempt, including interrupted-request recovery.
  verify_preclose_da
  task_attempt="$(date -u +'%Y%m%dT%H%M%SZ')-$$"
  set +e
  "${task_cli}" "${task_connect_hotplug[@]}" -ob PRODUCT_STATE=0x72 \
    > "${task_transaction_dir}/closed-request-${task_attempt}.log" 2>&1
  local task_result=$?
  set -e
  printf 'CLOSED request Programmer exit code: %s (disconnect may be expected)\n' \
    "${task_result}"
  sync -f "${task_transaction_dir}/closed-request-${task_attempt}.log"
  verify_closed_da
}

continue_provisioning() {
  wait_provisioning_rss_ready "CLOSED resume"
  submit_obkey da_config da_config_started da_config_complete DA_Config.obk
  submit_obkey oemirot_config oemirot_config_started \
    oemirot_config_complete OEMiRoT_Config.obk
  submit_obkey oemirot_data oemirot_data_started \
    oemirot_data_complete OEMiRoT_Data.obk
  verify_preclose_da
  request_closed
}

continue_closed_transaction() {
  local task_decision
  local task_state
  verify_closed_transaction
  # A power loss after the manifest fsync but before either evidence-phase
  # append is recoverable: the complete immutable evidence was independently
  # revalidated, and no target mutation phase can precede these markers.
  if ! phase_done pairing_store_dual_verified; then
    record_phase pairing_store_dual_verified
  fi
  if ! phase_done pre_mutation_evidence_verified; then
    record_phase pre_mutation_evidence_verified
  fi
  task_state="$(detect_lifecycle)"
  task_decision="$(python3 "${task_preflight_verifier}" resume-decision \
    "${task_phase_file}" --lifecycle "${task_state}")"
  case "${task_decision}" in
    OPEN_BOOT)
      continue_open_boot_install
      enter_provisioning
      continue_provisioning
      ;;
    PROVISIONING)
      if ! phase_done product_state_provisioning; then
        record_phase product_state_provisioning
      fi
      continue_provisioning
      ;;
    CLOSED_VERIFY)
      verify_closed_da
      ;;
    *)
      printf 'Cannot authoritatively classify target lifecycle; no mutation performed.\n' >&2
      return 1
      ;;
  esac
}

preflight() {
  local task_temp
  task_temp="$(mktemp -d)"
  check_inputs
  check_open_target
  check_programmer_mcu_uid "${task_temp}/programmer-mcu-uid.bin"
  "${task_cli}" "${task_connect_reset[@]}" -rst
  check_runtime_safe "${task_temp}/runtime.json"
  check_nrst_path
  check_ota_idle "${task_temp}/ota.json"
  rm -rf -- "${task_temp}"
}

case "${task_action}" in
  preflight)
    preflight
    ;;
  finalize)
    if [[ "${task_confirmation}" != "--accept-irreversible-closed" ]]; then
      printf '%s\n' \
        'This installs ReleaseClosed, provisions HDPL1 OBKeys, and irreversibly changes' \
        'PRODUCT_STATE to CLOSED. Re-run with:' \
        '  ./tools/finalize_oemirot_closed.sh finalize --accept-irreversible-closed' >&2
      exit 4
    fi
    preflight
    create_closed_transaction
    capture_closed_transaction_evidence
    continue_closed_transaction
    printf '\nCLOSED and DA provisioning integrity verified.\n'
    printf 'Cold-power-cycle only the ECU, then verify fresh Ethernet runtime MCU/ATECC identity and zero outputs.\n'
    ;;
  resume-closed-transition)
    if [[ "${task_confirmation}" != "--accept-same-physical-target" ]]; then
      printf '%s\n' \
        'Resume requires the exact immutable transaction and explicit confirmation that' \
        'the same physical ECU and ST-Link remained connected:' \
        '  ECU_CLOSED_TRANSACTION_DIR=/absolute/...-closed-transition \' \
        '  ./tools/finalize_oemirot_closed.sh resume-closed-transition --accept-same-physical-target' >&2
      exit 4
    fi
    select_closed_transaction
    continue_closed_transaction
    printf '\nCLOSED transaction resume reached verified CLOSED state.\n'
    ;;
  *)
    printf 'Usage: %s {preflight|finalize --accept-irreversible-closed|resume-closed-transition --accept-same-physical-target}\n' \
      "$0" >&2
    exit 2
    ;;
esac

trap - EXIT
