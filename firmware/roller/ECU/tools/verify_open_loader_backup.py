#!/usr/bin/env python3
"""Validate the paired primary images captured before an OPEN loader update."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import zipfile
from dataclasses import asdict, dataclass
from pathlib import Path


LAYOUT_HEADER = Path(__file__).resolve().parents[1] / "Shared" / "ecu_flash_layout.h"


def _layout_value(name: str) -> int:
    text = LAYOUT_HEADER.read_text(encoding="utf-8")
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(0x[0-9A-Fa-f]+|[0-9]+)\s*$",
                      text, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"missing canonical Flash layout value: {name}")
    return int(match.group(1), 0)


FLASH_SIZE = _layout_value("ECU_FLASH_TOTAL_SIZE")
SECURE_OFFSET = _layout_value("ECU_SECURE_PRIMARY_OFFSET")
SECURE_SIZE = _layout_value("ECU_SECURE_PRIMARY_SIZE")
NONSECURE_OFFSET = _layout_value("ECU_NONSECURE_PRIMARY_OFFSET")
NONSECURE_SIZE = _layout_value("ECU_NONSECURE_PRIMARY_SIZE")
SCRATCH_SIZE = _layout_value("ECU_SCRATCH_SIZE")
IMAGE_MAGIC = 0x96F3B83D
IMAGE_F_ENCRYPTED = 0x4
PROTECTED_TLV_MAGIC = 0x6908
UNPROTECTED_TLV_MAGIC = 0x6907
SECURITY_COUNTER_TLV = 0x50
ENCRYPTION_EC256_TLV = 0x32
SHA256_TLV = 0x10
KEYHASH_TLV = 0x01
ECDSA256_TLV = 0x22
BOOT_MAGIC = bytes.fromhex("77c295f360d2ef7f3552500f2cb67980")
BOOT_MAX_ALIGN = 16
IMAGE_OK_OFFSET_FROM_END = len(BOOT_MAGIC) + BOOT_MAX_ALIGN
COPY_DONE_OFFSET_FROM_END = IMAGE_OK_OFFSET_FROM_END + BOOT_MAX_ALIGN
SWAP_INFO_OFFSET_FROM_END = COPY_DONE_OFFSET_FROM_END + BOOT_MAX_ALIGN
SWAP_SIZE_OFFSET_FROM_END = SWAP_INFO_OFFSET_FROM_END + BOOT_MAX_ALIGN
BOOT_ENC_KEY_SIZE = 16
BOOT_STATUS_STATE_COUNT = 3
BOOT_STATUS_MAX_ENTRIES = (max(SECURE_SIZE, NONSECURE_SIZE) + SCRATCH_SIZE - 1) // SCRATCH_SIZE
BOOT_TRAILER_SIZE = (
    BOOT_STATUS_MAX_ENTRIES * BOOT_STATUS_STATE_COUNT * BOOT_MAX_ALIGN
    + 2 * BOOT_ENC_KEY_SIZE
    + 4 * BOOT_MAX_ALIGN
    + len(BOOT_MAGIC)
)

# This mode is intentionally release-specific.  It is used only after the
# reviewed Ethernet OTA has completed its scratch swap.  The ordinary OPEN
# loader backup path below remains plaintext-only and cannot opt in to it.
PINNED_INSTALLED_IDENTITY = (1, 0, 15, 0, 15)
PINNED_RELEASE_ARTIFACTS = {
    "metadata.json": (367,
        "9bf210ad3faf82fa7ea55e8123bbcd43f95ee836d8939df64d36cae28dc44bc3"),
    "secure-initial.bin": (196608,
        "1a50b9106f798fb64c1a766d81d194b8829e07497941b4af7efe41bb38b98d75"),
    "nonsecure-initial.bin": (327680,
        "4f96d510f06cda3de1a88de8be5a7516b09f1ed539b05e0d6be2eeaaa571d8ed"),
    "roller-ecu-1.0.15.recu": (525371,
        "9980acbf248f242c77c7044f48d627ca862d6714221f5835c0a21b7a24bf9e54"),
}
PINNED_UPDATE_HASHES = {
    "secure": "5808b47be3a24e690066c641957339f1ceb0bf9c14c60bff49a494d54ff5cbd2",
    "nonsecure": "76b1ab5ba2e7d19366c78621ac09da3689d80c9bc20d332785e6d5bb7df12293",
}
PINNED_INSTALLED_RECORD_HASHES = {
    "secure": "7d163262f0e83425aee2a5da3aa647795d1ee5288748d8c7d3bda1960d0f66bf",
    "nonsecure": "649423a9bda226f57515c9f2835d4e607b8bb38431eec727af9d6954ab119397",
}
PINNED_PAYLOAD_HASHES = {
    "secure": "c74896f879273ed0c50fc32a189e6a89bd3fca05aa33627cc58469ae773ef6ea",
    "nonsecure": "42e870f58b68188c3a38e78970d72f11035b59ac26db4904f642997d11bc0973",
}
PINNED_ENCRYPTED_KEY_AREAS = {
    "secure": "f78bd41faeaaa41f02c212552188a68f"
              "d6ff0c2271909fa83994b3a0654cb3be",
    "nonsecure": "5cb7f790101bc4e403c89deebaaa757e"
                 "50117ef6344f8881a78d0e94026b2147",
}


class BackupValidationError(ValueError):
    """The captured Flash cannot be used for an in-place loader update."""


@dataclass(frozen=True)
class ReleaseIdentity:
    major: int
    minor: int
    revision: int
    build: int
    security_counter: int


@dataclass(frozen=True)
class ImageReport:
    name: str
    offset: int
    size: int
    header_size: int
    image_size: int
    protected_tlv_size: int
    identity: ReleaseIdentity
    image_ok: int


@dataclass(frozen=True)
class InstalledImageReport:
    name: str
    offset: int
    size: int
    header_size: int
    image_size: int
    protected_tlv_size: int
    identity: ReleaseIdentity
    flags: int
    image_record_size: int
    image_record_sha256: str
    payload_sha256: str
    swap_size: int
    swap_info: int
    copy_done: int
    image_ok: int
    swap_status_entries: int
    encrypted_key_slots_present: int
    encrypted_key_area_sha256: str


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise BackupValidationError(message)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _program_unit(value: int) -> bytes:
    return bytes((value,)) + b"\xff" * (BOOT_MAX_ALIGN - 1)


def _parse_protected_identity(name: str, image: bytes, header_size: int,
                              image_size: int, protected_size: int,
                              version: tuple[int, int, int, int]) \
        -> tuple[ReleaseIdentity, int]:
    protected_offset = header_size + image_size
    protected_end = protected_offset + protected_size
    _require(protected_size >= 4 and protected_end <= len(image),
             f"{name}: protected TLV is outside the slot")
    tlv_magic, tlv_total = struct.unpack_from("<HH", image, protected_offset)
    _require(tlv_magic == PROTECTED_TLV_MAGIC,
             f"{name}: protected TLV magic is invalid")
    _require(tlv_total == protected_size,
             f"{name}: protected TLV length disagrees with the signed header")

    counters: list[int] = []
    cursor = protected_offset + 4
    while cursor < protected_end:
        _require(cursor + 4 <= protected_end, f"{name}: truncated protected TLV")
        tlv_type, tlv_length = struct.unpack_from("<HH", image, cursor)
        cursor += 4
        _require(cursor + tlv_length <= protected_end,
                 f"{name}: protected TLV value exceeds its area")
        if tlv_type == SECURITY_COUNTER_TLV:
            _require(tlv_length == 4, f"{name}: invalid security counter length")
            counters.append(struct.unpack_from("<I", image, cursor)[0])
        cursor += tlv_length
    _require(cursor == protected_end, f"{name}: malformed protected TLV area")
    _require(len(counters) == 1,
             f"{name}: expected exactly one signed security counter")
    return ReleaseIdentity(*version, counters[0]), protected_end


def _parse_unprotected_end(name: str, image: bytes, offset: int) \
        -> tuple[int, int, int, int, int]:
    _require(offset + 4 <= len(image), f"{name}: unprotected TLV is missing")
    magic, total = struct.unpack_from("<HH", image, offset)
    _require(magic == UNPROTECTED_TLV_MAGIC,
             f"{name}: unprotected TLV magic is invalid")
    _require(total >= 4 and offset + total <= len(image),
             f"{name}: unprotected TLV is outside the slot")
    cursor = offset + 4
    encryption_tlvs = 0
    sha256_tlvs = 0
    keyhash_tlvs = 0
    signature_tlvs = 0
    while cursor < offset + total:
        _require(cursor + 4 <= offset + total,
                 f"{name}: truncated unprotected TLV")
        tlv_type, tlv_length = struct.unpack_from("<HH", image, cursor)
        cursor += 4
        _require(cursor + tlv_length <= offset + total,
                 f"{name}: unprotected TLV value exceeds its area")
        if tlv_type == ENCRYPTION_EC256_TLV:
            _require(tlv_length == 113,
                     f"{name}: EC256 encryption TLV has the wrong length")
            encryption_tlvs += 1
        elif tlv_type == SHA256_TLV:
            _require(tlv_length == 32, f"{name}: SHA-256 TLV has the wrong length")
            sha256_tlvs += 1
        elif tlv_type == KEYHASH_TLV:
            _require(tlv_length == 32, f"{name}: key hash TLV has the wrong length")
            keyhash_tlvs += 1
        elif tlv_type == ECDSA256_TLV:
            _require(64 <= tlv_length <= 72,
                     f"{name}: ECDSA-P256 TLV has the wrong length")
            signature_tlvs += 1
        cursor += tlv_length
    _require(cursor == offset + total, f"{name}: malformed unprotected TLV area")
    return (cursor, encryption_tlvs, sha256_tlvs,
            keyhash_tlvs, signature_tlvs)


def parse_primary(name: str, image: bytes, offset: int, expected_size: int) -> ImageReport:
    _require(len(image) == expected_size, f"{name}: wrong slot size")
    _require(len(image) >= 32, f"{name}: truncated MCUboot header")
    magic, _load_address, header_size, protected_size, image_size, flags = \
        struct.unpack_from("<IIHHII", image, 0)
    major, minor, revision, build = struct.unpack_from("<BBHI", image, 20)
    _require(magic == IMAGE_MAGIC, f"{name}: invalid MCUboot image magic")
    _require(header_size >= 32 and header_size <= len(image),
             f"{name}: invalid MCUboot header size")
    _require(flags & IMAGE_F_ENCRYPTED == 0,
             f"{name}: encrypted primary cannot be audited")
    identity, _protected_end = _parse_protected_identity(
        name, image, header_size, image_size, protected_size,
        (major, minor, revision, build))

    _require(image[-len(BOOT_MAGIC):] == BOOT_MAGIC,
             f"{name}: primary trailer boot magic is not GOOD")
    image_ok_block = image[-IMAGE_OK_OFFSET_FROM_END:
                           -IMAGE_OK_OFFSET_FROM_END + BOOT_MAX_ALIGN]
    _require(len(image_ok_block) == BOOT_MAX_ALIGN,
             f"{name}: truncated image_ok program unit")
    _require(image_ok_block[0] == 0x01 and
             image_ok_block[1:] == b"\xff" * (BOOT_MAX_ALIGN - 1),
             f"{name}: primary is not exactly confirmed (image_ok != 0x01)")

    return ImageReport(
        name=name,
        offset=offset,
        size=expected_size,
        header_size=header_size,
        image_size=image_size,
        protected_tlv_size=protected_size,
        identity=identity,
        image_ok=image_ok_block[0],
    )


def _load_pinned_release(release_dir: Path) -> tuple[bytes, bytes, bytes, bytes]:
    _require(release_dir.name == "1.0.15" and release_dir.is_dir() and
             not release_dir.is_symlink(),
             "installed audit requires the real pinned 1.0.15 release directory")
    artifacts: dict[str, bytes] = {}
    for name, (expected_size, expected_hash) in PINNED_RELEASE_ARTIFACTS.items():
        path = release_dir / name
        _require(path.is_file() and not path.is_symlink(),
                 f"installed audit release artifact is missing: {name}")
        data = path.read_bytes()
        _require(len(data) == expected_size and _sha256(data) == expected_hash,
                 f"installed audit release artifact is not reviewed: {name}")
        artifacts[name] = data

    try:
        metadata = json.loads(artifacts["metadata.json"])
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise BackupValidationError("pinned release metadata is invalid") from exc
    expected_metadata = {
        "format": "roller-ecu-ota-v1",
        "version": "1.0.15",
        "security_counter": 15,
        "update_sequence": 15,
        "layout_version": 65536,
        "secure_sha256": PINNED_UPDATE_HASHES["secure"],
        "nonsecure_sha256": PINNED_UPDATE_HASHES["nonsecure"],
        "secure_size": SECURE_SIZE,
        "nonsecure_size": NONSECURE_SIZE,
    }
    _require(type(metadata) is dict and metadata == expected_metadata,
             "pinned release metadata fields are not canonical")

    package_path = release_dir / "roller-ecu-1.0.15.recu"
    try:
        with zipfile.ZipFile(package_path) as archive:
            infos = archive.infolist()
            names = [info.filename for info in infos]
            _require(len(names) == len(set(names)),
                     "pinned OTA package has duplicate members")
            _require(set(names) == {"metadata.json", "manifest.bin", "signature.bin",
                                    "secure.bin", "nonsecure.bin"},
                     "pinned OTA package member set is not canonical")
            for info in infos:
                _require(info.compress_type == zipfile.ZIP_STORED and
                         info.flag_bits & 0x1 == 0,
                         f"pinned OTA package member is not plain ZIP_STORED: {info.filename}")
            _require(archive.read("metadata.json") == artifacts["metadata.json"],
                     "pinned OTA package metadata differs from release metadata")
            manifest = archive.read("manifest.bin")
            signature = archive.read("signature.bin")
            secure_update = archive.read("secure.bin")
            nonsecure_update = archive.read("nonsecure.bin")
    except (OSError, zipfile.BadZipFile, KeyError) as exc:
        raise BackupValidationError("pinned OTA package is invalid") from exc

    _require(len(manifest) == 128 and len(signature) == 64 and
             signature != b"\x00" * 64,
             "pinned OTA manifest or transport signature has the wrong size")
    fields = struct.unpack("<12I32s32s4I", manifest)
    _require(fields[:12] == (0x31544F52, 1, 65536, 15,
                             1, 0, 15, 0, 15, 3, SECURE_SIZE, NONSECURE_SIZE),
             "pinned OTA manifest identity is not exact 1.0.15/counter15/sequence15")
    _require(fields[12] == bytes.fromhex(PINNED_UPDATE_HASHES["secure"]) and
             fields[13] == bytes.fromhex(PINNED_UPDATE_HASHES["nonsecure"]) and
             fields[14:] == (0, 0, 0, 0),
             "pinned OTA manifest hashes or reserved fields differ")
    _require(len(secure_update) == SECURE_SIZE and
             _sha256(secure_update) == PINNED_UPDATE_HASHES["secure"] and
             len(nonsecure_update) == NONSECURE_SIZE and
             _sha256(nonsecure_update) == PINNED_UPDATE_HASHES["nonsecure"],
             "pinned OTA image members differ from the reviewed release")
    return (artifacts["secure-initial.bin"],
            artifacts["nonsecure-initial.bin"], secure_update, nonsecure_update)


def _validate_installed_primary(name: str, image: bytes, initial: bytes,
                                update: bytes, offset: int,
                                expected_size: int, image_index: int) \
        -> InstalledImageReport:
    _require(len(image) == expected_size, f"{name}: wrong slot size")
    _require(len(initial) == expected_size and len(update) == expected_size,
             f"{name}: pinned reference has the wrong slot size")
    magic, _load_address, header_size, protected_size, image_size, flags = \
        struct.unpack_from("<IIHHII", image, 0)
    major, minor, revision, build = struct.unpack_from("<BBHI", image, 20)
    _require(magic == IMAGE_MAGIC, f"{name}: invalid MCUboot image magic")
    _require(flags == IMAGE_F_ENCRYPTED,
             f"{name}: installed OTA header flags are not exactly 0x4")
    _require(header_size >= 32 and header_size + image_size <= expected_size,
             f"{name}: invalid installed MCUboot geometry")
    identity, protected_end = _parse_protected_identity(
        name, image, header_size, image_size, protected_size,
        (major, minor, revision, build))
    _require(identity == ReleaseIdentity(*PINNED_INSTALLED_IDENTITY),
             f"{name}: installed identity is not exact 1.0.15+0/counter15")
    record_end, encryption_tlvs, sha256_tlvs, keyhash_tlvs, signature_tlvs = \
        _parse_unprotected_end(
        name, image, protected_end)
    _require((encryption_tlvs, sha256_tlvs, keyhash_tlvs, signature_tlvs) ==
             (1, 1, 1, 1),
             f"{name}: installed image must retain one ENC/hash/key/signature TLV")

    initial_report = parse_primary(
        f"{name}-initial", initial, offset, expected_size)
    _require(initial_report.identity == identity,
             f"{name}: pinned initial image identity differs")
    update_magic, _update_load, update_header_size, update_protected_size, \
        update_image_size, update_flags = struct.unpack_from("<IIHHII", update, 0)
    update_version = struct.unpack_from("<BBHI", update, 20)
    _require(update_magic == IMAGE_MAGIC and update_flags == IMAGE_F_ENCRYPTED and
             (update_header_size, update_protected_size, update_image_size) ==
             (header_size, protected_size, image_size),
             f"{name}: pinned OTA image geometry/flags differ")
    update_identity, update_protected_end = _parse_protected_identity(
        f"{name}-update", update, update_header_size, update_image_size,
        update_protected_size, update_version)
    update_record_end, update_encryption_tlvs, update_sha256_tlvs, \
        update_keyhash_tlvs, update_signature_tlvs = _parse_unprotected_end(
        f"{name}-update", update, update_protected_end)
    _require(update_identity == identity and update_record_end == record_end and
             (update_encryption_tlvs, update_sha256_tlvs,
              update_keyhash_tlvs, update_signature_tlvs) == (1, 1, 1, 1),
             f"{name}: pinned OTA signed identity/TLV layout differs")
    _require(image[:header_size] == update[:header_size],
             f"{name}: installed header differs from the exact encrypted OTA header")
    payload_end = header_size + image_size
    _require(image[header_size:payload_end] == initial[header_size:payload_end],
             f"{name}: installed payload is not the exact reviewed plaintext")
    _require(image[payload_end:record_end] == update[payload_end:record_end],
             f"{name}: installed signed TLV region differs from the exact OTA image")
    _require(_sha256(image[:record_end]) == PINNED_INSTALLED_RECORD_HASHES[name] and
             _sha256(image[header_size:payload_end]) == PINNED_PAYLOAD_HASHES[name],
             f"{name}: installed image record or plaintext payload hash differs")

    trailer_start = expected_size - BOOT_TRAILER_SIZE
    _require(record_end <= trailer_start and
             image[record_end:trailer_start] == b"\xff" * (trailer_start - record_end),
             f"{name}: bytes between signed image and MCUboot trailer are not erased")
    status_count = ((record_end + SCRATCH_SIZE - 1) // SCRATCH_SIZE) * \
        BOOT_STATUS_STATE_COUNT
    _require(status_count <= BOOT_STATUS_MAX_ENTRIES * BOOT_STATUS_STATE_COUNT,
             f"{name}: installed image needs too many swap status entries")
    status = image[trailer_start:
                   trailer_start + BOOT_STATUS_MAX_ENTRIES *
                   BOOT_STATUS_STATE_COUNT * BOOT_MAX_ALIGN]
    for entry in range(BOOT_STATUS_MAX_ENTRIES * BOOT_STATUS_STATE_COUNT):
        unit = status[entry * BOOT_MAX_ALIGN:(entry + 1) * BOOT_MAX_ALIGN]
        expected = _program_unit(entry % BOOT_STATUS_STATE_COUNT + 1) \
            if entry < status_count else b"\xff" * BOOT_MAX_ALIGN
        _require(unit == expected, f"{name}: MCUboot swap status is not complete/canonical")

    key_area = image[trailer_start + len(status):
                     trailer_start + len(status) + 2 * BOOT_ENC_KEY_SIZE]
    _require(key_area == bytes.fromhex(PINNED_ENCRYPTED_KEY_AREAS[name]),
             f"{name}: MCUboot encrypted swap key slots differ from the exact "
             "reviewed two-swap state")
    swap_size_block = image[-SWAP_SIZE_OFFSET_FROM_END:
                            -SWAP_SIZE_OFFSET_FROM_END + BOOT_MAX_ALIGN]
    swap_info_block = image[-SWAP_INFO_OFFSET_FROM_END:
                            -SWAP_INFO_OFFSET_FROM_END + BOOT_MAX_ALIGN]
    copy_done_block = image[-COPY_DONE_OFFSET_FROM_END:
                            -COPY_DONE_OFFSET_FROM_END + BOOT_MAX_ALIGN]
    image_ok_block = image[-IMAGE_OK_OFFSET_FROM_END:
                           -IMAGE_OK_OFFSET_FROM_END + BOOT_MAX_ALIGN]
    _require(swap_size_block == struct.pack("<I", record_end) + b"\xff" * 12,
             f"{name}: MCUboot swap size is not the exact image record size")
    expected_swap_info = (image_index << 4) | 0x2
    _require(swap_info_block == _program_unit(expected_swap_info),
             f"{name}: MCUboot swap info is not the reviewed post-swap state")
    _require(copy_done_block == _program_unit(1),
             f"{name}: MCUboot copy_done is not exactly 0x01")
    _require(image_ok_block == _program_unit(1),
             f"{name}: primary is not exactly confirmed (image_ok != 0x01)")
    _require(image[-len(BOOT_MAGIC):] == BOOT_MAGIC,
             f"{name}: primary trailer boot magic is not GOOD")

    return InstalledImageReport(
        name=name, offset=offset, size=expected_size,
        header_size=header_size, image_size=image_size,
        protected_tlv_size=protected_size, identity=identity, flags=flags,
        image_record_size=record_end,
        image_record_sha256=_sha256(image[:record_end]),
        payload_sha256=_sha256(image[header_size:payload_end]),
        swap_size=record_end, swap_info=expected_swap_info, copy_done=1,
        image_ok=1, swap_status_entries=status_count,
        encrypted_key_slots_present=2,
        encrypted_key_area_sha256=_sha256(key_area),
    )


def validate_installed_flash(
        flash: bytes, release_dir: Path) \
        -> tuple[InstalledImageReport, InstalledImageReport, bytes, bytes]:
    """Validate the exact post-swap, confirmed 1.0.15 installed pair."""
    _require(len(flash) == FLASH_SIZE, "full Flash backup must be exactly 2 MiB")
    initial_s, initial_ns, update_s, update_ns = _load_pinned_release(release_dir)
    secure = flash[SECURE_OFFSET:SECURE_OFFSET + SECURE_SIZE]
    nonsecure = flash[NONSECURE_OFFSET:NONSECURE_OFFSET + NONSECURE_SIZE]
    secure_report = _validate_installed_primary(
        "secure", secure, initial_s, update_s, SECURE_OFFSET, SECURE_SIZE, 0)
    nonsecure_report = _validate_installed_primary(
        "nonsecure", nonsecure, initial_ns, update_ns,
        NONSECURE_OFFSET, NONSECURE_SIZE, 1)
    _require(secure_report.identity == nonsecure_report.identity,
             "Secure and NonSecure installed release identities differ")
    return secure_report, nonsecure_report, secure, nonsecure


def validate_flash(flash: bytes) -> tuple[ImageReport, ImageReport, bytes, bytes]:
    _require(len(flash) == FLASH_SIZE, "full Flash backup must be exactly 2 MiB")
    secure = flash[SECURE_OFFSET:SECURE_OFFSET + SECURE_SIZE]
    nonsecure = flash[NONSECURE_OFFSET:NONSECURE_OFFSET + NONSECURE_SIZE]
    secure_report = parse_primary("secure", secure, SECURE_OFFSET, SECURE_SIZE)
    nonsecure_report = parse_primary(
        "nonsecure", nonsecure, NONSECURE_OFFSET, NONSECURE_SIZE)
    _require(secure_report.identity == nonsecure_report.identity,
             "Secure and NonSecure primary release identities differ")
    return secure_report, nonsecure_report, secure, nonsecure


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--installed-exact-1.0.15", dest="installed_exact_1_0_15",
        type=Path, metavar="RELEASE_DIR",
        help=("audit the exact decrypted-after-swap 1.0.15 pair; this explicit "
              "release-bound mode is never used by the pre-loader backup"),
    )
    args = parser.parse_args()

    if args.installed_exact_1_0_15 is None:
        secure_report, nonsecure_report, secure, nonsecure = validate_flash(
            args.flash.read_bytes())
        schema = "roller-ecu-open-loader-primary-audit-v1"
        installed_format = None
    else:
        secure_report, nonsecure_report, secure, nonsecure = \
            validate_installed_flash(
                args.flash.read_bytes(), args.installed_exact_1_0_15)
        schema = "roller-ecu-installed-primary-audit-v1"
        installed_format = {
            "format": "mcuboot-scratch-swap-decrypted-payload-encrypted-header-v1",
            "release_artifacts": {
                name: {"size": size, "sha256": digest}
                for name, (size, digest) in PINNED_RELEASE_ARTIFACTS.items()
            },
        }
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "secure-primary.bin").write_bytes(secure)
    (args.output_dir / "nonsecure-primary.bin").write_bytes(nonsecure)
    report = {
        "schema": schema,
        "release_identity": asdict(secure_report.identity),
        "images": {
            "secure": asdict(secure_report),
            "nonsecure": asdict(nonsecure_report),
        },
    }
    if installed_format is not None:
        report["installed_format"] = installed_format
    (args.output_dir / "primary-audit.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    identity = secure_report.identity
    description = "Exact installed post-swap pair" if installed_format else \
        "Paired primary backup"
    print(f"{description} verified: "
          f"{identity.major}.{identity.minor}.{identity.revision}+{identity.build}, "
          f"security_counter={identity.security_counter}, image_ok=0x01/0x01")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
