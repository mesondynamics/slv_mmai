#!/usr/bin/env bash
set -euo pipefail

task_interface="${ECU_INTERFACE:-enp2s0}"
task_target="${ECU_IP:-172.16.0.11}"
task_count="${ECU_PING_COUNT:-100}"
task_average_limit_ms="${ECU_PING_AVG_LIMIT_MS:-1.0}"
task_maximum_limit_ms="${ECU_PING_MAX_LIMIT_MS:-5.0}"

if ! ip route get "${task_target}" | grep -Eq "dev[[:space:]]+${task_interface}([[:space:]]|$)"; then
  printf 'ECU route does not use %s: %s\n' "${task_interface}" "${task_target}" >&2
  exit 2
fi

task_output="$(ping -n -I "${task_interface}" -c "${task_count}" -i 0.02 \
  -W 1 "${task_target}")"
printf '%s\n' "${task_output}"
task_packet_loss="$(printf '%s\n' "${task_output}" | \
  awk -F',' '/packets transmitted/ {value=$3; gsub(/[^0-9.]/, "", value); print value}')"
if [[ -z "${task_packet_loss}" ]] || ! awk -v value="${task_packet_loss}" \
  'BEGIN {exit !(value == 0)}'; then
  printf 'FAIL  packet loss is %s%%; zero loss is required\n' \
    "${task_packet_loss:-unknown}" >&2
  exit 1
fi
task_values="$(printf '%s\n' "${task_output}" | \
  awk -F' = ' '/^(rtt|round-trip)/ {split($2,a," "); print a[1]}')"
if [[ -z "${task_values}" ]]; then
  echo 'Unable to parse ping latency summary.' >&2
  exit 2
fi
IFS=/ read -r task_minimum task_average task_maximum task_deviation \
  <<<"${task_values}"

if ! awk -v value="${task_average}" -v limit="${task_average_limit_ms}" \
  'BEGIN {exit !(value <= limit)}'; then
  printf 'FAIL  average RTT %s ms exceeds %s ms\n' \
    "${task_average}" "${task_average_limit_ms}" >&2
  exit 1
fi
if ! awk -v value="${task_maximum}" -v limit="${task_maximum_limit_ms}" \
  'BEGIN {exit !(value <= limit)}'; then
  printf 'FAIL  maximum RTT %s ms exceeds %s ms\n' \
    "${task_maximum}" "${task_maximum_limit_ms}" >&2
  exit 1
fi
printf 'PASS  ECU RTT min/avg/max = %s/%s/%s ms (limits avg<=%s, max<=%s)\n' \
  "${task_minimum}" "${task_average}" "${task_maximum}" \
  "${task_average_limit_ms}" "${task_maximum_limit_ms}"
