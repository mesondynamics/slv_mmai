#!/usr/bin/env bash
set -euo pipefail

task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_test_dir="$(mktemp -d /tmp/ecu-protocol-test-XXXXXX)"
task_host_cc="${CC:-cc}"
task_host_cc_flags=()
if [[ -n "${ECU_HOST_CC_FLAGS:-}" ]]; then
  read -r -a task_host_cc_flags <<< "${ECU_HOST_CC_FLAGS}"
fi

cleanup() {
  rm -rf -- "${task_test_dir}"
}
trap cleanup EXIT

if command -v "${task_host_cc}" >/dev/null 2>&1; then
  "${task_host_cc}" "${task_host_cc_flags[@]}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/NonSecure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" \
    "${task_project_dir}/NonSecure/App/Src/ecu_protocol.c" \
    "${task_project_dir}/tests/test_ecu_protocol.c" \
    -o "${task_test_dir}/ecu_protocol_test"
  "${task_test_dir}/ecu_protocol_test"
  "${task_host_cc}" "${task_host_cc_flags[@]}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/Secure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" \
    "${task_project_dir}/Secure/App/Src/valve_control.c" \
    "${task_project_dir}/tests/test_valve_control.c" \
    -o "${task_test_dir}/valve_control_test"
  "${task_test_dir}/valve_control_test"
  "${task_host_cc}" "${task_host_cc_flags[@]}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/Secure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" \
    "${task_project_dir}/Secure/App/Src/steering_control.c" \
    "${task_project_dir}/tests/test_steering_control.c" \
    -o "${task_test_dir}/steering_control_test"
  "${task_test_dir}/steering_control_test"
  "${task_host_cc}" "${task_host_cc_flags[@]}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/Secure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" \
    "${task_project_dir}/Secure/App/Src/vehicle_j1939.c" \
    "${task_project_dir}/tests/test_vehicle_j1939.c" \
    -o "${task_test_dir}/vehicle_j1939_test"
  "${task_test_dir}/vehicle_j1939_test"
  "${task_host_cc}" "${task_host_cc_flags[@]}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/NonSecure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" \
    "${task_project_dir}/NonSecure/App/Src/j1939.c" \
    "${task_project_dir}/tests/test_j1939_wrapper.c" \
    -o "${task_test_dir}/j1939_wrapper_test"
  "${task_test_dir}/j1939_wrapper_test"
  "${task_host_cc}" "${task_host_cc_flags[@]}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/NonSecure/App/Network" \
    "${task_project_dir}/tests/test_control_authority_policy.c" \
    -o "${task_test_dir}/control_authority_policy_test"
  "${task_test_dir}/control_authority_policy_test"
  "${task_host_cc}" "${task_host_cc_flags[@]}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/Secure/App/Inc" \
    "${task_project_dir}/Secure/App/Src/security_sha256.c" \
    "${task_project_dir}/tests/test_security_sha256.c" \
    -o "${task_test_dir}/security_sha256_test"
  "${task_test_dir}/security_sha256_test"
  "${task_host_cc}" "${task_host_cc_flags[@]}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/tests/stubs/atecc" \
    -I"${task_project_dir}/Secure/App/Inc" \
    "${task_project_dir}/Secure/App/Src/atecc608.c" \
    "${task_project_dir}/tests/test_atecc608_recovery.c" \
    -o "${task_test_dir}/atecc608_recovery_test"
  "${task_test_dir}/atecc608_recovery_test"
  "${task_host_cc}" "${task_host_cc_flags[@]}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/NonSecure/App/Network" \
    "${task_project_dir}/NonSecure/App/Network/ethernet_phy_policy.c" \
    "${task_project_dir}/tests/test_ethernet_phy_policy.c" \
    -o "${task_test_dir}/ethernet_phy_policy_test"
  "${task_test_dir}/ethernet_phy_policy_test"
  "${task_host_cc}" "${task_host_cc_flags[@]}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/NonSecure/App/Network" \
    "${task_project_dir}/NonSecure/App/Network/ethernet_tx_scheduler.c" \
    "${task_project_dir}/tests/test_ethernet_tx_scheduler.c" \
    -o "${task_test_dir}/ethernet_tx_scheduler_test"
  "${task_test_dir}/ethernet_tx_scheduler_test"
  "${task_host_cc}" "${task_host_cc_flags[@]}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/Secure/App/Inc" \
    "${task_project_dir}/Secure/App/Src/security_ota_policy.c" \
    "${task_project_dir}/tests/test_security_ota_policy.c" \
    -o "${task_test_dir}/security_ota_policy_test"
  "${task_test_dir}/security_ota_policy_test"
  "${task_host_cc}" "${task_host_cc_flags[@]}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/Bootloader/OEMiROT/Config" \
    "${task_project_dir}/tests/test_roller_pair_policy.c" \
    -o "${task_test_dir}/roller_pair_policy_test"
  "${task_test_dir}/roller_pair_policy_test"
