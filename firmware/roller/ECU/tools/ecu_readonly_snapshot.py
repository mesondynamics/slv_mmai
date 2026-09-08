#!/usr/bin/env python3
"""Fresh UDP status/config/OTA GET capture; never sends actuator or RELEASE frames."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import select
import socket
import struct
import time

import ecu_debug_ui as ui
import ethernet_ota as ota
from verify_pairing_store import REVIEWED_IDENTITIES

IPS = {"SN-EJAHGJI": "172.16.0.11", "SN-EJAHGJQ": "172.16.0.21"}


def capture(serial: str, duration: float = 3.0) -> dict:
    ecu_ip = IPS[serial]
    latest, counts, sockets = {}, {}, []
    received = {}
    started = time.monotonic()
    try:
        for port in (ui.STATUS_PORT, ui.DIAGNOSTIC_PORT,
                     ui.TELEMETRY_PORT, ui.TUNING_PORT):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(sock)
            # Periodic ECU status is subnet broadcast; binding a unicast
            # address would silently discard it on Linux. Filter every peer.
            sock.bind(("0.0.0.0", port))
            sock.setblocking(False)
        sequence = int(time.time_ns()) & 0xFFFFFFFF
        sockets[-1].sendto(ui.encode_v2(ui.MSG_CONFIG_GET, 0, sequence, b""),
                          (ecu_ip, ui.TUNING_PORT))
        while time.monotonic() - started < duration:
            for sock in select.select(sockets, [], [], 0.1)[0]:
                frame, peer = sock.recvfrom(2048)
                if peer[0] != ecu_ip:
                    continue
                decoded = ui.decode_v2(frame)
                if decoded is None:
                    continue
                header, payload = decoded
                kind = header["message_type"]
                counts[str(kind)] = counts.get(str(kind), 0) + 1
                formats = {
                    ui.MSG_STATUS: ("status", ui.STATUS_FIELDS, ui.STATUS_FORMAT),
                    ui.MSG_DIAGNOSTIC: ("diagnostic", ui.DIAGNOSTIC_FIELDS,
                                        ui.DIAGNOSTIC_FORMAT),
                    ui.MSG_STEERING_STATUS: ("steering_status", ui.STEERING_STATUS_FIELDS,
                                             ui.STEERING_STATUS_FORMAT),
                }
                if kind in formats and peer[1] == ui.STATUS_PORT:
                    name, fields, fmt = formats[kind]
                    latest[name] = dict(zip(fields, struct.unpack(fmt, payload)))
                    received[name] = time.monotonic()
                elif kind == ui.MSG_SECURITY and peer[1] == ui.STATUS_PORT:
                    v = struct.unpack(ui.SECURITY_FORMAT, payload)
                    latest["security"] = dict(
                        api_version=v[0], flags=v[1], atecc_result=v[2], config_crc32c=v[3],
                        mcu_uid="".join(f"{x:08x}" for x in v[4:7]), serial=v[7].hex(),
                        revision=v[8].hex(), i2c_address=v[9], config_locked=v[10],
                        data_locked=v[11], device_status=v[12], auth_result=v[13],
                        pairing_generation=v[14])
                    received["security"] = time.monotonic()
                elif kind == ui.MSG_CONFIG_REPLY and peer[1] == ui.TUNING_PORT:
                    reply = ui.unpack_config_reply(payload)
                    if reply["request_sequence"] == sequence:
                        latest["valve_config"] = reply
        ages = {name: time.monotonic() - timestamp for name, timestamp in received.items()}
        if set(ages) != {"status", "diagnostic", "security", "steering_status"} or \
                any(age > 0.5 for age in ages.values()) or "valve_config" not in latest:
            raise RuntimeError("incomplete or stale passive status; no control was sent")
    finally:
        for sock in sockets:
            sock.close()
    client = ota.OtaClient("172.16.0.10", ecu_ip)
    try:
        latest["ota"] = client.status()
    finally:
        client.close()
    latest.update(device_serial=serial, ecu_ip=ecu_ip, frame_counts=counts,
                  captured_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                  observation_duration_s=time.monotonic() - started)
    return latest


def require_paired_safe(state: dict, serial: str, *, accepted_sequence: int | None = None):
    expected = REVIEWED_IDENTITIES[serial]
    sec, diag, status, steering = (state[x] for x in
                                   ("security", "diagnostic", "status", "steering_status"))
    if (sec["mcu_uid"] != "".join(f"{x:08x}" for x in expected.mcu_uid) or
            sec["serial"] != expected.serial.hex() or
            sec["config_crc32c"] != expected.config_crc32c or
            sec["auth_result"] != 0 or sec["pairing_generation"] != 1 or
            sec["atecc_result"] != 0 or sec["config_locked"] != 1 or
            sec["data_locked"] != 1 or sec["revision"] != "00006005"):
        raise RuntimeError("runtime MCU/ATECC pairing identity mismatch")
    if (not (diag["safety_status"] & ui.SAFETY_STATUS_PHYSICAL_ESTOP) or
            diag["requested_relay_mask"] != 0 or diag["applied_relay_mask"] != 0 or
            diag["valve_fault_flags"] != 0 or
            any(status[k] != 0 for k in ("forward_duty_permille", "reverse_duty_permille",
                                        "valve_requested_target_ma", "valve_applied_target_ma")) or
            status["forward_current_ma"] >= 50 or status["reverse_current_ma"] >= 50 or
            steering["command_enable"] != 0 or steering["speed_command_permille"] != 0):
        raise RuntimeError("physical E-stop/zero-output manufacturing guard failed")
    if (state["ota"]["result"] != 0 or state["ota"]["state"] != 0 or
            (accepted_sequence is not None and
             state["ota"]["accepted_sequence"] != accepted_sequence)):
        raise RuntimeError("unexpected OTA state/accepted sequence")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-serial", required=True, choices=tuple(IPS))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--require-paired-estop", action="store_true")
    args = parser.parse_args()
    state = capture(args.device_serial)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x") as stream:
        json.dump(state, stream, indent=2)
        stream.write("\n")
    if args.require_paired_estop:
        require_paired_safe(state, args.device_serial)
    print(f"Fresh read-only snapshot: {args.output}")


if __name__ == "__main__":
    main()
