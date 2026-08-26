#!/usr/bin/env bash
set -euo pipefail

task_interface="${ECU_INTERFACE:-enp2s0}"
task_connection="${ECU_CONNECTION:-ecu-bench}"
task_address="${ECU_HOST_ADDRESS:-172.16.0.10/16}"
task_network="${ECU_NETWORK:-172.16.0.0/16}"
task_route_rule="priority 100 to ${task_network} table 254"

if ! command -v nmcli >/dev/null 2>&1; then
  echo "NetworkManager nmcli is required." >&2
  exit 2
fi
if ! nmcli -t -f DEVICE device status | grep -Fx -- "${task_interface}" >/dev/null; then
  echo "Network interface not found: ${task_interface}" >&2
  exit 2
fi

if nmcli -g NAME connection show | grep -Fx -- "${task_connection}" >/dev/null; then
  nmcli connection modify "${task_connection}" \
    connection.interface-name "${task_interface}" \
    connection.autoconnect yes \
    ipv4.method manual \
    ipv4.addresses "${task_address}" \
    ipv4.gateway "" \
    ipv4.dns "" \
    ipv4.route-table 254 \
    ipv4.routing-rules "${task_route_rule}" \
    ipv4.never-default yes \
    ipv6.method disabled
else
  nmcli connection add type ethernet \
    ifname "${task_interface}" \
    con-name "${task_connection}" \
    connection.autoconnect yes \
    ipv4.method manual \
    ipv4.addresses "${task_address}" \
    ipv4.route-table 254 \
    ipv4.routing-rules "${task_route_rule}" \
    ipv4.never-default yes \
    ipv6.method disabled
fi

nmcli connection up "${task_connection}"
nmcli -f GENERAL.DEVICES,GENERAL.STATE,IP4.ADDRESS,IP4.ROUTE \
  connection show "${task_connection}"
