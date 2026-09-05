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
DIAGNOSTIC_PORT = 50003
ECU_STATUS_SOURCE_PORT = 50001
V2_MAGIC = 0x32554345
V2_VERSION = 2
V2_HEADER_FORMAT = "<IBBHHHIII"
V2_HEADER_SIZE = struct.calcsize(V2_HEADER_FORMAT)
V2_CRC_OFFSET = 20
MSG_DIAGNOSTIC = 0x03
MSG_STATUS, MSG_BEGIN, MSG_CHUNK, MSG_FINISH = 0x40, 0x41, 0x42, 0x43
STATUS_FORMAT = "<iIIi7I"
DIAGNOSTIC_FORMAT = "<12I12Hb5BII"
MANIFEST_FORMAT = "<12I32s32s4I"
CHUNK_FORMAT = "<IB3xIHHI512s"
DEFAULT_PUBLIC_KEY = Path(__file__).with_name("ota-transport-public.pem")

SAFETY_STATUS_PHYSICAL_ESTOP = 1 << 1
SAFETY_STATUS_ATECC_AUTHENTICATED = 1 << 22
SAFETY_STATUS_OTA_ACTIVE = 1 << 23
SAFETY_STATUS_OTA_READY = 1 << 24
SAFETY_STATUS_OTA_UNCONFIRMED = 1 << 25
SAFETY_RELAY_K12_RUN_PERMIT = 1 << 22
ECU_CAP_LATCHED_ESTOP_RESET = 1 << 11
OTA_RUNTIME_CONFIRM_TIMEOUT_S = 30.0


class IntentionalInterruption(RuntimeError):
    """Bench acceptance stopped before FINISH; the ECU must not swap."""


def validate_package_metadata(fields: tuple, metadata: object) -> None:
    """Require the human-readable metadata to mirror the signed manifest."""
    major, minor, revision, build = fields[4:8]
    version = f"{major}.{minor}.{revision}"
    if build:
        version += f"+{build}"
    expected = {
        "format": "roller-ecu-ota-v1",
        "version": version,
        "security_counter": fields[8],
        "update_sequence": fields[3],
        "layout_version": fields[2],
        "secure_sha256": fields[12].hex(),
        "nonsecure_sha256": fields[13].hex(),
        "secure_size": fields[10],
        "nonsecure_size": fields[11],
    }
    if not isinstance(metadata, dict) or set(metadata) != set(expected):
        raise ValueError("OTA metadata does not match the signed manifest schema")
    for name, expected_value in expected.items():
        actual_value = metadata[name]
        if type(actual_value) is not type(expected_value) or actual_value != expected_value:
            raise ValueError(
                f"OTA metadata field {name!r} does not match the signed manifest"
            )


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


def decode_diagnostic(frame: bytes) -> dict[str, int]:
    expected_payload_size = struct.calcsize(DIAGNOSTIC_FORMAT)
    if len(frame) != V2_HEADER_SIZE + expected_payload_size:
        raise ValueError("invalid diagnostic frame length")
    header = struct.unpack_from(V2_HEADER_FORMAT, frame)
    if header[0:5] != (
            V2_MAGIC, V2_VERSION, MSG_DIAGNOSTIC, V2_HEADER_SIZE,
            expected_payload_size):
        raise ValueError("invalid diagnostic header")
    checked = bytearray(frame)
    received_crc = struct.unpack_from("<I", checked, V2_CRC_OFFSET)[0]
    struct.pack_into("<I", checked, V2_CRC_OFFSET, 0)
    if crc32c(checked) != received_crc:
        raise ValueError("invalid diagnostic CRC")
    values = struct.unpack_from(DIAGNOSTIC_FORMAT, frame, V2_HEADER_SIZE)
    return {
        "capability_flags": values[0],
        "safety_status": values[1],
        "requested_relay_mask": values[2],
        "applied_relay_mask": values[3],
        "secure_uptime_ms": values[4],
    }


