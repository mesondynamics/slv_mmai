#!/usr/bin/env bash
set -euo pipefail

# Irreversible STM32H563 OPEN -> PROVISIONING -> CLOSED transition. This uses
# ST's reviewed H563 OEMiROT ordering and never invokes Full Regression.

task_action="${1:-preflight}"
task_confirmation="${2:-}"
task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_cli="${STM32_PROGRAMMER_CLI:-/home/plac/.local/share/stm32cube/bundles/programmer/2.23.0/bin/STM32_Programmer_CLI}"
task_probe="${STLINK_SERIAL:-066BFF565456857187210935}"
task_assets="${task_project_dir}/artifacts/security-provisioning"
task_backup_root="${ECU_BACKUP_DIR:-${task_project_dir}/artifacts/device-backups}"
task_boot_image="${task_project_dir}/Bootloader/OEMiROT/build/ReleaseClosed/ECU_OEMiROT.bin"
task_connect_hotplug=( -c port=SWD "sn=${task_probe}" ap=1 mode=Hotplug )
task_connect_reset=( -c port=SWD "sn=${task_probe}" ap=1 mode=UR reset=HWrst )
task_connect_da=( -c port=SWD "sn=${task_probe}" speed=fast )
task_backup_prefix=""
task_phase_file=""

strip_terminal_sequences() {
  sed -E $'s/\x1B\[[0-9;]*[mK]//g'
}

record_phase() {
  printf '%s\n' "$1" >> "${task_phase_file}"
  printf 'PHASE: %s\n' "$1"
}

failure_notice() {
  local task_status=$?
  if [[ ${task_status} -ne 0 && -n "${task_backup_prefix}" ]]; then
    printf '\nCLOSED transition stopped. Last phase and recovery backup:\n' >&2
    printf '  %s\n  %s-*\n' "${task_phase_file}" "${task_backup_prefix}" >&2
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
  require_option "${task_options}" 'PRODUCT_STATE[[:space:]]*: 0xED[[:space:]]+[(]Open[)]' \
    'target must still be OPEN'
  require_option "${task_options}" 'TZEN[[:space:]]*: 0xB4[[:space:]]' \
    'TrustZone must be enabled'
  require_option "${task_options}" 'SECBOOT_LOCK[[:space:]]*: 0xB4[[:space:]]' \
    'secure boot address must be locked'
  require_option "${task_options}" 'SECBOOTADD[[:space:]]*: 0xC0000[[:space:]]+[(]0x[Cc]000000[)]' \
    'secure boot address must be 0x0C000000'
  require_option "${task_options}" 'WRPSGn1[[:space:]]*: 0xFFFFFFF0[[:space:]]' \
    'OEMiROT WRP groups must be active'
  require_option "${task_options}" 'HDP1_STRT[[:space:]]*: 0x0[[:space:]]' \
    'HDP start must protect OEMiROT'
  require_option "${task_options}" 'HDP1_END[[:space:]]*: 0x17[[:space:]]' \
    'HDP end must cover boot and scratch'
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
  local task_state
  local task_try
  local task_fresh=0
  task_state="$(mktemp)"
  wait_ecu_network
  # Programmer's option-byte display resets the MCU.  ICMP can recover before
  # the UI backend has received fresh periodic status/diagnostic messages, so
  # wait for both protocol streams instead of treating that normal window as a
  # failed safety check.
  for task_try in $(seq 1 25); do
    if curl --max-time 3 -fsS \
      http://127.0.0.1:18088/api/state -o "${task_state}" && \
      python3 - "${task_state}" <<'PY'
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
    rm -f -- "${task_state}"
    printf 'Debug UI did not receive fresh ECU status after reset.\n' >&2
    return 1
  fi
  python3 - "${task_state}" <<'PY'
import json
import sys
state = json.load(open(sys.argv[1], encoding="utf-8"))
security = state.get("security") or {}
diagnostic = state.get("diagnostic") or {}
status = state.get("status") or {}
flags = int(security.get("flags", 0))
if int(state.get("status_age_ms", 999999)) > 500 or int(state.get("diagnostic_age_ms", 999999)) > 500:
    raise SystemExit("ECU UI status is stale")
if security.get("auth_result") != 0 or not flags & (1 << 4) or flags & (1 << 5):
    raise SystemExit("ATECC startup authentication is not healthy")
if security.get("config_locked") != 1 or security.get("data_locked") != 1:
    raise SystemExit("ATECC zones are not locked")
if int(diagnostic.get("requested_relay_mask", -1)) != 0 or int(diagnostic.get("applied_relay_mask", -1)) != 0:
    raise SystemExit("actuator relay mask is not zero")
if int(status.get("forward_duty_permille", -1)) != 0 or int(status.get("reverse_duty_permille", -1)) != 0:
    raise SystemExit("valve PWM is not zero")
if state.get("tuning_active") is not False:
    raise SystemExit("PI tuning telemetry is active")
print("ATECC authentication, zero outputs, and inactive tuning verified")
PY
  rm -f -- "${task_state}"
}

