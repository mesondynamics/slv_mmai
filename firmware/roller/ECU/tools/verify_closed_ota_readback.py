#!/usr/bin/env python3
"""Strictly audit authenticated DA primary readbacks after CLOSED OTA.

The immutable reviewed 1.0.16, 1.0.17, and 1.0.18 release profiles are
retained.  The CLI defaults to the current 1.0.18 release; an explicit older
release directory continues to reproduce its historical post-OTA audit.

CubeProgrammer may fail a single 320 KiB NonSecure upload.  Each readback is
therefore supplied as one or more explicitly addressed, contiguous segments:

  --secure-segment 0x0C030000=secure.bin
  --nonsecure-segment 0x08100000=ns-0.bin
  --nonsecure-segment 0x08128000=ns-1.bin
  --nonsecure-segment 0x08138000=ns-2.bin

The verifier is offline.  It never invokes CubeProgrammer or accesses the ECU.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import struct
import sys
import zipfile
from dataclasses import asdict, dataclass
from pathlib import Path
from types import MappingProxyType
from typing import Any


PROJECT = Path(__file__).resolve().parents[1]
LAYOUT_HEADER = PROJECT / "Shared" / "ecu_flash_layout.h"


def _layout_value(name: str) -> int:
    text = LAYOUT_HEADER.read_text(encoding="utf-8")
    match = re.search(
        rf"^#define\s+{re.escape(name)}\s+(0x[0-9A-Fa-f]+|[0-9]+)\s*$",
        text,
        re.MULTILINE,
    )
    if match is None:
        raise RuntimeError(f"missing canonical Flash layout value: {name}")
    return int(match.group(1), 0)


FLASH_BASE_NS = _layout_value("ECU_FLASH_BASE_NS")
FLASH_BASE_S = _layout_value("ECU_FLASH_BASE_S")
SECURE_OFFSET = _layout_value("ECU_SECURE_PRIMARY_OFFSET")
SECURE_SIZE = _layout_value("ECU_SECURE_PRIMARY_SIZE")
NONSECURE_OFFSET = _layout_value("ECU_NONSECURE_PRIMARY_OFFSET")
NONSECURE_SIZE = _layout_value("ECU_NONSECURE_PRIMARY_SIZE")
SCRATCH_SIZE = _layout_value("ECU_SCRATCH_SIZE")
HEADER_SIZE = _layout_value("ECU_MCUBOOT_HEADER_SIZE")
RESERVED_TRAILER_SIZE = _layout_value("ECU_MCUBOOT_TRAILER_SIZE")
LAYOUT_VERSION = _layout_value("ECU_LAYOUT_VERSION")
SECURE_ADDRESS = FLASH_BASE_S + SECURE_OFFSET
NONSECURE_ADDRESS = FLASH_BASE_NS + NONSECURE_OFFSET


@dataclass(frozen=True)
class ReleaseProfile:
    """Immutable, reviewed release and pre-swap reference identity."""

    version: str
    identity: tuple[int, int, int, int, int]
    update_sequence: int
    secure_update_sha256: str
    nonsecure_update_sha256: str
    secure_key_area_sha256: str
    nonsecure_key_area_sha256: str
    previous_version: str
    previous_identity: tuple[int, int, int, int, int]
    previous_update_sequence: int
    previous_secure_update_sha256: str
    previous_nonsecure_update_sha256: str
    previous_artifact: tuple[str, int, str]
    artifacts: tuple[tuple[str, int, str], ...]

    def update_hash(self, image: str) -> str:
        if image == "secure":
            return self.secure_update_sha256
        if image == "nonsecure":
            return self.nonsecure_update_sha256
        raise ReadbackVerificationError(f"unknown image in release profile: {image}")

    def previous_update_hash(self, image: str) -> str:
        if image == "secure":
            return self.previous_secure_update_sha256
        if image == "nonsecure":
            return self.previous_nonsecure_update_sha256
        raise ReadbackVerificationError(f"unknown image in release profile: {image}")

    def key_area_hash(self, image: str) -> str:
        if image == "secure":
            return self.secure_key_area_sha256
        if image == "nonsecure":
            return self.nonsecure_key_area_sha256
        raise ReadbackVerificationError(f"unknown image in release profile: {image}")

    def artifact_map(self) -> dict[str, tuple[int, str]]:
        return {
            name: (size, digest) for name, size, digest in self.artifacts
        }

    def expected_metadata(self) -> dict[str, object]:
        return {
            "format": "roller-ecu-ota-v1",
            "version": self.version,
            "security_counter": self.identity[4],
            "update_sequence": self.update_sequence,
            "layout_version": LAYOUT_VERSION,
            "secure_sha256": self.secure_update_sha256,
            "nonsecure_sha256": self.nonsecure_update_sha256,
            "secure_size": SECURE_SIZE,
            "nonsecure_size": NONSECURE_SIZE,
        }

EXPECTED_VERSION = "1.0.16"
EXPECTED_IDENTITY = (1, 0, 16, 0, 16)
EXPECTED_UPDATE_SEQUENCE = 16
EXPECTED_UPDATE_HASHES = {
    "secure": "76922f64c58f42ef2c356dce143f7e567655af2b9e73352a016f06b46ba7318f",
    "nonsecure": "a3505eaac811dd8797dc0062f4b3089b2b2cdd01b0dea4c967eee67a0a5ccc39",
}
# MCUboot scratch-swap retains two plaintext AES key slots in the primary
# trailer: current 1.0.16 followed by previous 1.0.15.  Pin only the combined
# high-entropy area hash so the audit is exact without disclosing either key.
EXPECTED_ENCRYPTED_KEY_AREA_HASHES = {
    "secure": "41682bc8bc9fc73699b3524dedeffcbc64615aa83f65e32c0271f09c47000118",
    "nonsecure": "b2dc2f05cccc69858bc54cc59f891f662c2ac7ef7a1c455b33089ed1196aa257",
}
PREVIOUS_VERSION = "1.0.15"
PREVIOUS_IDENTITY = (1, 0, 15, 0, 15)
PREVIOUS_UPDATE_SEQUENCE = 15
PREVIOUS_UPDATE_HASHES = {
    "secure": "5808b47be3a24e690066c641957339f1ceb0bf9c14c60bff49a494d54ff5cbd2",
    "nonsecure": "76b1ab5ba2e7d19366c78621ac09da3689d80c9bc20d332785e6d5bb7df12293",
}
PINNED_PREVIOUS_RELEASE_ARTIFACT = (
    "roller-ecu-1.0.15.recu",
    525371,
    "9980acbf248f242c77c7044f48d627ca862d6714221f5835c0a21b7a24bf9e54",
)
PINNED_RELEASE_ARTIFACTS = {
    "metadata.json": (
        367,
        "56bc55c9eff5427fbd5cdfedfba4c8e2918e5a6e51b6daa1751d0dd2c9d265bb",
    ),
    "secure-initial.bin": (
        SECURE_SIZE,
        "06fd682381b6b4b45ac7b81f74ecaf724af91c8d9c2ef103cf1628cc76970270",
    ),
    "nonsecure-initial.bin": (
        NONSECURE_SIZE,
        "6471e4c47a79a9843e32bd3d3936daea0b4c5c7cea62f33c1e63dca73b508404",
    ),
    "roller-ecu-1.0.16.recu": (
        525371,
        "3e5ab07c7b6127dcedb46a1b4b1a695740904dd940d7128631f714d959a0b623",
    ),
}
EXPECTED_METADATA = {
    "format": "roller-ecu-ota-v1",
    "version": EXPECTED_VERSION,
    "security_counter": EXPECTED_IDENTITY[4],
    "update_sequence": EXPECTED_UPDATE_SEQUENCE,
    "layout_version": LAYOUT_VERSION,
    "secure_sha256": EXPECTED_UPDATE_HASHES["secure"],
    "nonsecure_sha256": EXPECTED_UPDATE_HASHES["nonsecure"],
    "secure_size": SECURE_SIZE,
    "nonsecure_size": NONSECURE_SIZE,
}
DEFAULT_CLI_VERSION = "1.0.18"
RELEASE_PROFILES = MappingProxyType({
    "1.0.16": ReleaseProfile(
        version="1.0.16",
        identity=EXPECTED_IDENTITY,
        update_sequence=EXPECTED_UPDATE_SEQUENCE,
        secure_update_sha256=EXPECTED_UPDATE_HASHES["secure"],
        nonsecure_update_sha256=EXPECTED_UPDATE_HASHES["nonsecure"],
        secure_key_area_sha256=EXPECTED_ENCRYPTED_KEY_AREA_HASHES["secure"],
        nonsecure_key_area_sha256=EXPECTED_ENCRYPTED_KEY_AREA_HASHES["nonsecure"],
        previous_version=PREVIOUS_VERSION,
        previous_identity=PREVIOUS_IDENTITY,
        previous_update_sequence=PREVIOUS_UPDATE_SEQUENCE,
        previous_secure_update_sha256=PREVIOUS_UPDATE_HASHES["secure"],
        previous_nonsecure_update_sha256=PREVIOUS_UPDATE_HASHES["nonsecure"],
        previous_artifact=PINNED_PREVIOUS_RELEASE_ARTIFACT,
        artifacts=tuple(
            (name, size, digest)
            for name, (size, digest) in PINNED_RELEASE_ARTIFACTS.items()
        ),
    ),
    "1.0.17": ReleaseProfile(
        version="1.0.17",
        identity=(1, 0, 17, 0, 17),
        update_sequence=17,
        secure_update_sha256=
            "974d08e78a4f95b22c20d3652656c8bae107883247e241992018ce528cfad2ad",
        nonsecure_update_sha256=
            "d09255f90f2f26ea5992927620fb6a8fbf84b0924df1b6c2cffdeae0ee44a2a0",
        # Exact SHA-256 of the two plaintext 16-byte MCUboot key slots in
        # post-swap primary order: current 1.0.17, then previous 1.0.16.
        # Only the combined hashes are retained; no AES key is disclosed.
        secure_key_area_sha256=
            "6b7f2423e3c2007d6bd592252cc086c46039addcd3c299325e027afefd3b3567",
        nonsecure_key_area_sha256=
            "9f507fd23e07b0c8166c403539dcfecba6baf1d349d1d1cf4e1eb7ced368840f",
        previous_version="1.0.16",
        previous_identity=(1, 0, 16, 0, 16),
        previous_update_sequence=16,
        previous_secure_update_sha256=
            "76922f64c58f42ef2c356dce143f7e567655af2b9e73352a016f06b46ba7318f",
        previous_nonsecure_update_sha256=
            "a3505eaac811dd8797dc0062f4b3089b2b2cdd01b0dea4c967eee67a0a5ccc39",
        previous_artifact=(
            "roller-ecu-1.0.16.recu",
            525371,
            "3e5ab07c7b6127dcedb46a1b4b1a695740904dd940d7128631f714d959a0b623",
        ),
        artifacts=(
            ("metadata.json",
                367,
                "0dd2ff18cf34dc0b61b629248dd2c3c83a8abf54867cae4af5c5c481d9c70113",
            ),
            ("secure-initial.bin",
                SECURE_SIZE,
                "0d8f2caf88fff8eac65cf0f66e27d23dee1c3aaecac54eaa0d763cc14288c74e",
            ),
            ("nonsecure-initial.bin",
                NONSECURE_SIZE,
                "39c9b079d8148ccd1a813f44198595503ec209b965ef250137add7a08c1bc0b1",
            ),
            ("roller-ecu-1.0.17.recu",
                525371,
                "75948795f39d898795b85841e4ad2e06c47738240184ff9ffbdc729fc11ab599",
            ),
        ),
    ),
    "1.0.18": ReleaseProfile(
        version="1.0.18",
        identity=(1, 0, 18, 0, 18),
        update_sequence=18,
        secure_update_sha256=
            "c2ac8e07a3518bf8c1bd92597c1237fc6c2f2aa48d9db1d450275db3d06f597d",
        nonsecure_update_sha256=
            "35bba730ddb62e2909576537aaca0f32c9aebce505c3dad9a8734d9e3a6a20fd",
        # Exact SHA-256 of the two plaintext 16-byte MCUboot key slots in
        # post-swap primary order: current 1.0.18, then previous 1.0.17.
        # Only the combined hashes are retained; no AES key is disclosed.
        secure_key_area_sha256=
            "22636578e9f23bfcdcf038c73cba01e212b80819d88864f9b18d9fd8a9caa992",
        nonsecure_key_area_sha256=
            "4ce45fa0e4e4db9224ffc65a5ef391825a09fdcc53af16bb05c06b2367e23bc2",
        previous_version="1.0.17",
        previous_identity=(1, 0, 17, 0, 17),
        previous_update_sequence=17,
        previous_secure_update_sha256=
            "974d08e78a4f95b22c20d3652656c8bae107883247e241992018ce528cfad2ad",
        previous_nonsecure_update_sha256=
            "d09255f90f2f26ea5992927620fb6a8fbf84b0924df1b6c2cffdeae0ee44a2a0",
        previous_artifact=(
            "roller-ecu-1.0.17.recu",
            525371,
            "75948795f39d898795b85841e4ad2e06c47738240184ff9ffbdc729fc11ab599",
        ),
        artifacts=(
            ("metadata.json",
                367,
                "cece519ce6b67edee6187b71135199a6dcddac866f3c40f26d9e60b9d1c9591e",
            ),
            ("secure-initial.bin",
                SECURE_SIZE,
                "5393702f93134397e875aac8719249f897b9a7397acf3d2c604a8397c3a32179",
            ),
            ("nonsecure-initial.bin",
                NONSECURE_SIZE,
                "cdba7492e91db1bd38fe0fa133a1d7a0cf6c7fdbfe6d4d257a5005c47b8ae175",
            ),
            ("roller-ecu-1.0.18.recu",
                525371,
                "4586f106b91fe91f293643a356105c26bf3af74161ac28034fc3494f999a517a",
            ),
        ),
    ),
})

IMAGE_MAGIC = 0x96F3B83D
IMAGE_F_ENCRYPTED = 0x4
PROTECTED_TLV_MAGIC = 0x6908
UNPROTECTED_TLV_MAGIC = 0x6907
SECURITY_COUNTER_TLV = 0x50
DEPENDENCY_TLV = 0x40
SHA256_TLV = 0x10
KEYHASH_TLV = 0x01
ECDSA256_TLV = 0x22
ENCRYPTION_EC256_TLV = 0x32
BOOT_MAGIC = bytes.fromhex("77c295f360d2ef7f3552500f2cb67980")
BOOT_MAX_ALIGN = 16
BOOT_ENC_KEY_SIZE = 16
BOOT_STATUS_STATE_COUNT = 3
BOOT_STATUS_MAX_ENTRIES = (
    max(SECURE_SIZE, NONSECURE_SIZE) + SCRATCH_SIZE - 1
) // SCRATCH_SIZE
BOOT_TRAILER_SIZE = (
    BOOT_STATUS_MAX_ENTRIES * BOOT_STATUS_STATE_COUNT * BOOT_MAX_ALIGN
    + 2 * BOOT_ENC_KEY_SIZE
    + 4 * BOOT_MAX_ALIGN
    + len(BOOT_MAGIC)
)
IMAGE_OK_OFFSET_FROM_END = len(BOOT_MAGIC) + BOOT_MAX_ALIGN
COPY_DONE_OFFSET_FROM_END = IMAGE_OK_OFFSET_FROM_END + BOOT_MAX_ALIGN
SWAP_INFO_OFFSET_FROM_END = COPY_DONE_OFFSET_FROM_END + BOOT_MAX_ALIGN
SWAP_SIZE_OFFSET_FROM_END = SWAP_INFO_OFFSET_FROM_END + BOOT_MAX_ALIGN
MANIFEST_FORMAT = "<12I32s32s4I"
MANIFEST_MAGIC = 0x31544F52
MANIFEST_SCHEMA = 1
OTA_FLAGS = 0x3


class ReadbackVerificationError(ValueError):
    """The release reference or authenticated primary readback is not exact."""


@dataclass(frozen=True)
class ReleaseIdentity:
    major: int
    minor: int
    revision: int
    build: int
    security_counter: int


@dataclass(frozen=True)
class ImageRecord:
    header_size: int
    image_size: int
    protected_size: int
    record_end: int
    flags: int
    identity: ReleaseIdentity


@dataclass(frozen=True)
class ReleaseImage:
    name: str
    release_version: str
    address: int
    size: int
    image_index: int
    initial: bytes
    update: bytes
    initial_record: ImageRecord
    update_record: ImageRecord
    previous_record_size: int
    swap_size: int
    encrypted_key_area_sha256: str


@dataclass(frozen=True)
class ReleaseArtifacts:
    release_dir: Path
    profile: ReleaseProfile
    metadata: dict[str, Any]
    secure: ReleaseImage
    nonsecure: ReleaseImage


@dataclass(frozen=True)
class InstalledImageReport:
    name: str
    address: int
    size: int
    sha256: str
    identity: ReleaseIdentity
    header_size: int
    image_size: int
    image_record_size: int
    image_record_sha256: str
    payload_sha256: str
    image_ok: int
    copy_done: int
    swap_info: int
    swap_size: int
    swap_status_entries: int
    encrypted_key_slots_present: int
    encrypted_key_area_sha256: str


@dataclass(frozen=True)
class SegmentArgument:
    address: int
    path: Path


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ReadbackVerificationError(message)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _program_unit(value: int) -> bytes:
    return bytes((value,)) + b"\xff" * (BOOT_MAX_ALIGN - 1)


def _strict_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for name, value in pairs:
        if name in result:
            raise ValueError(f"duplicate JSON field: {name}")
        result[name] = value
    return result


def _read_real_file(path: Path, description: str) -> bytes:
    _require(path.is_file() and not path.is_symlink(),
             f"{description} is not a real regular file: {path}")
    try:
        return path.read_bytes()
    except OSError as exc:
        raise ReadbackVerificationError(
            f"cannot read {description} {path}: {exc}"
        ) from exc


def _parse_tlv_area(name: str, data: bytes, offset: int, expected_magic: int) \
        -> tuple[list[tuple[int, int]], int]:
    _require(offset + 4 <= len(data), f"{name}: TLV information header is missing")
    magic, total = struct.unpack_from("<HH", data, offset)
    _require(magic == expected_magic, f"{name}: TLV information magic is invalid")
    _require(total >= 4 and offset + total <= len(data),
             f"{name}: TLV area exceeds the slot")
    entries: list[tuple[int, int]] = []
    cursor = offset + 4
    end = offset + total
    while cursor < end:
        _require(cursor + 4 <= end, f"{name}: truncated TLV header")
        tlv_type, tlv_length = struct.unpack_from("<HH", data, cursor)
        cursor += 4
        _require(cursor + tlv_length <= end, f"{name}: TLV value exceeds its area")
        entries.append((tlv_type, tlv_length))
        cursor += tlv_length
    _require(cursor == end, f"{name}: malformed TLV area")
    return entries, end


def _parse_image_record(name: str, data: bytes, expected_size: int,
                        expected_flags: int,
                        expected_identity: ReleaseIdentity |
                        tuple[int, int, int, int, int] |
                        None = None,
                        expected_version: str | None = None) -> ImageRecord:
    if expected_identity is None:
        expected_identity = EXPECTED_IDENTITY
    if expected_version is None:
        expected_version = EXPECTED_VERSION
    expected_release_identity = expected_identity if isinstance(
        expected_identity, ReleaseIdentity
    ) else ReleaseIdentity(*expected_identity)
    _require(len(data) == expected_size, f"{name}: wrong slot size")
    _require(len(data) >= 32, f"{name}: truncated MCUboot header")
    magic, load_address, header_size, protected_size, image_size, flags = \
        struct.unpack_from("<IIHHII", data, 0)
    version = struct.unpack_from("<BBHI", data, 20)
    _require(magic == IMAGE_MAGIC, f"{name}: invalid MCUboot image magic")
    _require(load_address == 0, f"{name}: load address is not canonical")
    _require(header_size == HEADER_SIZE, f"{name}: MCUboot header size is not canonical")
    _require(flags == expected_flags, f"{name}: MCUboot flags are not exact")
    payload_end = header_size + image_size
    _require(payload_end <= expected_size - RESERVED_TRAILER_SIZE,
             f"{name}: payload overlaps the reserved MCUboot trailer")
    protected_entries, protected_end = _parse_tlv_area(
        name, data, payload_end, PROTECTED_TLV_MAGIC)
    _require(protected_end - payload_end == protected_size,
             f"{name}: protected TLV length disagrees with the signed header")
    _require(protected_entries == [(SECURITY_COUNTER_TLV, 4), (DEPENDENCY_TLV, 12)],
             f"{name}: protected TLV layout is not canonical")
    security_counter = struct.unpack_from("<I", data, payload_end + 8)[0]
    identity = ReleaseIdentity(*version, security_counter)
    _require(identity == expected_release_identity,
             f"{name}: version/security counter is not exact "
             f"{expected_version}/{expected_release_identity.security_counter}")

    unprotected_entries, record_end = _parse_tlv_area(
        name, data, protected_end, UNPROTECTED_TLV_MAGIC)
    expected_entries = [(SHA256_TLV, 32), (KEYHASH_TLV, 32)]
    _require(len(unprotected_entries) in (3, 4),
             f"{name}: unprotected TLV count is not canonical")
    _require(unprotected_entries[:2] == expected_entries and
             unprotected_entries[2][0] == ECDSA256_TLV and
             64 <= unprotected_entries[2][1] <= 72,
             f"{name}: signed authentication TLV layout is not canonical")
    if expected_flags == 0:
        _require(len(unprotected_entries) == 3,
                 f"{name}: plaintext initial image contains an encryption TLV")
    else:
        _require(len(unprotected_entries) == 4 and
                 unprotected_entries[3] == (ENCRYPTION_EC256_TLV, 113),
                 f"{name}: encrypted OTA image has no exact EC256 key TLV")
    _require(record_end <= expected_size - RESERVED_TRAILER_SIZE,
             f"{name}: signed image record overlaps the reserved trailer")
    return ImageRecord(
        header_size=header_size,
        image_size=image_size,
        protected_size=protected_size,
        record_end=record_end,
        flags=flags,
        identity=identity,
    )


def _validate_reference_trailer(name: str, data: bytes, *, confirmed: bool,
                                record_end: int) -> None:
    image_ok = _program_unit(1) if confirmed else b"\xff" * BOOT_MAX_ALIGN
    _require(data[record_end:-IMAGE_OK_OFFSET_FROM_END] ==
             b"\xff" * (len(data) - IMAGE_OK_OFFSET_FROM_END - record_end),
             f"{name}: reference padding/trailer state is not canonical")
    _require(data[-IMAGE_OK_OFFSET_FROM_END:
                  -IMAGE_OK_OFFSET_FROM_END + BOOT_MAX_ALIGN] == image_ok,
             f"{name}: reference image_ok state is not canonical")
    _require(data[-len(BOOT_MAGIC):] == BOOT_MAGIC,
             f"{name}: reference boot magic is not GOOD")


def _load_release_image(name: str, address: int, size: int, image_index: int,
                        initial: bytes, update: bytes,
                        previous_record_size: int,
                        encrypted_key_area_sha256: str,
                        expected_identity: tuple[int, int, int, int, int] =
                        EXPECTED_IDENTITY,
                        expected_version: str = EXPECTED_VERSION) -> ReleaseImage:
    initial_record = _parse_image_record(
        f"{name}-initial", initial, size, 0,
        expected_identity, expected_version)
    update_record = _parse_image_record(
        f"{name}-encrypted-update", update, size, IMAGE_F_ENCRYPTED,
        expected_identity, expected_version)
    _require(
        (initial_record.header_size, initial_record.image_size,
         initial_record.protected_size, initial_record.identity) ==
        (update_record.header_size, update_record.image_size,
         update_record.protected_size, update_record.identity),
        f"{name}: initial/update geometry or identity differs",
    )
    _validate_reference_trailer(
        f"{name}-initial", initial, confirmed=True,
        record_end=initial_record.record_end)
    _validate_reference_trailer(
        f"{name}-encrypted-update", update, confirmed=False,
        record_end=update_record.record_end)
    _require(0 < previous_record_size <= size - RESERVED_TRAILER_SIZE,
             f"{name}: previous release record geometry is invalid")

    # boot_swap_image() in the reviewed generated OEMiROT loader reads both
    # slots with boot_read_image_size(), then persists
    # max(primary_record_size, secondary_record_size) as bs->swap_size.
    # Consequently swap_size is not always the newly installed record_end.
    swap_size = max(previous_record_size, update_record.record_end)
    return ReleaseImage(
        name=name,
        release_version=expected_version,
        address=address,
        size=size,
        image_index=image_index,
        initial=initial,
        update=update,
        initial_record=initial_record,
        update_record=update_record,
        previous_record_size=previous_record_size,
        swap_size=swap_size,
        encrypted_key_area_sha256=encrypted_key_area_sha256,
    )


def _load_previous_release_records(release_history: Path,
                                   profile: ReleaseProfile) \
        -> tuple[ImageRecord, ImageRecord]:
    """Parse one profile's exact pre-swap encrypted records."""
    filename, expected_size, expected_hash = profile.previous_artifact
    previous_dir = release_history / profile.previous_version
    _require(previous_dir.is_dir() and not previous_dir.is_symlink(),
             f"previous release directory is not a real "
             f"{profile.previous_version} directory")
    package = _read_real_file(previous_dir / filename,
                              f"previous release artifact {filename}")
    _require(len(package) == expected_size and _sha256(package) == expected_hash,
             f"previous swap reference is not the reviewed "
             f"{profile.previous_version} package")

    try:
        with zipfile.ZipFile(io.BytesIO(package)) as archive:
            infos = archive.infolist()
            names = [info.filename for info in infos]
            expected_members = {
                "metadata.json", "manifest.bin", "signature.bin",
                "secure.bin", "nonsecure.bin",
            }
            _require(len(names) == len(set(names)) and set(names) == expected_members,
                     "previous OTA package member set is not canonical")
            for info in infos:
                _require(info.compress_type == zipfile.ZIP_STORED and
                         info.flag_bits == 0 and not info.is_dir(),
                         f"previous OTA package member encoding is not canonical: "
                         f"{info.filename}")
            metadata_bytes = archive.read("metadata.json")
            manifest = archive.read("manifest.bin")
            signature = archive.read("signature.bin")
            secure_update = archive.read("secure.bin")
            nonsecure_update = archive.read("nonsecure.bin")
    except (OSError, zipfile.BadZipFile, KeyError) as exc:
        raise ReadbackVerificationError(
            "reviewed previous OTA package cannot be parsed"
        ) from exc

    try:
        metadata = json.loads(
            metadata_bytes.decode("utf-8"), object_pairs_hook=_strict_json_object)
    except (UnicodeError, ValueError) as exc:
        raise ReadbackVerificationError("previous release metadata is invalid") from exc
    expected_metadata = {
        "format": "roller-ecu-ota-v1",
        "version": profile.previous_version,
        "security_counter": profile.previous_identity[4],
        "update_sequence": profile.previous_update_sequence,
        "layout_version": LAYOUT_VERSION,
        "secure_sha256": profile.previous_secure_update_sha256,
        "nonsecure_sha256": profile.previous_nonsecure_update_sha256,
        "secure_size": SECURE_SIZE,
        "nonsecure_size": NONSECURE_SIZE,
    }
    _require(type(metadata) is dict and metadata == expected_metadata,
             "previous release metadata fields are not exact")
    _require(len(manifest) == struct.calcsize(MANIFEST_FORMAT),
             "previous OTA manifest has the wrong size")
    fields = struct.unpack(MANIFEST_FORMAT, manifest)
    expected_prefix = (
        MANIFEST_MAGIC, MANIFEST_SCHEMA, LAYOUT_VERSION,
        profile.previous_update_sequence, *profile.previous_identity[:4],
        profile.previous_identity[4], OTA_FLAGS, SECURE_SIZE, NONSECURE_SIZE,
    )
    _require(fields[:12] == expected_prefix and
             fields[12] == bytes.fromhex(
                 profile.previous_secure_update_sha256) and
             fields[13] == bytes.fromhex(
                 profile.previous_nonsecure_update_sha256) and
             fields[14:] == (0, 0, 0, 0),
             "previous OTA manifest identity/hashes are not exact")
    _require(len(signature) == 64 and signature != b"\x00" * 64,
             "previous OTA transport signature is missing or malformed")
    _require(len(secure_update) == SECURE_SIZE and
             _sha256(secure_update) ==
             profile.previous_secure_update_sha256 and
             len(nonsecure_update) == NONSECURE_SIZE and
             _sha256(nonsecure_update) ==
             profile.previous_nonsecure_update_sha256,
             "previous OTA package image members differ from metadata")

    secure = _parse_image_record(
        "secure-previous-encrypted-update", secure_update, SECURE_SIZE,
        IMAGE_F_ENCRYPTED, profile.previous_identity,
        profile.previous_version)
    nonsecure = _parse_image_record(
        "nonsecure-previous-encrypted-update", nonsecure_update, NONSECURE_SIZE,
        IMAGE_F_ENCRYPTED, profile.previous_identity,
        profile.previous_version)
    _validate_reference_trailer(
        "secure-previous-encrypted-update", secure_update, confirmed=False,
        record_end=secure.record_end)
    _validate_reference_trailer(
        "nonsecure-previous-encrypted-update", nonsecure_update, confirmed=False,
        record_end=nonsecure.record_end)
    return secure, nonsecure


