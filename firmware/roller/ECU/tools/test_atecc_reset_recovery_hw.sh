#!/usr/bin/env bash
set -euo pipefail

# Reproduce the most hostile reset ordering used by the CLOSED finalizer:
# option-byte display/reset, immediate UID read under physical reset, then an
# immediate final reset.  Every cycle must recover the continuously powered
# ATECC608C, authenticate the exact paired identity, remain de-energized, and
# return to the confirmed OTA baseline.  This tool never programs Flash or
# option bytes.

task_cycles="${ECU_RESET_RECOVERY_CYCLES:-20}"
task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_cli="${STM32_PROGRAMMER_CLI:-/home/plac/.local/share/stm32cube/bundles/programmer/2.23.0/bin/STM32_Programmer_CLI}"
task_probe="${STLINK_SERIAL:-066BFF565456857187210935}"
task_interface="${ECU_INTERFACE:-enp2s0}"
task_ecu_ip="${ECU_IP:-172.16.0.11}"
task_ui_url="${ECU_UI_URL:-http://127.0.0.1:18088}"
task_stamp="$(date -u +%Y%m%dT%H%M%SZ)"
task_output_dir="${ECU_RESET_RECOVERY_OUTPUT_DIR:-${task_project_dir}/artifacts/hardware-regression/${task_stamp}-atecc-reset-recovery}"
task_verifier="${task_script_dir}/verify_closed_preflight.py"

if ! [[ "${task_cycles}" =~ ^[1-9][0-9]*$ ]] ||
   (( task_cycles > 100 )); then
  printf 'ECU_RESET_RECOVERY_CYCLES must be an integer in 1..100.\n' >&2
  exit 2
fi

for task_input in "${task_cli}" "${task_verifier}" \
    "${task_script_dir}/ethernet_ota.py"; do
  if [[ ! -f "${task_input}" ]]; then
    printf 'Missing reset-recovery input: %s\n' "${task_input}" >&2
    exit 2
  fi
done

mkdir -p -- "${task_output_dir}"
printf 'schema=roller-ecu-atecc-reset-recovery-hw-v1\n' \
  > "${task_output_dir}/summary.txt"
printf 'probe=%s\ncycles=%s\ninterface=%s\necu_ip=%s\n' \
  "${task_probe}" "${task_cycles}" "${task_interface}" "${task_ecu_ip}" \
  >> "${task_output_dir}/summary.txt"

for task_cycle in $(seq 1 "${task_cycles}"); do
  task_prefix="${task_output_dir}/cycle-$(printf '%03d' "${task_cycle}")"
  printf 'ATECC reset-recovery cycle %d/%d ... ' \
    "${task_cycle}" "${task_cycles}"

  "${task_cli}" \
    -c port=SWD "sn=${task_probe}" ap=1 mode=UR reset=HWrst \
    -ob displ -rst > "${task_prefix}-option-bytes.txt" 2>&1
  "${task_cli}" \
    -c port=SWD "sn=${task_probe}" ap=1 mode=UR reset=HWrst \
    -u 0x08FFF800 0xC "${task_prefix}-mcu-uid.bin" \
    > "${task_prefix}-uid-read.txt" 2>&1
  python3 "${task_verifier}" programmer-uid \
    "${task_prefix}-mcu-uid.bin" > "${task_prefix}-uid-verify.txt"
  "${task_cli}" \
    -c port=SWD "sn=${task_probe}" ap=1 mode=UR reset=HWrst \
    -rst > "${task_prefix}-final-reset.txt" 2>&1

  task_online=0
  for task_try in $(seq 1 40); do
    if ping -I "${task_interface}" -c 1 -W 1 "${task_ecu_ip}" \
        > /dev/null 2>&1; then
      task_online=1
      break
    fi
    sleep 0.2
  done
  if [[ "${task_online}" -ne 1 ]]; then
    printf 'FAIL (Ethernet timeout)\n'
    exit 1
  fi

  task_fresh=0
  for task_try in $(seq 1 40); do
    if curl --max-time 3 -fsS "${task_ui_url}/api/state" \
        -o "${task_prefix}-runtime.json" &&
       python3 "${task_verifier}" runtime-state \
        "${task_prefix}-runtime.json" > "${task_prefix}-runtime-verify.txt" \
        2>&1 &&
       python3 - "${task_prefix}-runtime.json" <<'PY'
import json
import sys

state = json.load(open(sys.argv[1], encoding="utf-8"))
uptime = (state.get("diagnostic") or {}).get("secure_uptime_ms")
if type(uptime) is not int or not 0 <= uptime < 10000:
    raise SystemExit(f"post-reset Secure uptime is not fresh: {uptime!r}")
PY
    then
      task_fresh=1
      break
    fi
    sleep 0.2
  done
  if [[ "${task_fresh}" -ne 1 ]]; then
    printf 'FAIL (ATECC/runtime safety state)\n'
    exit 1
  fi

  python3 "${task_script_dir}/ethernet_ota.py" --status \
    > "${task_prefix}-ota.json"
  python3 "${task_verifier}" ota-status "${task_prefix}-ota.json" \
    > "${task_prefix}-ota-verify.txt"
  ping -I "${task_interface}" -c 2 -W 1 "${task_ecu_ip}" \
    > "${task_prefix}-ping.txt"
  printf 'cycle=%d result=PASS\n' "${task_cycle}" \
    >> "${task_output_dir}/summary.txt"
  printf 'PASS\n'
done

sha256sum "${task_output_dir}"/cycle-*-mcu-uid.bin \
  "${task_output_dir}"/cycle-*-runtime.json \
  "${task_output_dir}"/cycle-*-ota.json \
  > "${task_output_dir}/evidence-sha256.txt"
printf 'result=PASS\n' >> "${task_output_dir}/summary.txt"
printf 'ATECC reset-recovery hardware regression PASS: %s cycles\n' \
  "${task_cycles}"
printf 'Evidence: %s\n' "${task_output_dir}"
