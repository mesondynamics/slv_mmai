#!/usr/bin/env python3
"""Frozen previous-ECU A5 protocol helpers for migration and test tooling.

This module deliberately exposes only the safe compatibility subset accepted by
the current ECU. It never converts legacy throttle percentage to valve current
and it never converts legacy angle commands to CAN2 motor speed.
"""

from __future__ import annotations

import struct
from collections.abc import Iterable, Mapping

FRAME_START = 0xA5
FRAME_STOP = 0x5A
FRAME_OVERHEAD = 5

CONTROL_PAYLOAD_FORMAT = "19BbhHBb5B"
CONTROL_DATAGRAM_FORMAT = "<BBHI" + CONTROL_PAYLOAD_FORMAT
STATUS_PAYLOAD_FORMAT = "<IIHh9H20BbBb7B" + "HBBhHBhhHBB"

CONTROL_FIELDS = [
    "pump_enable", "pump_select", "headlamp_front_on",
    "headlamp_rear_on", "led_front_on", "led_rear_on",
    "buzzer_reverse_on", "buzzer_main_on", "indicator1_on",
    "indicator2_on", "indicator3_on", "vib_original_on",
    "vib_strong_on", "vib_weak_on", "vib_front_selected",
    "vib_rear_selected", "engine_start_request", "power_latch_on",
    "speed_mode_high", "engine_speed_level", "steering_target_tdeg",
    "steering_speed_tdeg_per_s", "steering_enable", "throttle_percent",
    "parking_brake_on", "emergency_stop_on", "main_power_relay_on",
    "turn_signal_right_on", "turn_signal_left_on",
]

STATUS_FIELDS = [
    "sequence_id", "timestamp_ms", "steering_raw", "steering_cdeg",
    "steering_mv", "water_mv", "oil_temperature_mv", "oil_level_mv",
    "reserved_mv", "engine_signal_mv", "engine_signal_raw",
    "speed_frequency_centi_hz", "vehicle_speed_centi_kph",
    "speed_signal_present", "engine_running", "pump_enabled", "pump_select",
    "headlamp_front_on", "headlamp_rear_on", "led_front_on", "led_rear_on",
    "buzzer_reverse_on", "buzzer_main_on", "indicator1_on", "indicator2_on",
    "indicator3_on", "vib_original_on", "vib_strong_on", "vib_weak_on",
    "vib_front_selected", "vib_rear_selected", "engine_start_request_on",
    "speed_mode_high", "engine_speed_level", "power_latch_on",
    "throttle_percent", "parking_brake_on", "emergency_stop_on",
    "main_power_relay_on", "turn_signal_right_on", "turn_signal_left_on",
    "control_mode", "active_sender_id", "j1939_engine_rpm",
    "j1939_engine_torque_percent", "j1939_engine_load_percent",
    "j1939_coolant_temp_cdeg", "j1939_oil_pressure_kpa",
    "j1939_accelerator_percent", "j1939_fuel_temp_cdeg",
    "j1939_fuel_pressure_kpa", "j1939_ambient_temp_cdeg",
    "j1939_dtc_count", "j1939_data_valid",
]

CONTROL_DATAGRAM_SIZE = struct.calcsize(CONTROL_DATAGRAM_FORMAT)
STATUS_PAYLOAD_SIZE = struct.calcsize(STATUS_PAYLOAD_FORMAT)

_BOOLEAN_CONTROL_FIELDS = {
    name for name in CONTROL_FIELDS
    if name not in {
        "engine_speed_level", "steering_target_tdeg",
        "steering_speed_tdeg_per_s", "throttle_percent",
    }
}
_UNSUPPORTED_CONTROL_FIELDS = {
    "indicator1_on", "indicator2_on", "indicator3_on", "power_latch_on",
    "main_power_relay_on", "steering_target_tdeg",
    "steering_speed_tdeg_per_s", "steering_enable", "throttle_percent",
}


def xor_parity(data: Iterable[int]) -> int:
    parity = 0
    for value in data:
        parity ^= value
    return parity


def encode_frame(payload: bytes) -> bytes:
    if len(payload) > 0xFFFF:
        raise ValueError("V1 payload exceeds the 16-bit length field")
    return bytes((FRAME_START, len(payload) >> 8, len(payload) & 0xFF)) + \
        payload + bytes((xor_parity(payload), FRAME_STOP))


def decode_frame(frame: bytes, expected_payload_size: int) -> bytes:
    if len(frame) != expected_payload_size + FRAME_OVERHEAD:
        raise ValueError("V1 frame size mismatch")
    if frame[0] != FRAME_START or frame[-1] != FRAME_STOP:
        raise ValueError("V1 frame delimiter mismatch")
    declared_size = (frame[1] << 8) | frame[2]
    if declared_size != expected_payload_size:
        raise ValueError("V1 payload length mismatch")
    payload = frame[3:-2]
    if xor_parity(payload) != frame[-2]:
        raise ValueError("V1 XOR parity mismatch")
    return payload


def decode_control_frame(frame: bytes) -> dict[str, int]:
    payload = decode_frame(frame, CONTROL_DATAGRAM_SIZE)
    values = struct.unpack(CONTROL_DATAGRAM_FORMAT, payload)
    result = {
        "sender_id": values[0], "priority": values[1], "flags": values[2],
        "sequence": values[3],
    }
    result.update(zip(CONTROL_FIELDS, values[4:]))
    return result


def validate_safe_control(control: Mapping[str, int], flags: int = 0) -> None:
    unknown = set(control) - set(CONTROL_FIELDS)
    if unknown:
        raise ValueError(f"unknown V1 control fields: {sorted(unknown)}")
    if flags != 0:
        raise ValueError("V1 flags have no safe compatibility semantics")
    state = {name: int(control.get(name, 0)) for name in CONTROL_FIELDS}
    if any(state[name] not in (0, 1) for name in _BOOLEAN_CONTROL_FIELDS):
        raise ValueError("V1 boolean fields must be exactly 0 or 1")
    if not -3 <= state["engine_speed_level"] <= 3:
        raise ValueError("V1 engine_speed_level must be in -3..3")
    if state["vib_strong_on"] and state["vib_weak_on"]:
        raise ValueError("V1 strong and weak vibration requests conflict")
    requested = [name for name in _UNSUPPORTED_CONTROL_FIELDS if state[name]]
    if requested:
        raise ValueError(
            "unsafe or unsupported V1 request: " + ", ".join(sorted(requested))
        )


def build_safe_control_frame(control: Mapping[str, int], *, sender_id: int,
                             priority: int, sequence: int,
                             flags: int = 0) -> bytes:
    validate_safe_control(control, flags)
    state = {name: int(control.get(name, 0)) for name in CONTROL_FIELDS}
    payload = struct.pack(
        CONTROL_DATAGRAM_FORMAT, sender_id & 0xFF, priority & 0xFF,
        flags & 0xFFFF, sequence & 0xFFFFFFFF,
        *(state[name] for name in CONTROL_FIELDS),
    )
    return encode_frame(payload)


def decode_status_frame(frame: bytes) -> dict[str, int]:
    payload = decode_frame(frame, STATUS_PAYLOAD_SIZE)
    return dict(zip(STATUS_FIELDS, struct.unpack(STATUS_PAYLOAD_FORMAT, payload)))


if CONTROL_DATAGRAM_SIZE != 39 or STATUS_PAYLOAD_SIZE != 77:
    raise RuntimeError("frozen V1 ABI size changed")
