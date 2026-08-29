#!/usr/bin/env python3
"""Verify and transfer a signed Roller ECU update over the dedicated UDP port."""

from __future__ import annotations

import argparse
import hashlib
import json
import socket
import struct
import sys
import time
import zipfile
from pathlib import Path
from typing import Callable

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature


HOST_IP = "172.16.0.10"
ECU_IP = "172.16.0.11"
OTA_PORT = 50006
V2_MAGIC = 0x32554345
V2_VERSION = 2
V2_HEADER_FORMAT = "<IBBHHHIII"
V2_HEADER_SIZE = struct.calcsize(V2_HEADER_FORMAT)
V2_CRC_OFFSET = 20
MSG_STATUS, MSG_BEGIN, MSG_CHUNK, MSG_FINISH = 0x40, 0x41, 0x42, 0x43
STATUS_FORMAT = "<iIIi7I"
MANIFEST_FORMAT = "<12I32s32s4I"
CHUNK_FORMAT = "<IB3xIHHI512s"
DEFAULT_PUBLIC_KEY = Path("/home/plac/.local/share/roller-ecu-pki/ota-transport-public.pem")


class IntentionalInterruption(RuntimeError):
    """Bench acceptance stopped before FINISH; the ECU must not swap."""


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def encode_v2(message_type: int, sequence: int, payload: bytes) -> bytes:
    header = struct.pack(V2_HEADER_FORMAT, V2_MAGIC, V2_VERSION,
                         message_type, V2_HEADER_SIZE, len(payload), 0,
                         sequence, int(time.monotonic() * 1000) & 0xFFFFFFFF, 0)
    frame = bytearray(header + payload)
    struct.pack_into("<I", frame, V2_CRC_OFFSET, crc32c(frame))
    return bytes(frame)


def decode_status(frame: bytes, expected_sequence: int) -> dict[str, int]:
    if len(frame) != V2_HEADER_SIZE + struct.calcsize(STATUS_FORMAT):
        raise ValueError("invalid OTA status frame length")
    header = struct.unpack_from(V2_HEADER_FORMAT, frame)
    if (header[0], header[1], header[2], header[3], header[4]) != (
            V2_MAGIC, V2_VERSION, MSG_STATUS, V2_HEADER_SIZE,
            struct.calcsize(STATUS_FORMAT)):
        raise ValueError("invalid OTA status header")
    checked = bytearray(frame)
    received_crc = struct.unpack_from("<I", checked, V2_CRC_OFFSET)[0]
    struct.pack_into("<I", checked, V2_CRC_OFFSET, 0)
    if crc32c(checked) != received_crc:
        raise ValueError("invalid OTA status CRC")
    values = struct.unpack_from(STATUS_FORMAT, frame, V2_HEADER_SIZE)
    names = ("result", "request_sequence", "api_version", "ota_result",
             "state", "update_sequence", "accepted_sequence",
             "secure_received", "nonsecure_received", "secure_image_size",
             "nonsecure_image_size")
    status = dict(zip(names, values))
    # ECU response headers use a device-side transmit sequence.  The payload
    # explicitly echoes the request sequence and is therefore authoritative.
    if status["request_sequence"] != expected_sequence:
        raise ValueError("unexpected OTA request sequence")
    return status


class OtaClient:
    def __init__(self, host_ip: str, ecu_ip: str, timeout: float = 0.6,
                 retries: int = 8) -> None:
        self.target = (ecu_ip, OTA_PORT)
        self.timeout = timeout
        self.retries = retries
        self.sequence = 0
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind((host_ip, 0))
        self.socket.settimeout(timeout)

    def close(self) -> None:
        self.socket.close()

    def request(self, message_type: int, payload: bytes,
                timeout: float | None = None) -> dict[str, int]:
        self.sequence = (self.sequence + 1) & 0xFFFFFFFF
        sequence = self.sequence
        frame = encode_v2(message_type, sequence, payload)
        old_timeout = self.socket.gettimeout()
        self.socket.settimeout(timeout or self.timeout)
        try:
            for _ in range(self.retries):
                self.socket.sendto(frame, self.target)
                try:
                    while True:
                        response, source = self.socket.recvfrom(256)
                        if source[0] != self.target[0]:
                            continue
                        try:
                            return decode_status(response, sequence)
                        except ValueError:
                            continue
                except socket.timeout:
                    continue
        finally:
            self.socket.settimeout(old_timeout)
        raise TimeoutError(f"ECU did not acknowledge OTA message {message_type:#x}")

    def status(self) -> dict[str, int]:
        return self.request(MSG_STATUS, b"")


