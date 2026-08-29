#!/usr/bin/env python3
"""Zero-dependency V2 bench UI and UDP bridge for the roller ECU."""

from __future__ import annotations

import argparse
import importlib.util
import ipaddress
import json
import select
import signal
import socket
import struct
import threading
import time
import tempfile
from collections import deque
from dataclasses import dataclass, field
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


STATUS_PORT = 50001
CONTROL_PORT = 50002
DIAGNOSTIC_PORT = 50003
TELEMETRY_PORT = 50004
TUNING_PORT = 50005
OTA_MAX_PACKAGE_SIZE = 2 * 1024 * 1024
SEND_PERIOD_S = 0.05
SUBSCRIBE_PERIOD_S = 1.0
BROWSER_DEADMAN_S = 1.0
TUNING_CLIENT_LEASE_S = 1.5

V2_MAGIC = 0x32554345
V2_VERSION = 2
V2_HEADER_FORMAT = "<IBBHHHIII"
V2_HEADER_SIZE = struct.calcsize(V2_HEADER_FORMAT)
V2_CRC_OFFSET = 20

MSG_CONTROL = 0x01
MSG_STATUS = 0x02
MSG_DIAGNOSTIC = 0x03
MSG_TELEMETRY = 0x04
MSG_SECURITY = 0x05
MSG_CONFIG_GET = 0x10
MSG_CONFIG_APPLY = 0x11
MSG_CONFIG_SAVE = 0x12
MSG_CONFIG_RELOAD = 0x13
MSG_CONFIG_REPLY = 0x14
MSG_TELEMETRY_SUBSCRIBE = 0x15
MSG_OPERATION_ACK = 0x16
MSG_TELEMETRY_UNSUBSCRIBE = 0x17

CONTROL_FLAG_CLEAR_FAULT = 1 << 0
CONTROL_FLAG_RELEASE = 1 << 1

CONTROL_FORMAT = "<BBH19BbhHBh5B"
STATUS_FORMAT = "<IIHh9H20BbB7BHBBhHBhHhBBhhHHHHBBHII"
DIAGNOSTIC_FORMAT = "<12I12Hb5BII"
SECURITY_FORMAT = "<IIiI3I9s4s4B3xiI"
TELEMETRY_BATCH_FORMAT = "<IIHH"
TELEMETRY_SAMPLE_FORMAT = "<IhhHHHHHH"
CHANNEL_CONFIG_FORMAT = "<6HihH"
CONFIG_HEADER_FORMAT = "<II"

CONTROL_FIELDS = [
    "pump_enable", "pump_select", "headlamp_front_on", "headlamp_rear_on",
    "led_front_on", "led_rear_on", "buzzer_reverse_on", "buzzer_main_on",
    "indicator1_on", "indicator2_on", "indicator3_on", "vib_original_on",
    "vib_strong_on", "vib_weak_on", "vib_front_selected",
    "vib_rear_selected", "engine_start_request", "power_latch_on",
    "speed_mode_high", "engine_speed_level", "steering_target_tdeg",
    "steering_speed_tdeg_per_s", "steering_enable",
    "valve_current_target_ma", "parking_brake_on",
    "emergency_stop_request", "main_power_relay_on",
    "turn_signal_right_on", "turn_signal_left_on",
]

BOOLEAN_FIELDS = {
    name for name in CONTROL_FIELDS
    if name.endswith("_on") or name.endswith("_enable")
    or name.startswith("pump_") or name.endswith("_selected")
    or name in {"engine_start_request", "speed_mode_high",
                "emergency_stop_request"}
}

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
    "parking_brake_on", "emergency_stop_on", "main_power_relay_on",
    "turn_signal_right_on", "turn_signal_left_on", "control_mode",
    "active_sender_id", "j1939_engine_rpm", "j1939_engine_torque_percent",
    "j1939_engine_load_percent", "j1939_coolant_temp_cdeg",
    "j1939_oil_pressure_kpa", "j1939_accelerator_percent",
    "j1939_fuel_temp_cdeg", "j1939_fuel_pressure_kpa",
    "j1939_ambient_temp_cdeg", "j1939_dtc_count", "j1939_data_valid",
    "valve_requested_target_ma", "valve_applied_target_ma",
    "forward_current_ma", "reverse_current_ma", "forward_duty_permille",
    "reverse_duty_permille", "valve_state", "valve_config_dirty",
    "reserved", "valve_active_revision", "valve_persisted_generation",
]

