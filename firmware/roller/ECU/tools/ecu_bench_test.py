#!/usr/bin/env python3
"""Exercise the ECU's supported outputs and valve PI over the V2 UDP API."""

from __future__ import annotations

import argparse
import importlib.util
import pathlib
import select
import socket
import struct
import sys
import time
from typing import Callable


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("ecu_debug_ui", SCRIPT_DIR / "ecu_debug_ui.py")
UI = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = UI
SPEC.loader.exec_module(UI)

# Every live normal authority takes over the persistent selectable OEM inputs
# and closes K12. K3 (bit 16) is deliberately absent: it is active only while
# an engine high/low trigger is in progress.
BASE = sum(1 << bit for bit in (1, 3, 4, 8, 18, 22, 26))
RELAY_CASES = [
    ("喇叭 K21", {"buzzer_main_on": 1}, 1 << 2),
    ("倒车蜂鸣 K17", {"buzzer_reverse_on": 1}, 1 << 20),
    ("前主灯 K22", {"headlamp_front_on": 1}, 1 << 29),
    ("后主灯 K23", {"headlamp_rear_on": 1}, 1 << 14),
    ("前 LED K15", {"led_front_on": 1}, 1 << 7),
    ("后 LED K16", {"led_rear_on": 1}, 1 << 15),
    ("水泵 1 K26", {"pump_enable": 1, "pump_select": 0}, 1 << 6),
    ("水泵 2 K27", {"pump_enable": 1, "pump_select": 1}, 1 << 30),
    ("大振 K4", {"vib_strong_on": 1}, 1 << 9),
    ("小振 K6", {"vib_weak_on": 1}, 1 << 17),
    ("前振 K8", {"vib_front_selected": 1}, 1 << 0),
    ("后振 K10", {"vib_rear_selected": 1}, 1 << 19),
    ("驻车 K11", {"parking_brake_on": 1}, 1 << 21),
    ("兔子档 K14（GND）", {"speed_mode_high": 1}, 1 << 25),
    ("左转 K18", {"turn_signal_left_on": 1}, 1 << 23),
    ("右转 K19", {"turn_signal_right_on": 1}, 1 << 28),
]


class BenchFailure(RuntimeError):
    pass