read_secure_uptime() {
  curl --max-time 3 -fsS http://127.0.0.1:18088/api/state |
    python3 -c 'import json,sys; print(int((json.load(sys.stdin).get("diagnostic") or {}).get("secure_uptime_ms", -1)))'
}

check_nrst_path() {
  local task_before
  local task_after
  task_before="$(read_secure_uptime)"
  if [[ "${task_before}" -lt 0 ]]; then
    printf 'Cannot read Secure uptime before the NRST propagation test.\n' >&2
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
    printf 'ST-Link HWRSTPULSE failed; verify the NRST wire.\n' >&2
    return 1
  fi
  wait_ecu_network
  check_runtime_safe
  task_after="$(read_secure_uptime)"
  if [[ "${task_after}" -lt 0 || "${task_after}" -ge "${task_before}" ]]; then
    printf 'NRST did not reset Secure uptime (%s -> %s).\n' \
      "${task_before}" "${task_after}" >&2
    return 1
  fi
  printf 'Physical ST-Link NRST propagation verified (%s -> %s ms).\n' \
    "${task_before}" "${task_after}"
}

check_inputs() {
  local task_boot_size
  require_file "${task_cli}"
  require_file "${task_boot_image}"
  require_file "${task_assets}/DA_Config.obk"
  require_file "${task_assets}/OEMiRoT_Config.obk"
  require_file "${task_assets}/OEMiRoT_Data.obk"
  require_file "${task_assets}/security-assets.json"
  task_boot_size="$(stat -c '%s' -- "${task_boot_image}")"
  if [[ "${task_boot_size}" -le 0 || "${task_boot_size}" -gt 131072 ]]; then
    printf 'ReleaseClosed OEMiROT does not fit 128 KiB: %s bytes\n' \
      "${task_boot_size}" >&2
    exit 2
  fi
  python3 "${task_script_dir}/ecu_debug_auth.py" verify-assets \
    --programmer "${task_cli}" --probe "${task_probe}"
  python3 - "${task_assets}/DA_Config.obk" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
if len(data) != 108 or data[76:92] != bytes.fromhex("40400000000000000000000000000000"):
    raise SystemExit("DA_Config.obk permission payload is not 0x00004040")
print("DA_Config raw permission payload verified: 0x00004040")
PY
  sha256sum "${task_boot_image}" "${task_assets}/"*.obk \
    "${task_assets}/cert-leaf-chain.b64"
}

create_backup() {
  local task_timestamp
  task_timestamp="$(date -u +'%Y%m%dT%H%M%SZ')"
  umask 077
  mkdir -p -- "${task_backup_root}"
  task_backup_prefix="${task_backup_root}/stm32h563-${task_probe}-${task_timestamp}-before-closed"
  task_phase_file="${task_backup_prefix}-phase.txt"
  read_open_options > "${task_backup_prefix}-option-bytes.txt"
  "${task_cli}" "${task_connect_reset[@]}" \
    -u 0x08000000 0x200000 "${task_backup_prefix}-flash.bin"
  "${task_cli}" "${task_connect_reset[@]}" \
    -u 0x080E0000 0x20000 "${task_backup_prefix}-persistent.bin"
  cp -a -- "${task_assets}" "${task_backup_prefix}-security-assets"
  sha256sum "${task_backup_prefix}-flash.bin" \
    "${task_backup_prefix}-persistent.bin" \
    "${task_boot_image}" > "${task_backup_prefix}-sha256.txt"
  record_phase backup_complete
}