DIAGNOSTIC_FIELDS = [
    "capability_flags", "safety_status", "requested_relay_mask",
    "applied_relay_mask", "secure_uptime_ms", "command_age_ms",
    "valid_control_frames", "invalid_control_frames",
    "rejected_control_frames", "authority_switches",
    "legacy_v1_frames_rejected", "telemetry_frames_sent",
    *[f"slow_adc_{index}" for index in range(9)],
    *[f"current_adc_{index}" for index in range(2)],
    "engine_start_remaining_ms", "engine_speed_remaining",
    "engine_speed_state", "link_up", "active_sender_id", "control_mode",
    "software_i2c_bus_ok", "valve_fault_flags",
    "telemetry_dropped_samples",
]

CHANNEL_CONFIG_FIELDS = [
    "kp_permille_per_amp", "ki_permille_per_amp_second", "filter_cutoff_hz",
    "rise_slew_ma_per_s", "fall_slew_ma_per_s", "max_duty_permille",
    "current_gain_ppm", "current_offset_ma", "reserved",
]


def neutral_control() -> dict[str, int]:
    return {name: 0 for name in CONTROL_FIELDS}


def default_valve_config() -> dict[str, Any]:
    channel = {
        "kp_permille_per_amp": 400,
        "ki_permille_per_amp_second": 4000,
        "filter_cutoff_hz": 500,
        "rise_slew_ma_per_s": 1000,
        "fall_slew_ma_per_s": 2000,
        "max_duty_permille": 600,
        "current_gain_ppm": 1_000_000,
        "current_offset_ma": 0,
        "reserved": 0,
    }
    return {"version": 1, "size": 48,
            "forward": dict(channel), "reverse": dict(channel)}


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def encode_v2(message_type: int, flags: int, sequence: int,
              payload: bytes = b"", timestamp_ms: int | None = None) -> bytes:
    timestamp = int(time.monotonic() * 1000) if timestamp_ms is None else timestamp_ms
    header = struct.pack(
        V2_HEADER_FORMAT, V2_MAGIC, V2_VERSION, message_type, V2_HEADER_SIZE,
        len(payload), flags & 0xFFFF, sequence & 0xFFFFFFFF,
        timestamp & 0xFFFFFFFF, 0,
    )
    frame = bytearray(header + payload)
    struct.pack_into("<I", frame, V2_CRC_OFFSET, crc32c(frame))
    return bytes(frame)


def decode_v2(packet: bytes) -> tuple[dict[str, int], bytes] | None:
    if len(packet) < V2_HEADER_SIZE:
        return None
    values = struct.unpack_from(V2_HEADER_FORMAT, packet)
    magic, version, message_type, header_size, payload_size, flags, sequence, \
        timestamp_ms, received_crc = values
    if magic != V2_MAGIC or version != V2_VERSION or header_size != V2_HEADER_SIZE:
        return None
    if header_size + payload_size != len(packet):
        return None
    checked = bytearray(packet)
    struct.pack_into("<I", checked, V2_CRC_OFFSET, 0)
    if crc32c(checked) != received_crc:
        return None
    return ({"message_type": message_type, "flags": flags,
             "sequence": sequence, "timestamp_ms": timestamp_ms},
            packet[header_size:])


def sanitize_control(state: dict[str, int]) -> dict[str, int]:
    result = neutral_control()
    for name in CONTROL_FIELDS:
        value = int(state.get(name, 0))
        result[name] = 1 if name in BOOLEAN_FIELDS and value else value
    result["engine_speed_level"] = max(-3, min(3, result["engine_speed_level"]))
    result["steering_target_tdeg"] = max(
        -300, min(300, result["steering_target_tdeg"])
    )
    result["steering_speed_tdeg_per_s"] = max(
        0, min(6000, result["steering_speed_tdeg_per_s"])
    )
    result["valve_current_target_ma"] = max(
        -2000, min(2000, result["valve_current_target_ma"])
    )
    if -50 < result["valve_current_target_ma"] < 50:
        result["valve_current_target_ma"] = 0
    if result["vib_strong_on"] and result["vib_weak_on"]:
        result["vib_weak_on"] = 0
    return result


def build_control_payload(state: dict[str, int], sender_id: int,
                          priority: int) -> bytes:
    values = [state[name] for name in CONTROL_FIELDS]
    return struct.pack(CONTROL_FORMAT, sender_id, priority, 0, *values)