def load_release(release_dir: Path) -> ReleaseArtifacts:
    """Load one immutable reviewed release and its exact previous reference."""
    profile = RELEASE_PROFILES.get(release_dir.name)
    _require(profile is not None,
             f"release profile is not reviewed: {release_dir.name}")
    _require(release_dir.name == profile.version and release_dir.is_dir() and
             not release_dir.is_symlink(),
             f"release directory must be a real {profile.version} directory")
    try:
        entries = {entry.name: entry for entry in release_dir.iterdir()}
    except OSError as exc:
        raise ReadbackVerificationError(
            f"cannot enumerate release directory {release_dir}: {exc}"
        ) from exc
    pinned_artifacts = profile.artifact_map()
    _require(set(entries) == set(pinned_artifacts),
             "release directory member set is not canonical")

    artifacts: dict[str, bytes] = {}
    for filename, (expected_size, expected_hash) in pinned_artifacts.items():
        data = _read_real_file(entries[filename], f"release artifact {filename}")
        _require(len(data) == expected_size and _sha256(data) == expected_hash,
                 f"release artifact is not the reviewed "
                 f"{profile.version}: {filename}")
        artifacts[filename] = data

    try:
        metadata = json.loads(
            artifacts["metadata.json"].decode("utf-8"),
            object_pairs_hook=_strict_json_object,
        )
    except (UnicodeError, ValueError) as exc:
        raise ReadbackVerificationError("release metadata is invalid") from exc
    _require(type(metadata) is dict and metadata == profile.expected_metadata(),
             "release metadata fields are not exact")

    package_name = f"roller-ecu-{profile.version}.recu"
    try:
        # Parse the exact byte string that already passed the pinned size/hash
        # gate. Reopening the path here would create a verification TOCTOU.
        with zipfile.ZipFile(io.BytesIO(artifacts[package_name])) as archive:
            infos = archive.infolist()
            names = [info.filename for info in infos]
            expected_members = {
                "metadata.json", "manifest.bin", "signature.bin",
                "secure.bin", "nonsecure.bin",
            }
            _require(len(names) == len(set(names)) and set(names) == expected_members,
                     "OTA package member set is not canonical")
            for info in infos:
                _require(info.compress_type == zipfile.ZIP_STORED and
                         info.flag_bits == 0 and not info.is_dir(),
                         f"OTA package member encoding is not canonical: {info.filename}")
            packaged_metadata = archive.read("metadata.json")
            manifest = archive.read("manifest.bin")
            signature = archive.read("signature.bin")
            secure_update = archive.read("secure.bin")
            nonsecure_update = archive.read("nonsecure.bin")
    except (OSError, zipfile.BadZipFile, KeyError) as exc:
        raise ReadbackVerificationError("reviewed OTA package cannot be parsed") from exc

    _require(packaged_metadata == artifacts["metadata.json"],
             "OTA package metadata differs from release metadata")
    _require(len(manifest) == struct.calcsize(MANIFEST_FORMAT),
             "OTA manifest has the wrong size")
    fields = struct.unpack(MANIFEST_FORMAT, manifest)
    expected_prefix = (
        MANIFEST_MAGIC, MANIFEST_SCHEMA, LAYOUT_VERSION,
        profile.update_sequence,
        *profile.identity[:4], profile.identity[4], OTA_FLAGS,
        SECURE_SIZE, NONSECURE_SIZE,
    )
    _require(fields[:12] == expected_prefix,
             "OTA manifest release identity/layout is not exact")
    _require(fields[12] == bytes.fromhex(profile.secure_update_sha256) and
             fields[13] == bytes.fromhex(
                 profile.nonsecure_update_sha256) and
             fields[14:] == (0, 0, 0, 0),
             "OTA manifest hashes/reserved fields are not exact")
    _require(len(signature) == 64 and signature != b"\x00" * 64,
             "OTA transport signature is missing or malformed")
    _require(len(secure_update) == SECURE_SIZE and
             _sha256(secure_update) == profile.secure_update_sha256 and
             len(nonsecure_update) == NONSECURE_SIZE and
             _sha256(nonsecure_update) == profile.nonsecure_update_sha256,
             "OTA package image members differ from metadata")

    previous_secure, previous_nonsecure = _load_previous_release_records(
        release_dir.parent, profile)
    secure = _load_release_image(
        "secure", SECURE_ADDRESS, SECURE_SIZE, 0,
        artifacts["secure-initial.bin"], secure_update,
        previous_secure.record_end,
        profile.secure_key_area_sha256,
        profile.identity, profile.version)
    nonsecure = _load_release_image(
        "nonsecure", NONSECURE_ADDRESS, NONSECURE_SIZE, 1,
        artifacts["nonsecure-initial.bin"], nonsecure_update,
        previous_nonsecure.record_end,
        profile.nonsecure_key_area_sha256,
        profile.identity, profile.version)
    _require(secure.initial_record.identity == nonsecure.initial_record.identity,
             "Secure/NonSecure release references have different identities")
    return ReleaseArtifacts(release_dir, profile, metadata, secure, nonsecure)


