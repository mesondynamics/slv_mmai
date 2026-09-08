#!/usr/bin/env python3
"""Offline, exact-image audit for reviewed new-board releases; no hardware writes.

The legacy 1.0.15 recovery validator is deliberately unchanged. This audit is
not authority to request CLOSED: it proves signed image contents, pairing,
and confirmation state, not physical cold boot or Debug Authentication.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

import ethernet_ota as ota
import manufacture_open_device as manufacturing
import verify_open_loader_backup as layout
from verify_pairing_store import verify_dual_store

RELEASES = {
    "1.0.21": {name: value for name, value in manufacturing.PINNED.items()
               if not name.startswith("Release")},
    "1.0.22": {
        "metadata.json": (367, "72e9400b9627449e3e1503fa94b1285c272576191bf9d813915656916adb64d5"),
        "secure-initial.bin": (196608, "0a61bdb0883dc4c9f875c184fa9a86f77211fdca8b48593673df8f27964c252d"),
        "nonsecure-initial.bin": (327680, "1ac0dfa50c0b93a86980e14a760b56408d4287e4866456016b57de0c01b9fd5c"),
        "roller-ecu-1.0.22.recu": (525371, "5c76215bcb30e959622aa7ab651a36213e01ba4a7bdadc9fa2bf56ccc15994b3"),
    },
}
require = manufacturing.require
digest = manufacturing.digest


def tlvs(image: bytes, offset: int, magic: int):
    require(offset + 4 <= len(image), "truncated TLV header")
    actual_magic, size = struct.unpack_from("<HH", image, offset)
    require(actual_magic == magic and size >= 4 and offset + size <= len(image), "invalid TLV area")
    fields, cursor = {}, offset + 4
    while cursor < offset + size:
        require(cursor + 4 <= offset + size, "truncated TLV entry")
        tag, length = struct.unpack_from("<HH", image, cursor)
        cursor += 4
        require(tag not in fields and cursor + length <= offset + size, "duplicate/oversized TLV")
        fields[tag] = image[cursor:cursor + length]
        cursor += length
    require(cursor == offset + size, "TLV end mismatch")
    return fields, cursor


def verify_record(image: bytes, name: str, version: str, public_key: bytes, *, encrypted: bool):
    size = 0x30000 if name == "secure" else 0x50000
    require(len(image) == size, "wrong image slot size")
    magic, load_address, header_size, protected_size, image_size, flags = struct.unpack_from("<IIHHII", image)
    require(magic == layout.IMAGE_MAGIC and load_address == 0 and header_size == 0x400 and
            flags == (4 if encrypted else 0) and image_size % 16 == 0 and
            0 < image_size < size - 0x2400, "unreviewed image header/geometry")
    major, minor, revision = map(int, version.split("."))
    require(struct.unpack_from("<BBHI", image, 20) == (major, minor, revision, 0), "image version mismatch")
    protected, protected_end = tlvs(image, header_size + image_size, 0x6908)
    require(protected_end == header_size + image_size + protected_size and
            set(protected) == {0x50, 0x40} and
            protected[0x50] == struct.pack("<I", revision) and
            protected[0x40] == struct.pack("<B3xBBHI", 1 if name == "secure" else 0,
                                          major, minor, revision, 0),
            "signed counter/dependency mismatch")
    fields, record_end = tlvs(image, protected_end, 0x6907)
    require(set(fields) == ({0x10, 0x01, 0x22, 0x32} if encrypted else {0x10, 0x01, 0x22}),
            "unreviewed signature/encryption TLV set")
    require(fields[0x10] == hashlib.sha256(image[:protected_end]).digest(), "signed image hash mismatch")
    key = serialization.load_pem_public_key(public_key)
    require(isinstance(key, ec.EllipticCurvePublicKey) and key.curve.name == "secp256r1", "not a P-256 key")
    der = key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    require(fields[0x01] == hashlib.sha256(der).digest(), "image public-key hash mismatch")
    key.verify(fields[0x22], image[:protected_end], ec.ECDSA(hashes.SHA256()))
    if encrypted:
        require(len(fields[0x32]) == 113, "invalid P-256 encryption TLV")
    return dict(header_size=header_size, image_size=image_size, protected_tlv_size=protected_size,
                record_size=record_end, record_sha256=digest(image[:record_end]),
                payload_sha256=digest(image[header_size:header_size + image_size]), flags=flags)


def load_release(root: Path, keys: Path):
    version = root.name
    require(version in RELEASES, "unreviewed new-board release")
    files = {}
    for name, expected in RELEASES[version].items():
        data = manufacturing.regular_bytes(root / name)
        require((len(data), digest(data)) == expected, f"changed reviewed release: {name}")
        files[name] = data
    _, secure, nonsecure, metadata = ota.load_package(root / f"roller-ecu-{version}.recu",
                                                     keys / "ota-transport-public.pem")
    require(metadata["version"] == version and
            metadata["security_counter"] == metadata["update_sequence"] == int(version.split(".")[-1]),
            "release manifest identity mismatch")
    result = {}
    for name, update, suffix in (("secure", secure, "s"), ("nonsecure", nonsecure, "ns")):
        initial = files[f"{name}-initial.bin"]
        public = manufacturing.regular_bytes(keys / f"oemirot-auth-{suffix}-public.pem")
        report = verify_record(initial, name, version, public, encrypted=False)
        layout.parse_primary(name, initial, 0x30000 if name == "secure" else 0x100000, len(initial))
        header, protected, size = struct.unpack_from("<HHI", update, 8)
        require((header, protected, size) == (report["header_size"], report["protected_tlv_size"],
                                            report["image_size"]), "initial/update geometry differs")
        # OTA signatures cover the encrypted-flag header but PLAINTEXT payload.
        # Reconstruct exactly what MCUboot must produce after decryption, then
        # independently verify its hash, dependency, counter, key and ECDSA.
        installed = bytearray(update)
        installed[header:header + size] = initial[header:header + size]
        installed_report = verify_record(bytes(installed), name, version, public, encrypted=True)
        result[name] = dict(initial=initial, installed_record=bytes(installed[:installed_report["record_size"]]),
                            initial_report=report, installed_report=installed_report)
    return result


def audit_installed_trailer(image: bytes, record_size: int, index: int):
    trailer_start = len(image) - layout.BOOT_TRAILER_SIZE
    require(record_size < trailer_start and all(x == 0xFF for x in image[record_size:trailer_start]),
            "non-erased data outside signed image/trailer")
    entries = ((record_size + layout.SCRATCH_SIZE - 1) // layout.SCRATCH_SIZE) * 3
    total_entries = layout.BOOT_STATUS_MAX_ENTRIES * 3
    require(entries <= total_entries, "oversized swap status")
    for entry in range(total_entries):
        start = trailer_start + entry * 16
        expected = bytes([entry % 3 + 1]) + b"\xff" * 15 if entry < entries else b"\xff" * 16
        require(image[start:start + 16] == expected, "incomplete/noncanonical swap status")
    for offset, expected in (
            (layout.SWAP_SIZE_OFFSET_FROM_END, struct.pack("<I", record_size) + b"\xff" * 12),
            (layout.SWAP_INFO_OFFSET_FROM_END, bytes([(index << 4) | 2]) + b"\xff" * 15),
            (layout.COPY_DONE_OFFSET_FROM_END, b"\x01" + b"\xff" * 15),
            (layout.IMAGE_OK_OFFSET_FROM_END, b"\x01" + b"\xff" * 15)):
        require(image[-offset:-offset + 16] == expected, "swap/confirmation trailer mismatch")
    require(image[-16:] == layout.BOOT_MAGIC, "invalid boot magic")
    start = trailer_start + total_entries * 16
    key_area = image[start:start + 32]
    # These rollback scratch keys depend on swap history, not only this
    # release. Preserve their digest in evidence; do not invent a known value.
    return dict(image_ok=1, copy_done=1, swap_status_entries=entries,
                swap_key_area_sha256=digest(key_area),
                swap_key_slots_present=sum(key_area[i:i + 16] != b"\xff" * 16 for i in (0, 16)))


def audit_flash(flash: bytes, release_dir: Path, keys: Path, mode: str, boot_profile: str):
    require(mode in ("initial", "installed") and boot_profile in ("ReleaseOpen", "ReleaseClosed"), "unknown audit mode")
    require(len(flash) == 0x200000, "full Flash must be 2 MiB")
    boot_size, boot_sha = manufacturing.PINNED[f"{boot_profile}-ECU_OEMiROT.bin"]
    require(digest(flash[:boot_size]) == boot_sha and
            all(x == 0xFF for x in flash[boot_size:0x20000]), "wrong board/profile bootloader")
    pairing = verify_dual_store(flash[0xE0000:0xE4000], identity=manufacturing.IDENTITY)
    release = load_release(release_dir, keys)
    reports = {}
    for index, name, offset, size in ((0, "secure", 0x30000, 0x30000),
                                      (1, "nonsecure", 0x100000, 0x50000)):
        image = flash[offset:offset + size]
        reference = release[name]
        if mode == "initial":
            require(image == reference["initial"], f"{name} is not exact signed initial image")
            report = dict(reference["initial_report"], image_ok=1)
        else:
            record = reference["installed_record"]
            require(image[:len(record)] == record, f"{name} installed signed record differs")
            report = dict(reference["installed_report"], **audit_installed_trailer(image, len(record), index))
        reports[name] = dict(report, slot_sha256=digest(image))
    return dict(schema="roller-ecu-device-image-audit-v1", result="PASS", device_serial=manufacturing.SERIAL,
                version=release_dir.name, mode=mode, boot_profile=boot_profile, boot_sha256=boot_sha,
                full_flash_sha256=digest(flash), pairing=pairing, images=reports)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-dir", type=Path, required=True)
    parser.add_argument("--keys-dir", type=Path, default=manufacturing.PKI)
    parser.add_argument("--flash", type=Path)
    parser.add_argument("--mode", choices=("initial", "installed"), default="initial")
    parser.add_argument("--boot-profile", choices=("ReleaseOpen", "ReleaseClosed"), default="ReleaseOpen")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.flash:
        report = audit_flash(manufacturing.regular_bytes(args.flash), args.release_dir, args.keys_dir,
                             args.mode, args.boot_profile)
    else:
        images = load_release(args.release_dir, args.keys_dir)
        report = dict(result="PASS", device_serial=manufacturing.SERIAL, version=args.release_dir.name,
                      images={name: {k: v for k, v in data.items() if k.endswith("report")}
                              for name, data in images.items()})
    if args.output:
        manufacturing.write_json(args.output, report)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
