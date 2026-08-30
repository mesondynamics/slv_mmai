#!/usr/bin/env python3
"""Verify the reviewed ECU's two committed ATECC pairing-record sectors."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


SECTOR_SIZE = 0x2000
RECORD_SIZE = 144
MAGIC = 0x49435545
SCHEMA = 1
LAYOUT_VERSION = 0x00010000
COMMIT = 0x444E4249
EXPECTED_GENERATION = 1
EXPECTED_MCU_UID = (0x00380061, 0x34345112, 0x32383537)
EXPECTED_CONFIG_CRC32C = 0xEBB326F3
EXPECTED_SERIAL = bytes.fromhex("0123d47eb2ee0e9bee")
EXPECTED_REVISION = bytes.fromhex("00006005")
EXPECTED_I2C_ADDRESS = 0xC0
EXPECTED_PRIVATE_KEY_SLOT = 2
EXPECTED_PUBLIC_KEY = bytes.fromhex(
    "47ec7c6a4671e5d56546d84bb8fb29e88c6799b545e99d8b2d7e2da461faec"
    "fb2de509483ba53df727cc82db5a2a7726df9f89cd2e691d6fea45d7a7ab5164ee"
)


class VerificationError(RuntimeError):
    pass


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def verify_sector(sector: bytes, label: str) -> dict[str, object]:
    _require(len(sector) == SECTOR_SIZE,
             f"{label}: sector size is not 0x{SECTOR_SIZE:X}")
    record = sector[:RECORD_SIZE]
    _require(all(value == 0xFF for value in sector[RECORD_SIZE:]),
             f"{label}: unexpected data follows the committed record")

    magic, schema, generation, layout = struct.unpack_from("<4I", record, 0)
    uid = struct.unpack_from("<3I", record, 16)
    config_crc = struct.unpack_from("<I", record, 28)[0]
    serial = record[32:41]
    revision = record[41:45]
    i2c_address, private_slot, identity_padding = record[45:48]
    public_key = record[48:112]
    encoded_size, encoded_crc = struct.unpack_from("<2I", record, 112)
    reserved = record[120:128]
    commit = struct.unpack_from("<I", record, 128)[0]
    commit_padding = record[132:144]

    _require(magic == MAGIC, f"{label}: pairing magic mismatch")
    _require(schema == SCHEMA, f"{label}: pairing schema mismatch")
    _require(generation == EXPECTED_GENERATION,
             f"{label}: pairing generation mismatch")
    _require(layout == LAYOUT_VERSION, f"{label}: Flash layout mismatch")
    _require(uid == EXPECTED_MCU_UID, f"{label}: MCU UID mismatch")
    _require(config_crc == EXPECTED_CONFIG_CRC32C,
             f"{label}: ATECC config CRC32C mismatch")
    _require(serial == EXPECTED_SERIAL, f"{label}: ATECC serial mismatch")
    _require(revision == EXPECTED_REVISION,
             f"{label}: ATECC revision mismatch")
    _require(i2c_address == EXPECTED_I2C_ADDRESS,
             f"{label}: ATECC I2C address mismatch")
    _require(private_slot == EXPECTED_PRIVATE_KEY_SLOT,
             f"{label}: ATECC private-key slot mismatch")
    _require(identity_padding == 0, f"{label}: identity padding mismatch")
    _require(public_key == EXPECTED_PUBLIC_KEY,
             f"{label}: paired public key mismatch")
    _require(encoded_size == RECORD_SIZE,
             f"{label}: pairing record size mismatch")
    _require(encoded_crc == crc32c(record[:116]),
             f"{label}: pairing record CRC32C mismatch")
    _require(reserved == b"\xFF" * len(reserved),
             f"{label}: reserved bytes are not erased")
    _require(commit == COMMIT, f"{label}: commit marker mismatch")
    _require(commit_padding == b"\xFF" * len(commit_padding),
             f"{label}: commit padding is not erased")

    return {
        "label": label,
        "generation": generation,
        "mcu_uid": "".join(f"{word:08x}" for word in uid),
        "atecc_serial": serial.hex(),
        "atecc_revision": revision.hex(),
        "atecc_config_crc32c": f"0x{config_crc:08X}",
        "private_key_slot": private_slot,
        "record_sha256": hashlib.sha256(record).hexdigest(),
        "sector_sha256": hashlib.sha256(sector).hexdigest(),
    }


def verify_dual_store(store: bytes) -> dict[str, object]:
    _require(len(store) == 2 * SECTOR_SIZE,
             f"pairing store size is not 0x{2 * SECTOR_SIZE:X}")
    sector_a = store[:SECTOR_SIZE]
    sector_b = store[SECTOR_SIZE:]
    result_a = verify_sector(sector_a, "A")
    result_b = verify_sector(sector_b, "B")
    _require(sector_a == sector_b,
             "A/B pairing sectors are valid but not byte-identical")
    return {
        "result": "PASS",
        "redundant_copies": 2,
        "byte_identical": True,
        "store_sha256": hashlib.sha256(store).hexdigest(),
        "sector_a": result_a,
        "sector_b": result_b,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("sector", "dual"))
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    data = args.image.read_bytes()
    try:
        result = (verify_sector(data, "sector") if args.mode == "sector"
                  else verify_dual_store(data))
    except VerificationError as error:
        raise SystemExit(f"Pairing-store verification failed: {error}")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