def assemble_segments(name: str, segments: list[tuple[int, bytes]],
                      expected_address: int, expected_size: int) -> bytes:
    """Assemble explicitly addressed segments; gaps, overlap and extras fail."""
    _require(bool(segments), f"{name}: no readback segments supplied")
    cursor = expected_address
    result = bytearray()
    end = expected_address + expected_size
    for index, (address, data) in enumerate(segments):
        _require(type(address) is int and type(data) is bytes,
                 f"{name}: segment {index} has an invalid type")
        _require(bool(data), f"{name}: segment {index} is empty")
        if address < cursor:
            raise ReadbackVerificationError(
                f"{name}: segment {index} overlaps or is out of order")
        _require(address == cursor,
                 f"{name}: gap before segment {index} at 0x{address:08X}")
        _require(address + len(data) <= end,
                 f"{name}: segment {index} extends past the primary slot")
        result.extend(data)
        cursor += len(data)
    _require(cursor == end,
             f"{name}: readback is truncated by {end - cursor} bytes")
    return bytes(result)


def _slice_unit(image: bytes, offset_from_end: int) -> bytes:
    return image[-offset_from_end:-offset_from_end + BOOT_MAX_ALIGN]


def validate_installed_image(image: bytes, reference: ReleaseImage) \
        -> InstalledImageReport:
    name = reference.name
    _require(len(image) == reference.size, f"{name}: wrong primary slot size")
    actual = _parse_image_record(
        f"{name}-installed", image, reference.size, IMAGE_F_ENCRYPTED,
        reference.update_record.identity, reference.release_version)
    update = reference.update_record
    _require(
        (actual.header_size, actual.image_size, actual.protected_size,
         actual.record_end, actual.identity) ==
        (update.header_size, update.image_size, update.protected_size,
         update.record_end, update.identity),
        f"{name}: installed record geometry/identity differs from the release",
    )

    header_size = actual.header_size
    payload_end = header_size + actual.image_size
    record_end = actual.record_end
    _require(image[:header_size] == reference.update[:header_size],
             f"{name}: installed header differs from the exact OTA header")
    _require(image[header_size:payload_end] ==
             reference.initial[header_size:payload_end],
             f"{name}: installed plaintext payload differs from the exact initial image")
    _require(image[payload_end:record_end] ==
             reference.update[payload_end:record_end],
             f"{name}: installed signed TLV record differs from the exact OTA image")

    trailer_start = reference.size - BOOT_TRAILER_SIZE
    _require(record_end <= trailer_start,
             f"{name}: installed record overlaps the MCUboot trailer")
    _require(image[record_end:trailer_start] == b"\xff" * (trailer_start - record_end),
             f"{name}: padding before the MCUboot trailer is not erased")

    swap_size = reference.swap_size
    status_entries = ((swap_size + SCRATCH_SIZE - 1) // SCRATCH_SIZE) * \
        BOOT_STATUS_STATE_COUNT
    total_status_entries = BOOT_STATUS_MAX_ENTRIES * BOOT_STATUS_STATE_COUNT
    _require(status_entries <= total_status_entries,
             f"{name}: swap status does not fit the canonical trailer")
    status_end = trailer_start + total_status_entries * BOOT_MAX_ALIGN
    status = image[trailer_start:status_end]
    for entry in range(total_status_entries):
        unit = status[entry * BOOT_MAX_ALIGN:(entry + 1) * BOOT_MAX_ALIGN]
        expected = _program_unit(entry % BOOT_STATUS_STATE_COUNT + 1) \
            if entry < status_entries else b"\xff" * BOOT_MAX_ALIGN
        _require(unit == expected,
                 f"{name}: MCUboot swap status entry {entry} is not canonical")

    key_area = image[status_end:status_end + 2 * BOOT_ENC_KEY_SIZE]
    _require(_sha256(key_area) == reference.encrypted_key_area_sha256,
             f"{name}: encrypted swap key area differs from the exact "
             f"{reference.release_version} post-swap state")

    expected_swap_info = (reference.image_index << 4) | 0x2
    _require(_slice_unit(image, SWAP_SIZE_OFFSET_FROM_END) ==
             struct.pack("<I", swap_size) + b"\xff" * 12,
             f"{name}: MCUboot swap_size is not the exact max(previous,current) "
             "record size")
    _require(_slice_unit(image, SWAP_INFO_OFFSET_FROM_END) ==
             _program_unit(expected_swap_info),
             f"{name}: MCUboot swap_info is not the canonical post-swap state")
    _require(_slice_unit(image, COPY_DONE_OFFSET_FROM_END) == _program_unit(1),
             f"{name}: MCUboot copy_done is not exactly 0x01")
    _require(_slice_unit(image, IMAGE_OK_OFFSET_FROM_END) == _program_unit(1),
             f"{name}: primary image_ok is not exactly 0x01")
    _require(image[-len(BOOT_MAGIC):] == BOOT_MAGIC,
             f"{name}: primary boot magic is not GOOD")

    return InstalledImageReport(
        name=name,
        address=reference.address,
        size=reference.size,
        sha256=_sha256(image),
        identity=actual.identity,
        header_size=header_size,
        image_size=actual.image_size,
        image_record_size=record_end,
        image_record_sha256=_sha256(image[:record_end]),
        payload_sha256=_sha256(image[header_size:payload_end]),
        image_ok=1,
        copy_done=1,
        swap_info=expected_swap_info,
        swap_size=swap_size,
        swap_status_entries=status_entries,
        encrypted_key_slots_present=2,
        encrypted_key_area_sha256=_sha256(key_area),
    )


def validate_pair(secure: bytes, nonsecure: bytes,
                  release: ReleaseArtifacts) \
        -> tuple[InstalledImageReport, InstalledImageReport]:
    secure_report = validate_installed_image(secure, release.secure)
    nonsecure_report = validate_installed_image(nonsecure, release.nonsecure)
    _require(secure_report.identity == nonsecure_report.identity,
             "Secure and NonSecure installed release identities differ")
    return secure_report, nonsecure_report


def parse_segment_argument(text: str) -> SegmentArgument:
    address_text, separator, path_text = text.partition("=")
    if not separator or not address_text or not path_text:
        raise argparse.ArgumentTypeError("segment must be ADDRESS=FILE")
    try:
        address = int(address_text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("segment address is invalid") from exc
    if address < 0 or address > 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("segment address is outside 32-bit space")
    return SegmentArgument(address, Path(path_text))


def load_segment_arguments(name: str, arguments: list[SegmentArgument],
                           expected_address: int, expected_size: int) \
        -> tuple[bytes, list[dict[str, Any]]]:
    raw_segments: list[tuple[int, bytes]] = []
    reports: list[dict[str, Any]] = []
    for index, argument in enumerate(arguments):
        data = _read_real_file(argument.path, f"{name} segment {index}")
        raw_segments.append((argument.address, data))
        reports.append({
            "address": argument.address,
            "size": len(data),
            "sha256": _sha256(data),
            "path": str(argument.path),
        })
    return (
        assemble_segments(name, raw_segments, expected_address, expected_size),
        reports,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--release-dir",
        type=Path,
        default=PROJECT / "artifacts" / "firmware" / DEFAULT_CLI_VERSION,
        help=(f"immutable reviewed release directory; default is "
              f"{DEFAULT_CLI_VERSION}"),
    )
    parser.add_argument(
        "--secure-segment", action="append", required=True,
        type=parse_segment_argument, metavar="ADDRESS=FILE",
    )
    parser.add_argument(
        "--nonsecure-segment", action="append", required=True,
        type=parse_segment_argument, metavar="ADDRESS=FILE",
    )
    parser.add_argument(
        "--report", type=Path,
        help="create a new JSON evidence report (an existing path is never overwritten)",
    )
    args = parser.parse_args()

    try:
        release = load_release(args.release_dir)
        secure, secure_segments = load_segment_arguments(
            "secure", args.secure_segment, SECURE_ADDRESS, SECURE_SIZE)
        nonsecure, nonsecure_segments = load_segment_arguments(
            "nonsecure", args.nonsecure_segment,
            NONSECURE_ADDRESS, NONSECURE_SIZE)
        secure_report, nonsecure_report = validate_pair(
            secure, nonsecure, release)
        report = {
            "schema": "roller-ecu-closed-ota-primary-readback-audit-v1",
            "release": {
                "version": release.profile.version,
                "security_counter": release.profile.identity[4],
                "update_sequence": release.profile.update_sequence,
                "layout_version": LAYOUT_VERSION,
                "artifacts": {
                    name: {"size": size, "sha256": digest}
                    for name, (size, digest) in
                    release.profile.artifact_map().items()
                },
                "previous_swap_reference": {
                    "version": release.profile.previous_version,
                    "artifact": {
                        "name": release.profile.previous_artifact[0],
                        "size": release.profile.previous_artifact[1],
                        "sha256": release.profile.previous_artifact[2],
                    },
                    "record_sizes": {
                        "secure": release.secure.previous_record_size,
                        "nonsecure": release.nonsecure.previous_record_size,
                    },
                },
            },
            "segments": {
                "secure": secure_segments,
                "nonsecure": nonsecure_segments,
            },
            "images": {
                "secure": asdict(secure_report),
                "nonsecure": asdict(nonsecure_report),
            },
        }
        if args.report is not None:
            _require(not args.report.exists() and not args.report.is_symlink(),
                     f"report path already exists: {args.report}")
            try:
                with args.report.open("x", encoding="utf-8") as output:
                    json.dump(report, output, indent=2, sort_keys=True)
                    output.write("\n")
            except OSError as exc:
                raise ReadbackVerificationError(
                    f"cannot create report {args.report}: {exc}"
                ) from exc
    except ReadbackVerificationError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    print(
        f"Exact CLOSED OTA primary pair verified: {release.profile.version}, "
        f"security_counter={release.profile.identity[4]}, image_ok=0x01/0x01"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