def pack_valve_config(config: dict[str, Any]) -> bytes:
    version = int(config.get("version", 1))
    size = int(config.get("size", 48))
    packed = bytearray(struct.pack(CONFIG_HEADER_FORMAT, version, size))
    for direction in ("forward", "reverse"):
        channel = config[direction]
        packed.extend(struct.pack(
            CHANNEL_CONFIG_FORMAT,
            *(int(channel[name]) for name in CHANNEL_CONFIG_FIELDS),
        ))
    return bytes(packed)


def unpack_valve_config(data: bytes, offset: int = 0) -> tuple[dict[str, Any], int]:
    version, size = struct.unpack_from(CONFIG_HEADER_FORMAT, data, offset)
    offset += struct.calcsize(CONFIG_HEADER_FORMAT)
    result: dict[str, Any] = {"version": version, "size": size}
    for direction in ("forward", "reverse"):
        values = struct.unpack_from(CHANNEL_CONFIG_FORMAT, data, offset)
        result[direction] = dict(zip(CHANNEL_CONFIG_FIELDS, values))
        offset += struct.calcsize(CHANNEL_CONFIG_FORMAT)
    return result, offset


def unpack_config_reply(payload: bytes) -> dict[str, Any]:
    if len(payload) != 72:
        raise ValueError("invalid config reply length")
    result, request_sequence = struct.unpack_from("<iI", payload)
    active, persisted, persisted_crc = struct.unpack_from("<III", payload, 8)
    persisted_valid, dirty, using_defaults, _ = struct.unpack_from("<4B", payload, 20)
    config, _ = unpack_valve_config(payload, 24)
    return {
        "result": result, "request_sequence": request_sequence,
        "active_revision": active, "persisted_generation": persisted,
        "persisted_crc32c": persisted_crc,
        "persisted_valid": persisted_valid, "dirty": dirty,
        "using_defaults": using_defaults, "config": config,
    }