def release_requires_latched_estop_capability(version: str) -> bool:
    numeric = version.split("+", 1)[0].split(".")
    if len(numeric) != 3 or any(not value.isdigit() for value in numeric):
        raise ValueError("invalid release version in signed OTA metadata")
    return tuple(int(value) for value in numeric) >= (1, 0, 20)


DiagnosticProvider = Callable[[], tuple[dict[str, int] | None, float]]


class OtaSafetyMonitor:
    """Observe the independent E-stop input on ECU diagnostic broadcasts."""

    def __init__(self, ecu_ip: str,
                 diagnostic_provider: DiagnosticProvider | None = None) -> None:
        self.ecu_ip = ecu_ip
        self.diagnostic_provider = diagnostic_provider
        self.socket: socket.socket | None = None
        if diagnostic_provider is None:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.socket.bind(("0.0.0.0", DIAGNOSTIC_PORT))
            self.socket.setblocking(False)
        self.latest: dict[str, int] | None = None
        self.latest_received_at = 0.0
        self.provider_received_at = 0.0
        self.generation = 0
        self.armed = False
        self.release_observed = False

    def close(self) -> None:
        if self.socket is not None:
            self.socket.close()

    def _record(self, diagnostic: dict[str, int], received_at: float) -> None:
        self.latest = diagnostic
        self.latest_received_at = received_at
        self.generation += 1
        if (self.armed and not (
                diagnostic["safety_status"] &
                SAFETY_STATUS_PHYSICAL_ESTOP)):
            self.release_observed = True

    def drain(self) -> None:
        if self.diagnostic_provider is not None:
            diagnostic, received_at = self.diagnostic_provider()
            if (diagnostic is not None and
                    received_at > self.provider_received_at):
                self.provider_received_at = received_at
                self._record(dict(diagnostic), received_at)
            return

        assert self.socket is not None
        while True:
            try:
                frame, source = self.socket.recvfrom(2048)
            except BlockingIOError:
                return
            if (source[0] != self.ecu_ip or
                    source[1] != ECU_STATUS_SOURCE_PORT):
                continue
            try:
                diagnostic = decode_diagnostic(frame)
            except ValueError:
                continue
            self._record(diagnostic, time.monotonic())

    def wait_until_held(self, timeout: float = 2.0, *,
                        after_generation: int | None = None) -> dict[str, int]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.drain()
            if self.release_observed:
                raise RuntimeError(
                    "physical E-stop release was observed during OTA; "
                    "the transfer/confirmation cannot continue"
                )
            if (self.latest is not None and
                    (after_generation is None or
                     self.generation > after_generation) and
                    (self.latest["safety_status"] &
                     SAFETY_STATUS_PHYSICAL_ESTOP)):
                return self.latest
            time.sleep(0.02)
        raise RuntimeError(
            "no fresh ECU diagnostic proves that the physical E-stop is held"
        )

    def arm(self) -> None:
        self.armed = False
        self.release_observed = False
        self.drain()
        baseline = self.generation
        self.wait_until_held(after_generation=baseline)
        self.armed = True

    def require_held(self) -> None:
        self.drain()
        if self.release_observed:
            raise RuntimeError(
                "physical E-stop release was observed during OTA; keep the "
                "button held through transfer, reset, and confirmation"
            )
        if (self.latest is None or
                not (self.latest["safety_status"] &
                     SAFETY_STATUS_PHYSICAL_ESTOP)):
            raise RuntimeError("physical E-stop is not held")
        if time.monotonic() - self.latest_received_at > 0.5:
            baseline = self.generation
            self.wait_until_held(after_generation=baseline)

    def wait_for_confirmed_runtime(self, *, after_generation: int,
                                   require_new_capability: bool,
                                   timeout: float = 3.0) -> dict[str, int]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            diagnostic = self.wait_until_held(
                timeout=max(0.02, deadline - time.monotonic()),
                after_generation=after_generation,
            )
            status = diagnostic["safety_status"]
            if (require_new_capability and not (
                    diagnostic["capability_flags"] &
                    ECU_CAP_LATCHED_ESTOP_RESET)):
                raise RuntimeError(
                    "post-reset ECU lacks the required latched-E-stop capability; "
                    "the new paired image was not confirmed (possible rollback)"
                )
            if not (status & SAFETY_STATUS_ATECC_AUTHENTICATED):
                raise RuntimeError("post-reset ECU identity is not authenticated")
            if status & (SAFETY_STATUS_OTA_ACTIVE |
                         SAFETY_STATUS_OTA_READY |
                         SAFETY_STATUS_OTA_UNCONFIRMED):
                after_generation = self.generation
                continue
            if diagnostic["applied_relay_mask"] & \
                    SAFETY_RELAY_K12_RUN_PERMIT:
                raise RuntimeError(
                    "K12 is commanded on while the physical E-stop is held"
                )
            return diagnostic
        raise RuntimeError(
            "no confirmed post-reset safety diagnostic was received"
        )