else
  if [[ "${ECU_REQUIRE_NATIVE_TESTS:-0}" == "1" ]]; then
    printf 'ERROR Native C compiler unavailable; ECU_REQUIRE_NATIVE_TESTS=1 requires executed host assertions.\n' >&2
    exit 1
  fi
  task_arm_cc="${ARM_GNU_BIN_DIR:-/home/plac/.local/share/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin}/arm-none-eabi-gcc"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/NonSecure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" -c \
    "${task_project_dir}/NonSecure/App/Src/ecu_protocol.c" \
    -o "${task_test_dir}/ecu_protocol.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/NonSecure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" -c \
    "${task_project_dir}/tests/test_ecu_protocol.c" \
    -o "${task_test_dir}/test_ecu_protocol.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/Secure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" -c \
    "${task_project_dir}/Secure/App/Src/valve_control.c" \
    -o "${task_test_dir}/valve_control.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/Secure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" -c \
    "${task_project_dir}/tests/test_valve_control.c" \
    -o "${task_test_dir}/test_valve_control.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/Secure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" -c \
    "${task_project_dir}/Secure/App/Src/steering_control.c" \
    -o "${task_test_dir}/steering_control.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/Secure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" -c \
    "${task_project_dir}/tests/test_steering_control.c" \
    -o "${task_test_dir}/test_steering_control.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/Secure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" -c \
    "${task_project_dir}/Secure/App/Src/vehicle_j1939.c" \
    -o "${task_test_dir}/vehicle_j1939.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/Secure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" -c \
    "${task_project_dir}/tests/test_vehicle_j1939.c" \
    -o "${task_test_dir}/test_vehicle_j1939.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/NonSecure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" -c \
    "${task_project_dir}/NonSecure/App/Src/j1939.c" \
    -o "${task_test_dir}/j1939_wrapper.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/NonSecure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" -c \
    "${task_project_dir}/tests/test_j1939_wrapper.c" \
    -o "${task_test_dir}/test_j1939_wrapper.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/NonSecure/App/Network" -c \
    "${task_project_dir}/tests/test_control_authority_policy.c" \
    -o "${task_test_dir}/test_control_authority_policy.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/Secure/App/Inc" -c \
    "${task_project_dir}/Secure/App/Src/security_sha256.c" \
    -o "${task_test_dir}/security_sha256.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/Secure/App/Inc" -c \
    "${task_project_dir}/tests/test_security_sha256.c" \
    -o "${task_test_dir}/test_security_sha256.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/tests/stubs/atecc" \
    -I"${task_project_dir}/Secure/App/Inc" -c \
    "${task_project_dir}/Secure/App/Src/atecc608.c" \
    -o "${task_test_dir}/atecc608.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/tests/stubs/atecc" \
    -I"${task_project_dir}/Secure/App/Inc" -c \
    "${task_project_dir}/tests/test_atecc608_recovery.c" \
    -o "${task_test_dir}/test_atecc608_recovery.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/NonSecure/App/Network" -c \
    "${task_project_dir}/NonSecure/App/Network/ethernet_phy_policy.c" \
    -o "${task_test_dir}/ethernet_phy_policy.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/NonSecure/App/Network" -c \
    "${task_project_dir}/tests/test_ethernet_phy_policy.c" \
    -o "${task_test_dir}/test_ethernet_phy_policy.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/NonSecure/App/Network" -c \
    "${task_project_dir}/NonSecure/App/Network/ethernet_tx_scheduler.c" \
    -o "${task_test_dir}/ethernet_tx_scheduler.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/NonSecure/App/Network" -c \
    "${task_project_dir}/tests/test_ethernet_tx_scheduler.c" \
    -o "${task_test_dir}/test_ethernet_tx_scheduler.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/Secure/App/Inc" -c \
    "${task_project_dir}/Secure/App/Src/security_ota_policy.c" \
    -o "${task_test_dir}/security_ota_policy.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/Secure/App/Inc" -c \
    "${task_project_dir}/tests/test_security_ota_policy.c" \
    -o "${task_test_dir}/test_security_ota_policy.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/Bootloader/OEMiROT/Config" -c \
    "${task_project_dir}/tests/test_roller_pair_policy.c" \
    -o "${task_test_dir}/test_roller_pair_policy.o"
  printf 'INFO  Native C compiler unavailable; protocol/control C tests received ARM compile-only compatibility checks; behavior assertions were not executed.\n'
fi

python3 -m unittest discover -s "${task_project_dir}/tests" -p 'test_*.py' -v