def load_ota_module() -> Any:
    module_path = Path(__file__).with_name("ethernet_ota.py")
    spec = importlib.util.spec_from_file_location("ecu_ethernet_ota", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load Ethernet OTA transport")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@dataclass
class BenchState:
    ecu_ip: str = "172.16.0.11"
    sender_id: int = 1
    enabled: bool = False
    emergency: bool = False
    control: dict[str, int] = field(default_factory=neutral_control)
    status: dict[str, Any] | None = None
    diagnostic: dict[str, Any] | None = None
    security: dict[str, Any] | None = None
    valve_config: dict[str, Any] | None = None
    latest_telemetry: dict[str, Any] | None = None
    status_received_at: float = 0.0
    diagnostic_received_at: float = 0.0
    telemetry_received_at: float = 0.0
    browser_heartbeat_at: float = 0.0
    control_sequence: int = 0
    tuning_sequence: int = 0
    pending_flags: int = 0
    start_release_at: float = 0.0
    engine_reset_at: float = 0.0
    tx_error: str = ""
    tuning_error: str = ""
    ota: dict[str, Any] = field(default_factory=lambda: {
        "active": False, "stage": "idle", "error": "",
        "transferred": 0, "total": 0, "metadata": None,
    })


class BenchBridge:
    def __init__(self, ecu_ip: str) -> None:
        self.state = BenchState(ecu_ip=str(ipaddress.IPv4Address(ecu_ip)))
        self.lock = threading.RLock()
        self.reply_condition = threading.Condition(self.lock)
        self.telemetry_condition = threading.Condition(self.lock)
        self.stop_event = threading.Event()
        self.tx_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.status_socket = self._receiver_socket(STATUS_PORT)
        self.diagnostic_socket = self._receiver_socket(DIAGNOSTIC_PORT)
        self.telemetry_socket = self._receiver_socket(TELEMETRY_PORT)
        self.tuning_socket = self._receiver_socket(TUNING_PORT)
        self.rx_sockets = [self.status_socket, self.diagnostic_socket,
                           self.telemetry_socket, self.tuning_socket]
        self.pending_replies: dict[int, dict[str, Any]] = {}
        self.awaited_replies: set[int] = set()
        self.telemetry_history: deque[dict[str, Any]] = deque(maxlen=10_000)
        self.telemetry_generation = 0
        self.tuning_clients: dict[str, float] = {}
        self.telemetry_subscribed = False
        self.ota_thread: threading.Thread | None = None
        self.threads = [
            threading.Thread(target=self._sender_loop, name="ecu-control", daemon=True),
            threading.Thread(target=self._receiver_loop, name="ecu-rx", daemon=True),
        ]

    @staticmethod
    def _receiver_socket(port: int) -> socket.socket:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", port))
        sock.setblocking(False)
        return sock

    def start(self) -> None:
        for thread in self.threads:
            thread.start()

    def close(self) -> None:
        try:
            self._send_unsubscribe()
        except OSError:
            pass
        try:
            self.release_control()
        except OSError:
            pass
        self.stop_event.set()
        with self.telemetry_condition:
            self.telemetry_condition.notify_all()
        for thread in self.threads:
            thread.join(timeout=1.0)
        self.tx_socket.close()
        for sock in self.rx_sockets:
            sock.close()

    def _next_control_sequence(self) -> int:
        self.state.control_sequence = (self.state.control_sequence + 1) & 0xFFFFFFFF
        return self.state.control_sequence

    def _next_tuning_sequence(self) -> int:
        self.state.tuning_sequence = (self.state.tuning_sequence + 1) & 0xFFFFFFFF
        return self.state.tuning_sequence

    def _control_packet(self, control: dict[str, int], flags: int,
                        priority: int | None = None) -> bytes:
        sequence = self._next_control_sequence()
        effective_priority = self.state.sender_id if priority is None else priority
        payload = build_control_payload(
            sanitize_control(control), self.state.sender_id, effective_priority
        )
        return encode_v2(MSG_CONTROL, flags, sequence, payload)

    def _sender_loop(self) -> None:
        deadline = time.monotonic()
        last_subscribe = 0.0
        while not self.stop_event.is_set():
            now = time.monotonic()
            with self.lock:
                if self.state.start_release_at and now >= self.state.start_release_at:
                    self.state.control["engine_start_request"] = 0
                    self.state.start_release_at = 0.0
                if self.state.engine_reset_at and now >= self.state.engine_reset_at:
                    self.state.control["engine_speed_level"] = 0
                    self.state.engine_reset_at = 0.0
                if self.state.enabled and (
                    not self.state.browser_heartbeat_at
                    or now - self.state.browser_heartbeat_at > BROWSER_DEADMAN_S
                ):
                    self.state.control = neutral_control()
                    self.state.enabled = False
                    self.state.pending_flags |= CONTROL_FLAG_RELEASE

                should_send = (self.state.enabled or self.state.emergency
                               or self.state.pending_flags != 0)
                if should_send:
                    flags = self.state.pending_flags
                    self.state.pending_flags = 0
                    control = neutral_control() if (
                        self.state.emergency or flags & CONTROL_FLAG_RELEASE
                    ) else dict(self.state.control)
                    if self.state.emergency:
                        control["emergency_stop_request"] = 1
                    packet = self._control_packet(
                        control, flags, 255 if self.state.emergency else None
                    )
                    target = (self.state.ecu_ip, CONTROL_PORT)
                else:
                    packet = b""
                    target = ("0.0.0.0", 0)
                self.tuning_clients = {
                    client_id: expires_at
                    for client_id, expires_at in self.tuning_clients.items()
                    if expires_at > now
                }
                tuning_active = bool(self.tuning_clients)
                telemetry_subscribed = self.telemetry_subscribed
            if packet:
                try:
                    self.tx_socket.sendto(packet, target)
                    with self.lock:
                        self.state.tx_error = ""
                except OSError as error:
                    with self.lock:
                        self.state.tx_error = str(error)

            if tuning_active and now - last_subscribe >= SUBSCRIBE_PERIOD_S:
                last_subscribe = now
                self._send_subscription()
            elif not tuning_active and telemetry_subscribed:
                self._send_unsubscribe()
            deadline += SEND_PERIOD_S
            wait = deadline - time.monotonic()
            if wait < 0:
                deadline = time.monotonic()
                wait = 0
            self.stop_event.wait(wait)

    def _send_subscription(self) -> None:
        with self.lock:
            sequence = self._next_tuning_sequence()
            ecu_ip = self.state.ecu_ip
        payload = struct.pack("<HHI", TELEMETRY_PORT, 1000, 2000)
        try:
            self.tuning_socket.sendto(
                encode_v2(MSG_TELEMETRY_SUBSCRIBE, 0, sequence, payload),
                (ecu_ip, TUNING_PORT),
            )
            with self.lock:
                self.telemetry_subscribed = True
                self.state.tuning_error = ""
        except OSError as error:
            with self.lock:
                self.state.tuning_error = str(error)

    def _send_unsubscribe(self) -> None:
        with self.lock:
            if not self.telemetry_subscribed:
                return
            sequence = self._next_tuning_sequence()
            ecu_ip = self.state.ecu_ip
        try:
            self.tuning_socket.sendto(
                encode_v2(MSG_TELEMETRY_UNSUBSCRIBE, 0, sequence, b""),
                (ecu_ip, TUNING_PORT),
            )
        finally:
            with self.lock:
                self.telemetry_subscribed = False

    def _receiver_loop(self) -> None:
        while not self.stop_event.is_set():
            readable, _, _ = select.select(self.rx_sockets, [], [], 0.25)
            for sock in readable:
                try:
                    packet, source = sock.recvfrom(2048)
                except OSError:
                    continue
                with self.lock:
                    expected_source = self.state.ecu_ip
                if source[0] != expected_source:
                    continue
                decoded = decode_v2(packet)
                if decoded is None:
                    continue
                header, payload = decoded
                received_at = time.monotonic()
                message_type = header["message_type"]
                try:
                    if message_type == MSG_STATUS and len(payload) == struct.calcsize(STATUS_FORMAT):
                        values = struct.unpack(STATUS_FORMAT, payload)
                        with self.lock:
                            self.state.status = dict(zip(STATUS_FIELDS, values))
                            self.state.status_received_at = received_at
                    elif message_type == MSG_DIAGNOSTIC and len(payload) == struct.calcsize(DIAGNOSTIC_FORMAT):
                        values = struct.unpack(DIAGNOSTIC_FORMAT, payload)
                        with self.lock:
                            self.state.diagnostic = dict(zip(DIAGNOSTIC_FIELDS, values))
                            self.state.diagnostic_received_at = received_at
                    elif message_type == MSG_SECURITY and len(payload) == struct.calcsize(SECURITY_FORMAT):
                        values = struct.unpack(SECURITY_FORMAT, payload)
                        security = {
                            "api_version": values[0], "flags": values[1],
                            "atecc_result": values[2],
                            "config_crc32c": values[3],
                            "mcu_uid": "".join(f"{value:08x}" for value in values[4:7]),
                            "serial": values[7].hex(),
                            "revision": values[8].hex(),
                            "i2c_address": values[9],
                            "config_locked": values[10],
                            "data_locked": values[11],
                            "device_status": values[12],
                            "auth_result": values[13],
                            "pairing_generation": values[14],
                        }
                        with self.lock:
                            self.state.security = security
                    elif message_type == MSG_TELEMETRY:
                        self._receive_telemetry(payload, received_at)
                    elif message_type == MSG_CONFIG_REPLY:
                        reply = unpack_config_reply(payload)
                        self._receive_reply(reply["request_sequence"], reply)
                    elif message_type == MSG_OPERATION_ACK and len(payload) == 8:
                        result, request_sequence = struct.unpack("<iI", payload)
                        self._receive_reply(request_sequence, {
                            "result": result, "request_sequence": request_sequence,
                        })
                except (ValueError, struct.error):
                    continue

    def _receive_reply(self, sequence: int, reply: dict[str, Any]) -> None:
        with self.reply_condition:
            if "config" in reply:
                self.state.valve_config = reply
            if sequence in self.awaited_replies:
                self.pending_replies[sequence] = reply
            self.reply_condition.notify_all()

    def _receive_telemetry(self, payload: bytes, received_at: float) -> None:
        header_size = struct.calcsize(TELEMETRY_BATCH_FORMAT)
        sample_size = struct.calcsize(TELEMETRY_SAMPLE_FORMAT)
        if len(payload) < header_size:
            return
        first_sequence, dropped, count, period_us = struct.unpack_from(
            TELEMETRY_BATCH_FORMAT, payload
        )
        if count > 16 or len(payload) != header_size + count * sample_size:
            return
        samples: list[dict[str, Any]] = []
        offset = header_size
        for index in range(count):
            values = struct.unpack_from(TELEMETRY_SAMPLE_FORMAT, payload, offset)
            offset += sample_size
            timestamp_us, requested, applied, forward_current, reverse_current, \
                forward_duty, reverse_duty, state, faults = values
            sample = {
                "sequence": (first_sequence + index) & 0xFFFFFFFF,
                "timestamp_us": timestamp_us,
                "requested_target_ma": requested,
                "applied_target_ma": applied,
                "forward_current_ma": forward_current,
                "reverse_current_ma": reverse_current,
                "signed_feedback_ma": forward_current if state in (1, 4) else -reverse_current,
                "forward_duty_permille": forward_duty,
                "reverse_duty_permille": reverse_duty,
                "state": state, "fault_flags": faults,
                "dropped_samples": dropped, "sample_period_us": period_us,
            }
            samples.append(sample)
        with self.telemetry_condition:
            for sample in samples:
                self.telemetry_generation += 1
                sample["ui_sequence"] = self.telemetry_generation
                self.telemetry_history.append(sample)
            if samples:
                self.state.latest_telemetry = samples[-1]
                self.state.telemetry_received_at = received_at
            self.telemetry_condition.notify_all()

    def wait_telemetry(self, after: int, timeout: float = 15.0) -> tuple[int, list[dict[str, Any]]]:
        with self.telemetry_condition:
            self.telemetry_condition.wait_for(
                lambda: self.telemetry_generation > after or self.stop_event.is_set(),
                timeout=timeout,
            )
            samples = [sample for sample in self.telemetry_history
                       if sample["ui_sequence"] > after]
            if len(samples) > 1000:
                samples = samples[-1000:]
            return self.telemetry_generation, samples

    def snapshot(self) -> dict[str, Any]:
        now = time.monotonic()
        with self.lock:
            tuning_clients = sum(
                1 for expires_at in self.tuning_clients.values()
                if expires_at > now
            )
            return {
                "ecu_ip": self.state.ecu_ip, "sender_id": self.state.sender_id,
                "enabled": self.state.enabled, "emergency": self.state.emergency,
                "control": dict(self.state.control), "status": self.state.status,
                "diagnostic": self.state.diagnostic,
                "security": self.state.security,
                "valve_config": self.state.valve_config,
                "latest_telemetry": self.state.latest_telemetry,
                "status_age_ms": None if not self.state.status_received_at else
                    int((now - self.state.status_received_at) * 1000),
                "diagnostic_age_ms": None if not self.state.diagnostic_received_at else
                    int((now - self.state.diagnostic_received_at) * 1000),
                "telemetry_age_ms": None if not self.state.telemetry_received_at else
                    int((now - self.state.telemetry_received_at) * 1000),
                "tx_error": self.state.tx_error,
                "tuning_error": self.state.tuning_error,
                "tuning_active": tuning_clients > 0,
                "tuning_clients": tuning_clients,
                "ota": dict(self.state.ota),
            }

    def start_ota(self, package_data: bytes) -> dict[str, Any]:
        if not package_data or len(package_data) > OTA_MAX_PACKAGE_SIZE:
            raise ValueError("OTA package must be 1 byte to 2 MiB")
        ota = load_ota_module()
        temporary = tempfile.NamedTemporaryFile(
            prefix="roller-ecu-", suffix=".recu", delete=False
        )
        package_path = Path(temporary.name)
        try:
            temporary.write(package_data)
            temporary.flush()
            temporary.close()
            _, _, _, metadata = ota.load_package(
                package_path, ota.DEFAULT_PUBLIC_KEY
            )
        except Exception:
            temporary.close()
            package_path.unlink(missing_ok=True)
            raise
        with self.lock:
            if self.state.ota["active"]:
                package_path.unlink(missing_ok=True)
                raise ValueError("an OTA transfer is already active")
            self.state.control = neutral_control()
            self.state.enabled = False
            self.state.emergency = False
            self.state.ota = {
                "active": True, "stage": "queued", "error": "",
                "transferred": 0,
                "total": int(metadata["secure_size"]) +
                         int(metadata["nonsecure_size"]),
                "metadata": metadata,
            }
            ecu_ip = self.state.ecu_ip

        def update(values: dict[str, Any]) -> None:
            with self.lock:
                self.state.ota.update(values)

        def worker() -> None:
            client = ota.OtaClient(ota.HOST_IP, ecu_ip)
            try:
                self.release_control()
                ota.transfer(client, package_path, ota.DEFAULT_PUBLIC_KEY, update)
                with self.lock:
                    self.state.ota["active"] = False
                    self.state.ota["stage"] = "complete"
            except Exception as error:  # surfaced verbatim to local-only UI
                with self.lock:
                    self.state.ota["active"] = False
                    self.state.ota["stage"] = "error"
                    self.state.ota["error"] = str(error)
            finally:
                client.close()
                package_path.unlink(missing_ok=True)

        self.ota_thread = threading.Thread(
            target=worker, name="ecu-ota", daemon=True
        )
        self.ota_thread.start()
        with self.lock:
            return dict(self.state.ota)

    def set_tuning_session(self, client_id: Any, active: Any) -> None:
        if not isinstance(client_id, str) or not client_id or len(client_id) > 128:
            raise ValueError("valid client_id required")
        if not all(character.isalnum() or character in "-_.:" for character in client_id):
            raise ValueError("invalid client_id")
        if not isinstance(active, bool):
            raise ValueError("active must be boolean")
        with self.lock:
            if active:
                self.tuning_clients[client_id] = (
                    time.monotonic() + TUNING_CLIENT_LEASE_S
                )
            else:
                self.tuning_clients.pop(client_id, None)

    def heartbeat(self) -> None:
        with self.lock:
            self.state.browser_heartbeat_at = time.monotonic()

    def configure(self, request: dict[str, Any]) -> None:
        with self.lock:
            if "ecu_ip" in request:
                self.state.ecu_ip = str(ipaddress.IPv4Address(request["ecu_ip"]))
            if "sender_id" in request:
                sender_id = int(request["sender_id"])
                if sender_id not in (1, 2, 3):
                    raise ValueError("sender_id must be 1, 2, or 3")
                self.state.sender_id = sender_id
            if "enabled" in request:
                self.state.enabled = bool(request["enabled"])
                self.state.browser_heartbeat_at = time.monotonic()

    def patch_control(self, patch: dict[str, Any]) -> None:
        unknown = set(patch) - set(CONTROL_FIELDS)
        if unknown:
            raise ValueError(f"unknown control fields: {sorted(unknown)}")
        with self.lock:
            updated = dict(self.state.control)
            updated.update({key: int(value) for key, value in patch.items()})
            self.state.control = sanitize_control(updated)
            self.state.browser_heartbeat_at = time.monotonic()
            now = time.monotonic()
            if "engine_start_request" in patch:
                self.state.start_release_at = now + 3.0 \
                    if self.state.control["engine_start_request"] else 0.0
            if "engine_speed_level" in patch:
                count = abs(self.state.control["engine_speed_level"])
                self.state.engine_reset_at = (
                    now + 1.35 + max(0, count - 1) * 3.10 if count else 0.0
                )

    def neutral(self) -> None:
        with self.lock:
            self.state.control = neutral_control()
            self.state.start_release_at = 0.0
            self.state.engine_reset_at = 0.0
            self.state.emergency = False

    def release_control(self) -> None:
        with self.lock:
            self.state.control = neutral_control()
            self.state.enabled = False
            self.state.emergency = False
            packet = self._control_packet(self.state.control, CONTROL_FLAG_RELEASE)
            target = (self.state.ecu_ip, CONTROL_PORT)
        self.tx_socket.sendto(packet, target)

    def set_emergency(self, active: bool) -> None:
        with self.lock:
            self.state.emergency = active
            self.state.browser_heartbeat_at = time.monotonic()
            if active:
                self.state.control = neutral_control()

    def clear_fault(self) -> None:
        with self.lock:
            self.state.control = neutral_control()
            self.state.emergency = False
            self.state.enabled = True
            self.state.browser_heartbeat_at = time.monotonic()
            self.state.pending_flags |= CONTROL_FLAG_CLEAR_FAULT

    def config_request(self, message_type: int,
                       payload: bytes = b"") -> dict[str, Any]:
        with self.reply_condition:
            sequence = self._next_tuning_sequence()
            self.awaited_replies.add(sequence)
            ecu_ip = self.state.ecu_ip
        packet = encode_v2(message_type, 0, sequence, payload)
        try:
            self.tuning_socket.sendto(packet, (ecu_ip, TUNING_PORT))
        except OSError:
            with self.reply_condition:
                self.awaited_replies.discard(sequence)
            raise
        with self.reply_condition:
            ready = self.reply_condition.wait_for(
                lambda: sequence in self.pending_replies, timeout=1.5
            )
            self.awaited_replies.discard(sequence)
            if not ready:
                raise TimeoutError("ECU tuning response timed out")
            reply = self.pending_replies.pop(sequence)
            if "config" in reply:
                self.state.valve_config = reply
            self.state.tuning_error = "" if reply["result"] == 0 else \
                f"ECU result {reply['result']}"
            return reply


class UiHandler(BaseHTTPRequestHandler):
    bridge: BenchBridge
    index_path: Path
    protocol_version = "HTTP/1.1"

    def log_message(self, format_string: str, *args: Any) -> None:
        del format_string, args

    def _json(self, value: Any, status: HTTPStatus = HTTPStatus.OK) -> None:
        body = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _request_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        if length < 0 or length > 64 * 1024:
            raise ValueError("request too large")
        data = self.rfile.read(length) if length else b"{}"
        value = json.loads(data)
        if not isinstance(value, dict):
            raise ValueError("JSON object required")
        return value

    def _request_binary(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > OTA_MAX_PACKAGE_SIZE:
            raise ValueError("OTA package must be 1 byte to 2 MiB")
        return self.rfile.read(length)

    def _sse(self) -> None:
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        after = int(self.headers.get("Last-Event-ID", "0") or 0)
        try:
            while not self.bridge.stop_event.is_set():
                generation, samples = self.bridge.wait_telemetry(after)
                data = json.dumps(samples, separators=(",", ":"),
                                  ensure_ascii=False)
                self.wfile.write(
                    f"id: {generation}\nevent: samples\ndata: {data}\n\n".encode()
                )
                self.wfile.flush()
                after = generation
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            return

    def do_GET(self) -> None:  # noqa: N802
        try:
            if self.path in ("/", "/index.html"):
                body = self.index_path.read_bytes()
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)
            elif self.path == "/api/state":
                self._json(self.bridge.snapshot())
            elif self.path == "/api/valve/config":
                self._json(self.bridge.config_request(MSG_CONFIG_GET))
            elif self.path == "/api/valve/stream":
                self._sse()
            else:
                self._json({"error": "not found"}, HTTPStatus.NOT_FOUND)
        except (OSError, TimeoutError) as error:
            self._json({"error": str(error)}, HTTPStatus.GATEWAY_TIMEOUT)

    def do_POST(self) -> None:  # noqa: N802
        try:
            if self.path == "/api/ota/upload":
                if self.headers.get("Content-Type", "") != \
                        "application/octet-stream":
                    raise ValueError("application/octet-stream required")
                self._json(self.bridge.start_ota(self._request_binary()))
                return
            request = self._request_json()
            response: Any = None
            if self.path == "/api/config":
                self.bridge.configure(request)
            elif self.path == "/api/control":
                self.bridge.patch_control(request)
            elif self.path == "/api/neutral":
                self.bridge.neutral()
            elif self.path == "/api/release":
                self.bridge.release_control()
            elif self.path == "/api/emergency":
                self.bridge.set_emergency(bool(request.get("active", True)))
            elif self.path == "/api/clear-fault":
                self.bridge.clear_fault()
            elif self.path == "/api/heartbeat":
                self.bridge.heartbeat()
            elif self.path == "/api/valve/tuning-session":
                self.bridge.set_tuning_session(
                    request.get("client_id"), request.get("active")
                )
            elif self.path == "/api/valve/apply":
                response = self.bridge.config_request(
                    MSG_CONFIG_APPLY, pack_valve_config(request)
                )
            elif self.path == "/api/valve/save":
                self.bridge.release_control()
                time.sleep(0.1)
                response = self.bridge.config_request(MSG_CONFIG_SAVE)
            elif self.path == "/api/valve/reload":
                response = self.bridge.config_request(MSG_CONFIG_RELOAD)
            else:
                self._json({"error": "not found"}, HTTPStatus.NOT_FOUND)
                return
            self._json(self.bridge.snapshot() if response is None else response)
        except (ValueError, KeyError, struct.error, json.JSONDecodeError) as error:
            self._json({"error": str(error)}, HTTPStatus.BAD_REQUEST)
        except (OSError, TimeoutError) as error:
            self._json({"error": str(error)}, HTTPStatus.GATEWAY_TIMEOUT)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8088)
    parser.add_argument("--ecu-ip", default="172.16.0.11")
    args = parser.parse_args()

    bridge = BenchBridge(args.ecu_ip)
    UiHandler.bridge = bridge
    UiHandler.index_path = Path(__file__).with_name("ecu_debug_ui") / "index.html"
    server = ThreadingHTTPServer((args.host, args.port), UiHandler)

    def request_stop(signum: int, frame: Any) -> None:
        del signum, frame
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    bridge.start()
    print(f"ECU bench UI: http://{args.host}:{args.port}")
    try:
        server.serve_forever(poll_interval=0.25)
    finally:
        server.server_close()
        bridge.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
