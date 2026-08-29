#!/usr/bin/env python3
"""Inspect and explicitly provision the ECU ATECC608C over the factory UDP API.

The default ``inspect`` action is read-only.  ``provision`` requires the exact
device identity returned by the ECU and the literal ``--confirm-lock`` switch.
The ATECC configuration is always backed up before any irreversible request.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import time


ECU_MAGIC = 0x32554345
ECU_VERSION = 2
ECU_HEADER = struct.Struct("<IBBHHHIII")
ECU_FACTORY_STATUS = 0x30
ECU_FACTORY_PROVISION = 0x31
ECU_TUNING_PORT = 50005
FACTORY_TOKEN = 0x4B434F4C
STATUS = struct.Struct("<i6IH4B9s4s128s64s")
REQUEST = struct.Struct("<5I9s3x")
ALL_PHASES = 0x7F
PRIVATE_SLOT = 2

PHASE_NAMES = (
    (1 << 0, "probed"),
    (1 << 1, "journaled"),
    (1 << 2, "config_locked"),
    (1 << 3, "data_locked"),
    (1 << 4, "slot_locked"),
    (1 << 5, "manifest_saved"),
    (1 << 6, "authenticated"),
)


def crc32c(data: bytes) -> int:
    value = 0xFFFFFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
    return value ^ 0xFFFFFFFF


def encode_frame(message_type: int, sequence: int, payload: bytes = b"") -> bytes:
    header = ECU_HEADER.pack(
        ECU_MAGIC, ECU_VERSION, message_type, ECU_HEADER.size,
        len(payload), 0, sequence, int(time.monotonic() * 1000) & 0xFFFFFFFF,
        0,
    )
    frame = bytearray(header + payload)
    struct.pack_into("<I", frame, 20, crc32c(frame))
    return bytes(frame)


def decode_frame(frame: bytes, expected_sequence: int) -> bytes:
    if len(frame) < ECU_HEADER.size:
        raise RuntimeError("short ECU response")
    values = ECU_HEADER.unpack_from(frame)
    magic, version, message_type, header_size, payload_size, _, sequence, _, wire_crc = values
    if (magic, version, message_type, header_size) != (
        ECU_MAGIC, ECU_VERSION, ECU_FACTORY_STATUS, ECU_HEADER.size,
    ):
        raise RuntimeError("unexpected ECU response header")
    if len(frame) != header_size + payload_size:
        raise RuntimeError("ECU response length mismatch")
    checked = bytearray(frame)
    struct.pack_into("<I", checked, 20, 0)
    if crc32c(checked) != wire_crc:
        raise RuntimeError("ECU response CRC32C mismatch")
    payload = frame[header_size:]
    # The response header carries the ECU's independent transmit sequence.
    # Every operation response echoes the request sequence in its payload;
    # that is the correlation field shared by the two protocol peers.
    if len(payload) < 8 or struct.unpack_from("<I", payload, 4)[0] != \
            expected_sequence:
        raise RuntimeError("unexpected ECU request sequence")
    return payload


def exchange(sock: socket.socket, ecu_ip: str, message_type: int,
             sequence: int, payload: bytes = b"", timeout: float = 5.0) -> bytes:
    sock.settimeout(timeout)
    request = encode_frame(message_type, sequence, payload)
    for attempt in range(3):
        sock.sendto(request, (ecu_ip, ECU_TUNING_PORT))
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            sock.settimeout(remaining)
            try:
                response, peer = sock.recvfrom(1024)
            except TimeoutError:
                break
            if peer[0] != ecu_ip:
                continue
            try:
                return decode_frame(response, sequence)
            except RuntimeError:
                continue
        if attempt != 2:
            time.sleep(0.2)
    raise TimeoutError(f"ECU {ecu_ip}:{ECU_TUNING_PORT} did not respond")


def parse_status(payload: bytes) -> dict[str, object]:
    if len(payload) != STATUS.size:
        raise RuntimeError(
            f"factory status ABI mismatch: received {len(payload)}, expected {STATUS.size}")
    values = STATUS.unpack(payload)
    result, request_sequence, phases, config_crc = values[:4]
    uid = values[4:7]
    slot_mask = values[7]
    config_locked, data_locked, device_status, private_slot = values[8:12]
    serial, revision, config, public_key = values[12:]
    return {
        "result": result,
        "request_sequence": request_sequence,
        "phase_flags": phases,
        "phases": [name for bit, name in PHASE_NAMES if phases & bit],
        "config_crc32c": config_crc,
        "mcu_uid_words": list(uid),
        "mcu_uid_hex": "".join(f"{word:08x}" for word in uid),
        "slot_locked_mask": slot_mask,
        "config_locked": bool(config_locked),
        "data_locked": bool(data_locked),
        "device_status": device_status,
        "private_key_slot": private_slot,
        "atecc_serial": serial.hex(),
        "atecc_revision": revision.hex(),
        "config_hex": config.hex(),
        "public_key_hex": public_key.hex(),
    }


def save_backup(status: dict[str, object], root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    os.chmod(root, 0o700)
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    serial = str(status["atecc_serial"])
    prefix = root / f"atecc608c-{serial}-{stamp}"
    config = bytes.fromhex(str(status["config_hex"]))
    config_path = prefix.with_name(prefix.name + "-config.bin")
    json_path = prefix.with_name(prefix.name + "-status.json")
    digest_path = prefix.with_name(prefix.name + "-sha256.txt")
    config_path.write_bytes(config)
    json_path.write_text(json.dumps(status, indent=2, sort_keys=True) + "\n")
    hashes = (
        f"{hashlib.sha256(config).hexdigest()}  {config_path.name}\n"
        f"{hashlib.sha256(json_path.read_bytes()).hexdigest()}  {json_path.name}\n"
    )
    digest_path.write_text(hashes)
    for path in (config_path, json_path, digest_path):
        os.chmod(path, 0o600)
    return prefix


def print_status(status: dict[str, object]) -> None:
    print(f"result: {status['result']}")
    print(f"MCU UID: {status['mcu_uid_hex']}")
    print(f"ATECC serial: {status['atecc_serial']}")
    print(f"ATECC revision: {status['atecc_revision']}")
    print(f"config CRC32C: 0x{int(status['config_crc32c']):08X}")
    print(f"device status: 0x{int(status['device_status']):02X}")
    print(f"config/data locked: {status['config_locked']}/{status['data_locked']}")
    print(f"slot lock bitmap: 0x{int(status['slot_locked_mask']):04X}")
    print(f"factory phases: {', '.join(status['phases']) or 'none'}")


def inspect(sock: socket.socket, args: argparse.Namespace, sequence: int) -> dict[str, object]:
    status = parse_status(exchange(sock, args.ecu_ip, ECU_FACTORY_STATUS, sequence))
    if status["request_sequence"] != sequence:
        raise RuntimeError("factory payload sequence mismatch")
    print_status(status)
    return status


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("inspect", "provision"), nargs="?",
                        default="inspect")
    parser.add_argument("--ecu-ip", default="172.16.0.11")
    parser.add_argument("--host-ip", default="172.16.0.10")
    parser.add_argument("--confirm-lock", action="store_true",
                        help="acknowledge irreversible ATECC zone/slot locks")
    parser.add_argument(
        "--backup-dir", type=Path,
        default=Path(__file__).resolve().parent.parent / "artifacts/device-backups")
    args = parser.parse_args()

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((args.host_ip, 0))
        sequence = int(time.time_ns()) & 0xFFFFFFFF
        status = inspect(sock, args, sequence)
        backup = save_backup(status, args.backup_dir)
        print(f"read-only backup: {backup}-*")
        if args.action == "inspect":
            return 0 if status["result"] == 0 else 1
        if not args.confirm_lock:
            parser.error("provision requires --confirm-lock (ATECC locks are irreversible)")
        if status["result"] != 0:
            raise RuntimeError("refusing provisioning after failed inspection")
        if status["config_locked"] or status["data_locked"]:
            if not (int(status["phase_flags"]) & (1 << 1)):
                raise RuntimeError("locked ATECC has no matching ECU factory journal")

        uid = [int(value) for value in status["mcu_uid_words"]]
        serial = bytes.fromhex(str(status["atecc_serial"]))
        provision_request = REQUEST.pack(
            FACTORY_TOKEN, int(status["config_crc32c"]), *uid, serial)
        sequence = (sequence + 1) & 0xFFFFFFFF
        result = parse_status(exchange(
            sock, args.ecu_ip, ECU_FACTORY_PROVISION, sequence,
            provision_request, timeout=35.0))
        if result["request_sequence"] != sequence:
            raise RuntimeError("provision response sequence mismatch")
        print_status(result)
        save_backup(result, args.backup_dir)
        slot_is_locked = not (
            int(result["slot_locked_mask"]) & (1 << PRIVATE_SLOT))
        if (result["result"] != 0 or result["phase_flags"] != ALL_PHASES or
                not result["config_locked"] or not result["data_locked"] or
                result["private_key_slot"] != PRIVATE_SLOT or
                not slot_is_locked or
                not any(bytes.fromhex(str(result["public_key_hex"])) )):
            raise RuntimeError("ATECC provisioning did not reach authenticated final state")

        sequence = (sequence + 1) & 0xFFFFFFFF
        verified = inspect(sock, args, sequence)
        if verified["phase_flags"] != ALL_PHASES:
            raise RuntimeError("post-provision read-back verification failed")
        print("ATECC608C provisioning and board-pairing authentication: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