class OtaClient:
    def __init__(self, host_ip: str, ecu_ip: str, timeout: float = 0.6,
                 retries: int = 8) -> None:
        self.target = (ecu_ip, OTA_PORT)
        self.host_ip = host_ip
        self.ecu_ip = ecu_ip
        self.timeout = timeout
        self.retries = retries
        self.sequence = 0
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind((host_ip, 0))
        self.socket.settimeout(timeout)

    def close(self) -> None:
        self.socket.close()

    def request(self, message_type: int, payload: bytes,
                timeout: float | None = None,
                attempts: int | None = None) -> dict[str, int]:
        self.sequence = (self.sequence + 1) & 0xFFFFFFFF
        sequence = self.sequence
        frame = encode_v2(message_type, sequence, payload)
        attempt_count = self.retries if attempts is None else attempts
        if attempt_count <= 0:
            raise ValueError("OTA request attempts must be positive")
        old_timeout = self.socket.gettimeout()
        self.socket.settimeout(timeout or self.timeout)
        try:
            for _ in range(attempt_count):
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
    validate_package_metadata(fields, metadata)
    return manifest + signature, secure, nonsecure, metadata


def require_ok(status: dict[str, int], operation: str) -> None:
    if status["result"] != 0:
        if operation == "begin" and status["result"] == -2:
            raise RuntimeError(
                "begin rejected: press and hold the ECU physical E-stop; "
                "Secure ESTOP_DETECT must remain active through transfer, "
                "swap, and image confirmation"
            )
        raise RuntimeError(f"{operation} rejected: {status}")


