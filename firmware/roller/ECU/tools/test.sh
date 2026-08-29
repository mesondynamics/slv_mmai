#!/usr/bin/env bash
set -euo pipefail

task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_test_dir="$(mktemp -d /tmp/ecu-protocol-test-XXXXXX)"

cleanup() {
  rm -rf -- "${task_test_dir}"
}
trap cleanup EXIT

if command -v "${CC:-cc}" >/dev/null 2>&1; then
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/NonSecure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" \
    "${task_project_dir}/NonSecure/App/Src/ecu_protocol.c" \
    "${task_project_dir}/tests/test_ecu_protocol.c" \
    -o "${task_test_dir}/ecu_protocol_test"
  "${task_test_dir}/ecu_protocol_test"
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/Secure/App/Inc" \
    -I"${task_project_dir}/Secure_nsclib" \
    "${task_project_dir}/Secure/App/Src/valve_control.c" \
    "${task_project_dir}/tests/test_valve_control.c" \
    -o "${task_test_dir}/valve_control_test"
  "${task_test_dir}/valve_control_test"
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
    -I"${task_project_dir}/Secure/App/Inc" \
    "${task_project_dir}/Secure/App/Src/security_sha256.c" \
    "${task_project_dir}/tests/test_security_sha256.c" \
    -o "${task_test_dir}/security_sha256_test"
  "${task_test_dir}/security_sha256_test"
else
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
    -I"${task_project_dir}/Secure/App/Inc" -c \
    "${task_project_dir}/Secure/App/Src/security_sha256.c" \
    -o "${task_test_dir}/security_sha256.o"
  "${task_arm_cc}" -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 \
    -I"${task_project_dir}/Secure/App/Inc" -c \
    "${task_project_dir}/tests/test_security_sha256.c" \
    -o "${task_test_dir}/test_security_sha256.o"
  printf 'INFO  Native C compiler unavailable; protocol/control C tests compile passed; CRC vectors run in Python.\n'
fi

python3 -m unittest discover -s "${task_project_dir}/tests" -p 'test_*.py' -v