install_release_closed_boot() {
  "${task_cli}" "${task_connect_reset[@]}" \
    -ob WRPSGn1=0xFFFFFFFF WRPSGn2=0xFFFFFFFF \
        HDP1_STRT=0x1 HDP1_END=0x0 HDP2_STRT=0x1 HDP2_END=0x0
  record_phase boot_protection_removed
  "${task_cli}" "${task_connect_reset[@]}" \
    -d "${task_boot_image}" 0x0C000000 -v
  record_phase release_closed_boot_verified
  "${task_cli}" "${task_connect_reset[@]}" \
    -ob WRPSGn1=0xFFFFFFF0 WRPSGn2=0xFFFFFFFF \
        HDP1_STRT=0x0 HDP1_END=0x17 HDP2_STRT=0x1 HDP2_END=0x0
  record_phase boot_protection_restored
  # ReleaseClosed deliberately refuses to run while the product is OPEN. Hold
  # the core under hardware reset before the following Hot Plug lifecycle
  # write, otherwise the boot image can enter its fail-safe loop between the
  # two independent CubeProgrammer invocations.
  "${task_cli}" "${task_connect_reset[@]}" -halt -score
  record_phase release_closed_halted_before_provisioning
}

enter_provisioning_and_program_obkeys() {
  "${task_cli}" "${task_connect_hotplug[@]}" -ob PRODUCT_STATE=0x17
  record_phase product_state_provisioning
  for task_obk in DA_Config.obk OEMiRoT_Config.obk OEMiRoT_Data.obk; do
    "${task_cli}" "${task_connect_hotplug[@]}" -hardRst
    "${task_cli}" "${task_connect_hotplug[@]}" \
      -sdp "${task_assets}/${task_obk}"
    record_phase "${task_obk}_provisioned"
  done
  "${task_cli}" "${task_connect_hotplug[@]}" -hardRst
}

set_closed() {
  local task_result
  set +e
  "${task_cli}" "${task_connect_hotplug[@]}" -ob PRODUCT_STATE=0x72
  task_result=$?
  set -e
  printf 'CLOSED transition Programmer exit code: %s\n' "${task_result}"
  record_phase product_state_closed_requested
  # A disconnect is expected when CLOSED takes effect; authoritative checks
  # are DA discovery and the running ReleaseClosed application.
}

verify_closed_da() {
  local task_try
  local task_output=""
  local task_result=1
  for task_try in $(seq 1 5); do
    set +e
    task_output="$("${task_cli}" "${task_connect_da[@]}" debugauth=2 2>&1)"
    task_result=$?
    set -e
    printf '%s\n' "${task_output}"
    if [[ "${task_result}" -eq 0 ]] &&
       grep -Eq 'PSA lifecycle[.]*:[[:space:]]*ST_LIFECYCLE_CLOSED' \
         <<<"${task_output}" &&
       grep -Eq 'integrity status message:[[:space:]]*VALID' \
         <<<"${task_output}"; then
      record_phase product_state_closed_verified
      return 0
    fi
    sleep 0.2
  done
  printf 'CLOSED DA discovery or provisioning-integrity verification failed.\n' >&2
  return 1
}

preflight() {
  check_inputs
  check_open_target
  check_runtime_safe
  check_nrst_path
  python3 "${task_script_dir}/ethernet_ota.py" --status
}

case "${task_action}" in
  preflight)
    preflight
    ;;
  finalize)
    if [[ "${task_confirmation}" != "--accept-irreversible-closed" ]]; then
      printf '%s\n' \
        'This installs ReleaseClosed, reprovisions all HDPL1 OBKeys, and changes' \
        'PRODUCT_STATE to CLOSED. Re-run with:' \
        '  ./tools/finalize_oemirot_closed.sh finalize --accept-irreversible-closed' >&2
      exit 4
    fi
    preflight
    create_backup
    install_release_closed_boot
    enter_provisioning_and_program_obkeys
    set_closed
    verify_closed_da
    printf '\nCLOSED verified. Hardware power-cycle the ECU, then verify Ethernet and:\n'
    printf '  python3 tools/ecu_debug_auth.py --accept-closed-target discover\n'
    ;;
  *)
    printf 'Usage: %s {preflight|finalize [--accept-irreversible-closed]}\n' "$0" >&2
    exit 2
    ;;
esac

trap - EXIT