def transfer(client: OtaClient, package: Path, public_key: Path,
             progress: Callable[[dict], None] | None = None,
             stop_after_bytes: int | None = None,
             diagnostic_provider: DiagnosticProvider | None = None) -> None:
    def report(stage: str, **values: object) -> None:
        if progress is not None:
            progress({"stage": stage, **values})

    begin, secure, nonsecure, metadata = load_package(package, public_key)
    update_sequence = int(metadata["update_sequence"])
    total_size = len(secure) + len(nonsecure)
    if stop_after_bytes is not None and not 0 < stop_after_bytes < total_size:
        raise ValueError("stop-after-bytes must be inside the paired image payload")
    monitor = OtaSafetyMonitor(client.ecu_ip, diagnostic_provider)
    try:
        report("waiting-estop", metadata=metadata, transferred=0,
               total=total_size)
        print("Waiting for a fresh physical E-stop diagnostic...")
        monitor.arm()
        monitor.require_held()
        report("begin", metadata=metadata, transferred=0,
               total=total_size)
        status = client.request(MSG_BEGIN, begin, timeout=20.0)
        require_ok(status, "begin")
        monitor.require_held()
        report("transfer", metadata=metadata,
               transferred=status["secure_received"] +
                           status["nonsecure_received"],
               total=total_size, ecu_status=status)
        print(f"ECU accepted {metadata['version']}; safe output quarantine active")
        for image_index, image, received_name in (
                (0, secure, "secure_received"),
                (1, nonsecure, "nonsecure_received")):
            offset = status[received_name]
            if offset > len(image) or offset % 16:
                raise RuntimeError(f"ECU reported invalid resume offset {offset}")
            while offset < len(image):
                monitor.require_held()
                data = image[offset:offset + 512]
                padded = data + bytes(512 - len(data))
                payload = struct.pack(
                    CHUNK_FORMAT, update_sequence, image_index, offset,
                    len(data), 0, crc32c(data), padded
                )
                status = client.request(MSG_CHUNK, payload)
                require_ok(status, f"image {image_index} offset {offset:#x}")
                offset += len(data)
                report("transfer", metadata=metadata,
                       transferred=status["secure_received"] +
                                   status["nonsecure_received"],
                       total=total_size, ecu_status=status)
                transferred = (status["secure_received"] +
                               status["nonsecure_received"])
                if (stop_after_bytes is not None and
                        transferred >= stop_after_bytes):
                    report("interrupted", metadata=metadata,
                           transferred=transferred, total=total_size,
                           ecu_status=status)
                    raise IntentionalInterruption(
                        f"bench interruption after {transferred}/{total_size} "
                        "bytes; FINISH was not sent and the ECU was not reset"
                    )
                if offset % 0x8000 == 0 or offset == len(image):
                    print(f"image {image_index}: {offset}/{len(image)} bytes")

        monitor.require_held()
        pre_finish_generation = monitor.generation
        status = client.request(
            MSG_FINISH, struct.pack("<I", update_sequence), timeout=2.0
        )
        require_ok(status, "finish")
        if status["state"] != 2:
            raise RuntimeError(
                f"ECU did not enter ready-to-swap state: {status}"
            )
        monitor.wait_until_held(
            timeout=1.0, after_generation=pre_finish_generation
        )
        report("reset", metadata=metadata, transferred=total_size,
               total=total_size, ecu_status=status)
        print("Transfer verified; keep the physical E-stop held while ECU "
              "resets, swaps, authenticates, and confirms both images")

        report("confirming", metadata=metadata, transferred=total_size,
               total=total_size, ecu_status=status)
        deadline = time.monotonic() + OTA_RUNTIME_CONFIRM_TIMEOUT_S
        runtime_status: dict[str, int] | None = None
        time.sleep(0.55)
        while time.monotonic() < deadline:
            monitor.drain()
            if monitor.release_observed:
                monitor.require_held()
            try:
                candidate = client.request(
                    MSG_STATUS, b"", timeout=0.4, attempts=1
                )
            except TimeoutError:
                time.sleep(0.10)
                continue
            if candidate["accepted_sequence"] > update_sequence:
                raise RuntimeError(
                    "ECU accepted sequence is newer than this release"
                )
            if (candidate["result"] == 0 and candidate["ota_result"] == 0 and
                    candidate["state"] == 0 and
                    candidate["accepted_sequence"] == update_sequence):
                runtime_status = candidate
                break
            time.sleep(0.10)
        if runtime_status is None:
            raise RuntimeError(
                "ECU did not return with the expected confirmed OTA sequence"
            )
        monitor.drain()
        diagnostic_generation = monitor.generation
        monitor.wait_for_confirmed_runtime(
            after_generation=diagnostic_generation,
            require_new_capability=release_requires_latched_estop_capability(
                str(metadata["version"])
            ),
        )
        report("complete", metadata=metadata, transferred=total_size,
               total=total_size, ecu_status=runtime_status)
        print("Paired runtime confirmation verified; the physical E-stop may "
              "now be released")
    finally:
        monitor.close()


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