def load_package(path: Path, public_key_path: Path) -> tuple[bytes, bytes, bytes, dict]:
    with zipfile.ZipFile(path, "r") as archive:
        expected = {"metadata.json", "manifest.bin", "signature.bin",
                    "secure.bin", "nonsecure.bin"}
        if set(archive.namelist()) != expected:
            raise ValueError("unexpected OTA package members")
        metadata = json.loads(archive.read("metadata.json"))
        manifest = archive.read("manifest.bin")
        signature = archive.read("signature.bin")
        secure = archive.read("secure.bin")
        nonsecure = archive.read("nonsecure.bin")
    if len(manifest) != 128 or len(signature) != 64:
        raise ValueError("invalid manifest or signature length")
    fields = struct.unpack(MANIFEST_FORMAT, manifest)
    if fields[0:3] != (0x31544F52, 1, 0x00010000) or fields[9] != 3:
        raise ValueError("unsupported OTA manifest policy")
    if len(secure) != fields[10] or len(nonsecure) != fields[11]:
        raise ValueError("image size does not match manifest")
    if hashlib.sha256(secure).digest() != fields[12] or \
       hashlib.sha256(nonsecure).digest() != fields[13]:
        raise ValueError("image hash does not match manifest")
    public_key = serialization.load_pem_public_key(public_key_path.read_bytes())
    if not isinstance(public_key, ec.EllipticCurvePublicKey):
        raise TypeError("OTA public key is not EC")
    r = int.from_bytes(signature[:32], "big")
    s = int.from_bytes(signature[32:], "big")
    try:
        public_key.verify(encode_dss_signature(r, s), manifest,
                          ec.ECDSA(hashes.SHA256()))
    except InvalidSignature as error:
        raise ValueError("OTA transport signature is invalid") from error
    return manifest + signature, secure, nonsecure, metadata


def require_ok(status: dict[str, int], operation: str) -> None:
    if status["result"] != 0:
        raise RuntimeError(f"{operation} rejected: {status}")


def transfer(client: OtaClient, package: Path, public_key: Path,
             progress: Callable[[dict], None] | None = None,
             stop_after_bytes: int | None = None) -> None:
    def report(stage: str, **values: object) -> None:
        if progress is not None:
            progress({"stage": stage, **values})

    begin, secure, nonsecure, metadata = load_package(package, public_key)
    update_sequence = int(metadata["update_sequence"])
    total_size = len(secure) + len(nonsecure)
    if stop_after_bytes is not None and not 0 < stop_after_bytes < total_size:
        raise ValueError("stop-after-bytes must be inside the paired image payload")
    report("begin", metadata=metadata, transferred=0,
           total=total_size)
    status = client.request(MSG_BEGIN, begin, timeout=20.0)
    require_ok(status, "begin")
    report("transfer", metadata=metadata,
           transferred=status["secure_received"] + status["nonsecure_received"],
           total=len(secure) + len(nonsecure), ecu_status=status)
    print(f"ECU accepted {metadata['version']}; safe output quarantine active")
    for image_index, image, received_name in (
            (0, secure, "secure_received"),
            (1, nonsecure, "nonsecure_received")):
        offset = status[received_name]
        if offset > len(image) or offset % 16:
            raise RuntimeError(f"ECU reported invalid resume offset {offset}")
        while offset < len(image):
            data = image[offset:offset + 512]
            padded = data + bytes(512 - len(data))
            payload = struct.pack(CHUNK_FORMAT, update_sequence, image_index,
                                  offset, len(data), 0, crc32c(data), padded)
            status = client.request(MSG_CHUNK, payload)
            require_ok(status, f"image {image_index} offset {offset:#x}")
            offset += len(data)
            report("transfer", metadata=metadata,
                   transferred=status["secure_received"] +
                               status["nonsecure_received"],
                   total=total_size, ecu_status=status)
            transferred = (status["secure_received"] +
                           status["nonsecure_received"])
            if stop_after_bytes is not None and transferred >= stop_after_bytes:
                report("interrupted", metadata=metadata,
                       transferred=transferred, total=total_size,
                       ecu_status=status)
                raise IntentionalInterruption(
                    f"bench interruption after {transferred}/{total_size} bytes; "
                    "FINISH was not sent and the ECU was not reset"
                )
            if offset % 0x8000 == 0 or offset == len(image):
                print(f"image {image_index}: {offset}/{len(image)} bytes")
    status = client.request(MSG_FINISH, struct.pack("<I", update_sequence),
                            timeout=2.0)
    require_ok(status, "finish")
    if status["state"] != 2:
        raise RuntimeError(f"ECU did not enter ready-to-swap state: {status}")
    report("reset", metadata=metadata, transferred=len(secure) + len(nonsecure),
           total=len(secure) + len(nonsecure), ecu_status=status)
    print("Transfer verified; ECU is resetting into OEMiROT test swap")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", nargs="?", type=Path)
    parser.add_argument("--status", action="store_true")
    parser.add_argument("--host-ip", default=HOST_IP)
    parser.add_argument("--ecu-ip", default=ECU_IP)
    parser.add_argument("--public-key", type=Path, default=DEFAULT_PUBLIC_KEY)
    parser.add_argument(
        "--stop-after-bytes", type=int,
        help="bench-test only: stop mid-transfer without FINISH or reset")
    args = parser.parse_args()
    if not args.status and args.package is None:
        parser.error("provide an update package or --status")
    client = OtaClient(args.host_ip, args.ecu_ip)
    try:
        try:
            if args.status:
                print(json.dumps(client.status(), indent=2))
            else:
                transfer(client, args.package, args.public_key,
                         stop_after_bytes=args.stop_after_bytes)
        except IntentionalInterruption as interruption:
            print(str(interruption))
        except (OSError, ValueError, RuntimeError) as error:
            print(f"OTA failed: {error}", file=sys.stderr)
            return 1
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