class Bench:
    def __init__(self, ecu_ip: str) -> None:
        self.target = (ecu_ip, UI.CONTROL_PORT)
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx = [
            self.receiver(UI.STATUS_PORT),
            self.receiver(UI.DIAGNOSTIC_PORT),
        ]
        seed = int(time.time() * 1000) & 0xFFFFFFFF
        self.sequences = {1: seed, 2: seed, 3: seed}
        self.status = None
        self.diagnostic = None
        self.steering_status = None

    @staticmethod
    def receiver(port: int) -> socket.socket:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", port))
        sock.setblocking(False)
        return sock

    def close(self) -> None:
        try:
            self.tx.sendto(self.next_frame(UI.neutral_control(), 2,
                                           flags=UI.CONTROL_FLAG_RELEASE),
                           self.target)
            time.sleep(0.05)
        except OSError:
            pass
        self.tx.close()
        for sock in self.rx:
            sock.close()

    def receive(self, timeout: float) -> None:
        readable, _, _ = select.select(self.rx, [], [], timeout)
        for sock in readable:
            packet, source = sock.recvfrom(2048)
            if source[0] != self.target[0] or source[1] != UI.STATUS_PORT:
                continue
            decoded = UI.decode_v2(packet)
            if decoded is None:
                continue
            header, payload = decoded
            port = sock.getsockname()[1]
            if (port == UI.STATUS_PORT and
                    header["message_type"] == UI.MSG_STATUS and
                    len(payload) == struct.calcsize(UI.STATUS_FORMAT)):
                self.status = dict(zip(UI.STATUS_FIELDS, struct.unpack(UI.STATUS_FORMAT, payload)))
            elif (port == UI.STATUS_PORT and
                  header["message_type"] == UI.MSG_STEERING_STATUS and
                  len(payload) == struct.calcsize(UI.STEERING_STATUS_FORMAT)):
                self.steering_status = dict(zip(
                    UI.STEERING_STATUS_FIELDS,
                    struct.unpack(UI.STEERING_STATUS_FORMAT, payload),
                ))
            elif (port == UI.DIAGNOSTIC_PORT and
                  header["message_type"] == UI.MSG_DIAGNOSTIC and
                  len(payload) == struct.calcsize(UI.DIAGNOSTIC_FORMAT)):
                self.diagnostic = dict(zip(
                    UI.DIAGNOSTIC_FIELDS,
                    struct.unpack(UI.DIAGNOSTIC_FORMAT, payload),
                ))

    def next_frame(self, control: dict[str, int], sender: int, *, flags: int = 0,
                   emergency: bool = False) -> bytes:
        self.sequences[sender] = (self.sequences[sender] + 1) & 0xFFFFFFFF
        effective = UI.neutral_control() if emergency else dict(control)
        if emergency:
            effective["emergency_stop_request"] = 1
        payload = UI.build_control_payload(
            UI.sanitize_control(effective), sender, 255 if emergency else sender
        )
        return UI.encode_v2(UI.MSG_CONTROL, flags, self.sequences[sender], payload)

    def drive(self, controls: list[tuple[dict[str, int], int, bool]], duration: float,
              predicate: Callable[[], bool] | None = None, flags: int = 0) -> bool:
        deadline = time.monotonic() + duration
        matched = False
        first = True
        while time.monotonic() < deadline:
            cycle = time.monotonic()
            for control, sender, emergency in controls:
                packet = self.next_frame(control, sender, flags=flags if first else 0,
                                         emergency=emergency)
                self.tx.sendto(packet, self.target)
            first = False
            self.receive(max(0.0, min(0.04, deadline - time.monotonic())))
            if predicate is not None and predicate():
                matched = True
            delay = 0.04 - (time.monotonic() - cycle)
            if delay > 0:
                time.sleep(delay)
        return matched if predicate is not None else True

    def wait(self, duration: float, predicate: Callable[[], bool] | None = None) -> bool:
        deadline = time.monotonic() + duration
        matched = False
        while time.monotonic() < deadline:
            self.receive(min(0.1, deadline - time.monotonic()))
            if predicate is not None and predicate():
                matched = True
        return matched if predicate is not None else True


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BenchFailure(message)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ecu-ip", default="172.16.0.11")
    parser.add_argument("--accept-unloaded-actuation", action="store_true",
                        help="confirm that relay/valve actuation is safe on this bench")
    parser.add_argument(
        "--accept-disconnected-valves", action="store_true",
        help=("valve coils are disconnected: verify target/PWM/reversal logic "
              "but explicitly skip physical current tracking"),
    )
    args = parser.parse_args()
    if not args.accept_unloaded_actuation:
        parser.error("--accept-unloaded-actuation is required")

    bench = Bench(args.ecu_ip)
    neutral = UI.neutral_control()
    passed = 0

    def relay_mask() -> int:
        return int((bench.diagnostic or {}).get("applied_relay_mask", -1)) & 0xFFFFFFFF

    def mode() -> int:
        return int((bench.status or {}).get("control_mode", -1))

    def run_case(name: str, control: dict[str, int], expected_mask: int,
                 duration: float = 0.34) -> None:
        nonlocal passed
        value = dict(neutral)
        value.update(control)
        matched = bench.drive([(value, 2, False)], duration,
                              lambda: relay_mask() == expected_mask and mode() == 2)
        require(matched, f"{name}: expected mask 0x{expected_mask:08X}, got 0x{relay_mask():08X}")
        passed += 1
        print(f"PASS  {name}: 0x{expected_mask:08X}")

    try:
        require(bench.wait(
            3.0,
            lambda: (bench.status is not None and
                     bench.diagnostic is not None and
                     bench.steering_status is not None),
        ), "did not receive ECU status/diagnostic/steering broadcasts")
        require(
            int(bench.steering_status["requested_velocity_tdeg_per_s"]) == 0 and
            int(bench.steering_status["applied_velocity_tdeg_per_s"]) == 0 and
            int(bench.steering_status["speed_command_permille"]) == 0 and
            int(bench.steering_status["command_enable"]) == 0 and
            int(bench.steering_status["motor_enable_confirmed"]) == 0,
            "steering was not passive at bench-test entry",
        )
        print("PASS  Ethernet status/diagnostic/steering broadcasts; steering passive")
        passed += 1

        # Neutral + CLEAR_FAULT. A non-clearable or physical E-stop fault keeps
        # the output unarmed and is reported as a deliberate bench failure.
        require(bench.drive([(neutral, 2, False)], 0.45,
                            lambda: relay_mask() == BASE and mode() == 2,
                            flags=UI.CONTROL_FLAG_CLEAR_FAULT),
                f"could not clear/arm Secure outputs; safety=0x{int(bench.diagnostic['safety_status']):08X}")
        print(f"PASS  Neutral takeover base mask: 0x{BASE:08X}")
        passed += 1

        for name, patch, extra in RELAY_CASES:
            run_case(name, patch, BASE | extra)

        run_case("旧 ABI 未置 bit2 的转向字段保持 no-op",
                 {"indicator1_on": 1, "indicator2_on": 1, "indicator3_on": 1,
                  "power_latch_on": 1, "main_power_relay_on": 1,
                  "steering_enable": 1}, BASE)
        forward = {**neutral, "valve_current_target_ma": 200}
        if args.accept_disconnected_valves:
            require(bench.drive([(forward, 2, False)], 1.25,
                                lambda: int(bench.status["valve_applied_target_ma"]) == 200 and
                                int(bench.status["forward_current_ma"]) < 50 and
                                int(bench.status["forward_duty_permille"]) >= 550 and
                                int(bench.status["reverse_duty_permille"]) == 0),
                    "forward disconnected-valve target/PWM check failed")
            print("PASS  前进阀目标斜坡及 PWM 互斥（线圈未接，闭环电流验收 SKIP）")
        else:
            require(bench.drive([(forward, 2, False)], 1.25,
                                lambda: 120 <= int(bench.status["forward_current_ma"]) <= 300 and
                                int(bench.status["forward_duty_permille"]) > 0 and
                                int(bench.status["reverse_duty_permille"]) == 0),
                    "forward valve did not track 200 mA with exclusive PWM")
            print("PASS  前进阀 200 mA 闭环及 PWM 互斥")
        passed += 1
        reverse = {**neutral, "valve_current_target_ma": -200}
        if args.accept_disconnected_valves:
            require(bench.drive([(reverse, 2, False)], 1.65,
                                lambda: int(bench.status["valve_applied_target_ma"]) == -200 and
                                int(bench.status["reverse_current_ma"]) < 50 and
                                int(bench.status["reverse_duty_permille"]) >= 550 and
                                int(bench.status["forward_duty_permille"]) == 0),
                    "reverse disconnected-valve target/PWM check failed")
            print("PASS  安全换向及后退阀 PWM 互斥（线圈未接，闭环电流验收 SKIP）")
        else:
            require(bench.drive([(reverse, 2, False)], 1.65,
                                lambda: 120 <= int(bench.status["reverse_current_ma"]) <= 300 and
                                int(bench.status["reverse_duty_permille"]) > 0 and
                                int(bench.status["forward_duty_permille"]) == 0),
                    "reverse valve did not complete safe reversal and track 200 mA")
            print("PASS  安全换向及后退阀 200 mA 闭环 / PWM 互斥")
        passed += 1
        bench.drive([(neutral, 2, False)], 0.45)

        require(bench.drive([({**neutral, "engine_start_request": 1}, 2, False)], 0.55,
                            lambda: bool(relay_mask() & (1 << 31))),
                "engine starter K20 was not observed")
        bench.drive([(neutral, 2, False)], 0.2)
        print("PASS  发动机启动 K20 及主动释放")
        passed += 1

        bench.drive([({**neutral, "engine_start_request": 1}, 2, False)], 3.35)
        require(not bool(relay_mask() & (1 << 31)) and
                bool(int(bench.diagnostic["safety_status"]) & (1 << 10)),
                "Secure 3 s engine-starter limit was not enforced")
        bench.drive([(neutral, 2, False)], 0.2)
        print("PASS  发动机启动 Secure 3 s 强制释放")
        passed += 1

        require(bench.drive([({**neutral, "engine_speed_level": 1}, 2, False)], 1.25,
                            lambda: (relay_mask() & ((1 << 16) | (1 << 27))) ==
                                    ((1 << 16) | (1 << 27)) and
                                    not bool(relay_mask() & (1 << 24))),
                "engine-speed-up K24 pulse was not observed")
        require(bench.drive([({**neutral, "engine_speed_level": 1}, 2, False)], 0.35,
                            lambda: not bool(relay_mask() &
                                             ((1 << 16) | (1 << 24) | (1 << 27)))),
                "K3/K24 did not restore after the 1 s high-speed trigger")
        bench.drive([(neutral, 2, False)], 0.2)
        print("PASS  发动机高转速 K3 接管 / K24 BAT 1 s / 恢复原车")
        passed += 1
        require(bench.drive([({**neutral, "engine_speed_level": -1}, 2, False)], 1.25,
                            lambda: (relay_mask() & ((1 << 16) | (1 << 24))) ==
                                    ((1 << 16) | (1 << 24)) and
                                    not bool(relay_mask() & (1 << 27))),
                "engine-speed-down K25 pulse was not observed")
        require(bench.drive([({**neutral, "engine_speed_level": -1}, 2, False)], 0.35,
                            lambda: not bool(relay_mask() &
                                             ((1 << 16) | (1 << 24) | (1 << 27)))),
                "K3/K25 did not restore after the 1 s low-speed trigger")
        bench.drive([(neutral, 2, False)], 0.2)
        print("PASS  发动机低转速 K3 接管 / K25 GND 1 s / 恢复原车")
        passed += 1

        require(bench.drive([({**neutral, "emergency_stop_request": 1}, 2, False)], 0.35,
                            lambda: relay_mask() == 0 and mode() == 4),
                "V2 emergency request did not promote/disarm outputs")
        print("PASS  V2 emergency_stop_request=1 紧急停止")
        passed += 1

        require(bench.drive([(neutral, 2, True)], 0.35,
                            lambda: relay_mask() == 0 and mode() == 4),
                "network emergency did not disarm outputs")
        print("PASS  网络紧急优先级 255")
        passed += 1

        sender1 = {**neutral, "buzzer_main_on": 1}
        sender3 = {**neutral, "led_front_on": 1}
        require(bench.drive([(sender1, 1, False), (sender3, 3, False)], 0.4,
                            lambda: mode() == 3 and relay_mask() == (BASE | (1 << 7))),
                "sender 3 did not preempt sender 1")
        require(bench.drive([(sender1, 1, False)], 0.4,
                            lambda: mode() == 1 and relay_mask() == (BASE | (1 << 2))),
                "authority did not fall back from sender 3 to sender 1")
        print("PASS  控制权 3 > 1 及超时回退")
        passed += 1

        bench.drive([(neutral, 1, False)], 0.2)
        require(bench.wait(0.55, lambda: mode() == 0 and relay_mask() == 0),
                "control timeout did not restore IDLE/all-off")
        print("PASS  250 ms 控制丢失恢复 IDLE / relay mask 0")
        passed += 1
    except BenchFailure as error:
        print(f"FAIL  {error}", file=sys.stderr)
        return 1
    finally:
        # Stop transmitting; the ECU's independent timeout is the final safe
        # action even if an assertion or host-side exception occurs.
        bench.close()

    print(f"\nBench protocol acceptance passed: {passed} checks.")
    if args.accept_disconnected_valves:
        print("WARNING: valve current tracking was skipped because both coils were declared disconnected.")
    print("Note: diagnostic masks verify ECU logic; inspect relay contacts electrically before loading.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
