#!/usr/bin/env bash
set -euo pipefail

task_action="${1:-validate}"
task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_cubemx_bin="${CUBEMX_BIN:-/home/plac/Applications/STM32CubeMX/STM32CubeMX}"
task_xauthority="${XAUTHORITY:-/run/user/$(id -u)/gdm/Xauthority}"
task_display="${DISPLAY:-:0}"
task_cli_file="$(mktemp /tmp/ecu-cubemx-XXXXXX.script)"

cleanup() {
  rm -f -- "${task_cli_file}"
}
trap cleanup EXIT

if [[ ! -x "${task_cubemx_bin}" ]]; then
  echo "STM32CubeMX executable not found: ${task_cubemx_bin}" >&2
  echo "Set CUBEMX_BIN to the STM32CubeMX executable." >&2
  exit 2
fi

case "${task_action}" in
  validate)
    {
      printf 'config load "%s/ECU.ioc"\n' "${task_project_dir}"
      printf 'config saveext "/tmp/ECU-cubemx-expanded.ioc"\n'
      printf 'exit\n'
    } >"${task_cli_file}"
    ;;
  generate)
    {
      printf 'config load "%s/ECU.ioc"\n' "${task_project_dir}"
      printf 'project generate\n'
      printf 'exit\n'
    } >"${task_cli_file}"
    ;;
  *)
    echo "Usage: $0 {validate|generate}" >&2
    exit 2
    ;;
esac

DISPLAY="${task_display}" XAUTHORITY="${task_xauthority}" \
  "${task_cubemx_bin}" -q "${task_cli_file}"
