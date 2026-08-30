#!/usr/bin/env python3
"""Fail-closed validators for the irreversible OPEN-to-CLOSED gate."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import struct
import uuid
from pathlib import Path
from typing import Any


EXPECTED_MCU_UID = "003800613434511232383537"
EXPECTED_ATECC_SERIAL = "0123d47eb2ee0e9bee"
EXPECTED_ATECC_CONFIG_CRC32C = 0xEBB326F3
EXPECTED_PAIRING_GENERATION = 1
EXPECTED_ACCEPTED_SEQUENCE = 15
EXPECTED_RELEASE_CLOSED_SIZE = 52292
EXPECTED_RELEASE_CLOSED_SHA256 = (
    "4073a9faa15eed7ca6014f168ae91efc35ba1e26d8e74651552c9a0a3d8ef884"
)
EXPECTED_RELEASE_IDENTITY = {
    "major": 1,
    "minor": 0,
    "revision": 15,
    "build": 0,
    "security_counter": 15,
}
MAX_UI_AGE_MS = 500
SAFETY_STATUS_OTA_UNCONFIRMED = 1 << 25
SAFETY_SECURITY_AUTHENTICATED = 1 << 4
SAFETY_SECURITY_QUARANTINE = 1 << 5

CLOSED_PHASES = (
    "transaction_created",
    "pairing_store_dual_verified",
    "pre_mutation_evidence_verified",
    "boot_unprotect_started",
    "boot_unprotected",
    "boot_program_started",
    "boot_program_readback_verified",
    "boot_protection_restore_started",
    "boot_protection_verified",
    "release_closed_halt_started",
    "release_closed_halted",
    "provisioning_request_started",
    "product_state_provisioning",
    "da_config_started",
    "da_config_complete",
    "oemirot_config_started",
    "oemirot_config_complete",
    "oemirot_data_started",
    "oemirot_data_complete",
    "preclose_da_discovery_started",
    "preclose_da_integrity_verified",
    "closed_request_started",
    "product_state_closed_verified",
)

TRANSACTION_FILE_SIZES = {
    "ReleaseClosed-ECU_OEMiROT.bin": (
        EXPECTED_RELEASE_CLOSED_SIZE, EXPECTED_RELEASE_CLOSED_SIZE),
    "DA_Config.obk": (108, 108),
    "OEMiRoT_Config.obk": (300, 300),
    "OEMiRoT_Data.obk": (204, 204),
    "security-assets.json": (1, None),
    "cert-root.b64": (1, None),
    "cert-intermediate.b64": (1, None),
    "cert-leaf.b64": (1, None),
    "cert-leaf-chain.b64": (1, None),
    "oemirot-auth-s-public.pem": (1, None),
    "oemirot-auth-ns-public.pem": (1, None),
    "option-bytes-before.txt": (1, None),
    "full-flash.bin": (2097152, 2097152),
    "secure-persistent.bin": (131072, 131072),
    "programmer-mcu-uid.bin": (12, 12),
    "pairing-store.bin": (16384, 16384),
    "runtime-before.json": (1, None),
    "ota-before.json": (1, None),
    "secure-primary.bin": (196608, 196608),
    "nonsecure-primary.bin": (327680, 327680),
    "primary-audit.json": (1, None),
}


class ClosedPreflightError(ValueError):
    """The irreversible CLOSED transition has not met a required gate."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ClosedPreflightError(message)


def _exact_int(value: Any, expected: int, name: str) -> None:
    _require(type(value) is int and value == expected,
             f"{name} must be exactly {expected}, got {value!r}")


def _fresh_age(state: dict[str, Any], name: str) -> None:
    value = state.get(name)
    _require(type(value) is int and 0 <= value <= MAX_UI_AGE_MS,
             f"{name} must be a fresh 0..{MAX_UI_AGE_MS} ms sample, got {value!r}")


def validate_programmer_uid(data: bytes) -> str:
    _require(len(data) == 12,
             "Programmer MCU UID upload must be exactly 12 bytes")
    actual = "".join(f"{word:08x}" for word in struct.unpack("<III", data))
    _require(actual == EXPECTED_MCU_UID,
             f"physical Programmer MCU UID mismatch: {actual}")
    return actual


def validate_runtime_state(state: dict[str, Any]) -> None:
    _require(type(state) is dict, "UI state root must be an object")
    for age_name in ("status_age_ms", "diagnostic_age_ms", "security_age_ms"):
        _fresh_age(state, age_name)

    security = state.get("security")
    diagnostic = state.get("diagnostic")
    status = state.get("status")
    _require(type(security) is dict, "fresh UI security state is missing")
    _require(type(diagnostic) is dict, "fresh UI diagnostic state is missing")
    _require(type(status) is dict, "fresh UI status state is missing")

    _exact_int(security.get("atecc_result"), 0, "ATECC probe result")
    _exact_int(security.get("auth_result"), 0, "ATECC authentication result")
    flags = security.get("flags")
    _require(type(flags) is int, "ATECC security flags are missing")
    _require(bool(flags & SAFETY_SECURITY_AUTHENTICATED),
             "ATECC authenticated flag is clear")
    _require(not bool(flags & SAFETY_SECURITY_QUARANTINE),
             "ATECC quarantine flag is set")
    _exact_int(security.get("config_locked"), 1, "ATECC Config lock")
    _exact_int(security.get("data_locked"), 1, "ATECC Data lock")
    _require(security.get("mcu_uid") == EXPECTED_MCU_UID,
             "runtime manufacturing MCU identity is not the reviewed ECU")
    _require(security.get("serial") == EXPECTED_ATECC_SERIAL,
             "runtime ATECC serial is not the reviewed ECU")
    _exact_int(security.get("config_crc32c"),
               EXPECTED_ATECC_CONFIG_CRC32C, "ATECC Config CRC-32C")
    _exact_int(security.get("pairing_generation"),
               EXPECTED_PAIRING_GENERATION, "ATECC pairing generation")

    safety_status = diagnostic.get("safety_status")
    _require(type(safety_status) is int, "Secure safety status is missing")
    _require((safety_status & SAFETY_STATUS_OTA_UNCONFIRMED) == 0,
             "SAFETY_STATUS_OTA_UNCONFIRMED is set")
    _exact_int(diagnostic.get("requested_relay_mask"), 0,
               "requested actuator relay mask")
    _exact_int(diagnostic.get("applied_relay_mask"), 0,
               "applied actuator relay mask")
    _exact_int(status.get("forward_duty_permille"), 0,
               "forward valve duty")
    _exact_int(status.get("reverse_duty_permille"), 0,
               "reverse valve duty")
    _require(state.get("tuning_active") is False,
             "PI tuning telemetry must be inactive")


def validate_ota_status(status: dict[str, Any]) -> None:
    _require(type(status) is dict, "OTA status root must be an object")
    _exact_int(status.get("result"), 0, "OTA request result")
    _exact_int(status.get("ota_result"), 0, "Secure OTA result")
    _exact_int(status.get("state"), 0, "OTA state (IDLE)")
    _exact_int(status.get("accepted_sequence"), EXPECTED_ACCEPTED_SEQUENCE,
               "OTA accepted sequence")
    for name in ("update_sequence", "secure_received", "nonsecure_received",
                 "secure_image_size", "nonsecure_image_size"):
        _exact_int(status.get(name), 0, f"idle OTA {name}")


def validate_primary_audit(report: dict[str, Any]) -> None:
    _require(type(report) is dict, "primary audit root must be an object")
    _require(report.get("schema") == "roller-ecu-open-loader-primary-audit-v1",
             "primary audit schema is not recognized")
    identity = report.get("release_identity")
    _require(type(identity) is dict and
             set(identity) == set(EXPECTED_RELEASE_IDENTITY),
             "paired primary release identity fields are not canonical")
    for field, expected in EXPECTED_RELEASE_IDENTITY.items():
        _exact_int(identity.get(field), expected,
                   f"paired primary release identity {field}")
    images = report.get("images")
    _require(type(images) is dict, "paired primary image reports are missing")
    for name in ("secure", "nonsecure"):
        image = images.get(name)
        _require(type(image) is dict, f"{name} primary audit is missing")
        _exact_int(image.get("image_ok"), 1, f"{name} primary image_ok")
        image_identity = image.get("identity")
        _require(type(image_identity) is dict and
                 set(image_identity) == set(EXPECTED_RELEASE_IDENTITY),
                 f"{name} primary identity fields are not canonical")
        for field, expected in EXPECTED_RELEASE_IDENTITY.items():
            _exact_int(image_identity.get(field), expected,
                       f"{name} primary identity {field}")


def validate_installed_primary_audit(report: dict[str, Any]) -> None:
    """Validate the release-bound post-scratch-swap audit report."""
    _require(type(report) is dict and set(report) == {
        "schema", "release_identity", "images", "installed_format"},
        "installed primary audit fields are not canonical")
    _require(report.get("schema") == "roller-ecu-installed-primary-audit-v1",
             "installed primary audit schema is not recognized")
    identity = report.get("release_identity")
    _require(type(identity) is dict and set(identity) == set(EXPECTED_RELEASE_IDENTITY),
             "installed paired primary identity fields are not canonical")
    for field, expected in EXPECTED_RELEASE_IDENTITY.items():
        _exact_int(identity.get(field), expected,
                   f"installed paired primary identity {field}")
    installed_format = report.get("installed_format")
    expected_artifacts = {
        "metadata.json": {"size": 367, "sha256":
            "9bf210ad3faf82fa7ea55e8123bbcd43f95ee836d8939df64d36cae28dc44bc3"},
        "secure-initial.bin": {"size": 196608, "sha256":
            "1a50b9106f798fb64c1a766d81d194b8829e07497941b4af7efe41bb38b98d75"},
        "nonsecure-initial.bin": {"size": 327680, "sha256":
            "4f96d510f06cda3de1a88de8be5a7516b09f1ed539b05e0d6be2eeaaa571d8ed"},
        "roller-ecu-1.0.15.recu": {"size": 525371, "sha256":
            "9980acbf248f242c77c7044f48d627ca862d6714221f5835c0a21b7a24bf9e54"},
    }
    _require(type(installed_format) is dict and installed_format == {
        "format": "mcuboot-scratch-swap-decrypted-payload-encrypted-header-v1",
        "release_artifacts": expected_artifacts,
    }, "installed primary storage format/release artifacts differ")
    expected_images = {
        "secure": {
            "name": "secure", "offset": 196608, "size": 196608,
            "header_size": 1024, "image_size": 186560,
            "protected_tlv_size": 28, "flags": 4,
            "image_record_size": 187880,
            "image_record_sha256":
                "7d163262f0e83425aee2a5da3aa647795d1ee5288748d8c7d3bda1960d0f66bf",
            "payload_sha256":
                "c74896f879273ed0c50fc32a189e6a89bd3fca05aa33627cc58469ae773ef6ea",
            "swap_size": 187880, "swap_info": 2, "copy_done": 1,
            "image_ok": 1, "swap_status_entries": 9,
            "encrypted_key_slots_present": 2,
            "encrypted_key_area_sha256":
                "2d414e4574b199dc825769fa542f7dd2683633c4bb7cce40e1606da74a7f0d8c",
        },
        "nonsecure": {
            "name": "nonsecure", "offset": 1048576, "size": 327680,
            "header_size": 1024, "image_size": 34832,
            "protected_tlv_size": 28, "flags": 4,
            "image_record_size": 36153,
            "image_record_sha256":
                "649423a9bda226f57515c9f2835d4e607b8bb38431eec727af9d6954ab119397",
            "payload_sha256":
                "42e870f58b68188c3a38e78970d72f11035b59ac26db4904f642997d11bc0973",
            "swap_size": 36153, "swap_info": 18, "copy_done": 1,
            "image_ok": 1, "swap_status_entries": 3,
            "encrypted_key_slots_present": 2,
            "encrypted_key_area_sha256":
                "70a551233699e6ce27717fabe5873a97a3b95058c3e5105735b50bfa127b3613",
        },
    }
    images = report.get("images")
    _require(type(images) is dict and set(images) == set(expected_images),
             "installed paired primary reports are missing or unknown")
    for name, expected in expected_images.items():
        image = images.get(name)
        _require(type(image) is dict and set(image) == set(expected) | {"identity"},
                 f"{name} installed primary fields are not canonical")
        image_identity = image.get("identity")
        _require(type(image_identity) is dict and
                 set(image_identity) == set(EXPECTED_RELEASE_IDENTITY),
                 f"{name} installed primary identity fields differ")
        for field, value in EXPECTED_RELEASE_IDENTITY.items():
            _exact_int(image_identity.get(field), value,
                       f"{name} installed primary identity {field}")
        for field, value in expected.items():
            _require(type(image.get(field)) is type(value) and image.get(field) == value,
                     f"{name} installed primary {field} differs")


def validate_phase_journal(text: str) -> tuple[str, ...]:
    _require(text != "", "CLOSED transaction phase journal is empty")
    _require(text.endswith("\n"), "phase journal has an incomplete final record")
    phases = tuple(text.splitlines())
    _require(all(phase and phase.strip() == phase for phase in phases),
             "phase journal contains a blank or non-canonical record")
    _require(phases == CLOSED_PHASES[:len(phases)],
             "phase journal is unknown, duplicated, skipped, or out of order")
    return phases


def decide_resume_path(phases: tuple[str, ...], lifecycle: str) -> str:
    _require(phases == CLOSED_PHASES[:len(phases)] and
             "pre_mutation_evidence_verified" in phases,
             "transaction has no durable pre-mutation evidence gate")
    lifecycle = lifecycle.upper()
    if lifecycle == "OPEN":
        _require("product_state_provisioning" not in phases,
                 "journal says PROVISIONING/CLOSED but target reports OPEN")
        return "OPEN_BOOT"
    if lifecycle == "PROVISIONING":
        _require("provisioning_request_started" in phases,
                 "target entered PROVISIONING outside this transaction")
        _require("product_state_closed_verified" not in phases,
                 "journal says CLOSED verified but target reports PROVISIONING")
        return "PROVISIONING"
    if lifecycle == "CLOSED":
        _require("closed_request_started" in phases,
                 "target is CLOSED without write-ahead CLOSED evidence")
        return "CLOSED_VERIFY"
    raise ClosedPreflightError("target lifecycle cannot be authoritatively classified")


def validate_transaction_manifest(root: Path, expected_probe: str) -> dict[str, Any]:
    manifest = _load_json(root / "manifest.json")
    _require(set(manifest) == {
        "schema", "transaction_uuid", "created_utc", "target", "release",
        "tool", "files"},
        "CLOSED transaction manifest fields are not canonical")
    _require(manifest.get("schema") == "roller-ecu-closed-transition-v2",
             "CLOSED transaction manifest schema is not recognized")
    transaction_id = manifest.get("transaction_uuid")
    try:
        parsed_uuid = uuid.UUID(transaction_id, version=4)
    except (AttributeError, TypeError, ValueError) as error:
        raise ClosedPreflightError("transaction UUID is not canonical UUIDv4") from error
    _require(str(parsed_uuid) == transaction_id,
             "transaction UUID is not canonical UUIDv4")
    created_utc = manifest.get("created_utc")
    try:
        created = datetime.datetime.fromisoformat(created_utc)
    except (TypeError, ValueError) as error:
        raise ClosedPreflightError("transaction creation timestamp is invalid") from error
    _require(created.tzinfo is not None,
             "transaction creation timestamp must include a timezone")
    target = manifest.get("target")
    target_text = {
        "probe_serial": expected_probe,
        "device_id": "0x484",
        "product_state": "OPEN",
        "mcu_uid": EXPECTED_MCU_UID,
        "atecc608_serial": EXPECTED_ATECC_SERIAL,
    }
    _require(type(target) is dict and set(target) == set(target_text) | {
        "atecc_config_crc32c", "pairing_generation"},
        "CLOSED transaction target identity fields are not exact")
    for name, expected in target_text.items():
        _require(target.get(name) == expected,
                 f"CLOSED transaction target {name} is not exact")
    _exact_int(target.get("atecc_config_crc32c"),
               EXPECTED_ATECC_CONFIG_CRC32C,
               "transaction ATECC Config CRC-32C")
    _exact_int(target.get("pairing_generation"), EXPECTED_PAIRING_GENERATION,
               "transaction ATECC pairing generation")
    release = manifest.get("release")
    _require(type(release) is dict and set(release) == {
        "version", "build", "security_counter", "accepted_sequence",
        "oemirot_size", "oemirot_sha256"},
        "CLOSED transaction release identity fields are not exact")
    _require(release.get("version") == "1.0.15",
             "transaction release version is not exactly 1.0.15")
    _exact_int(release.get("build"), 0, "transaction release build")
    _exact_int(release.get("security_counter"), 15,
               "transaction release security counter")
    _exact_int(release.get("accepted_sequence"), EXPECTED_ACCEPTED_SEQUENCE,
               "transaction OTA accepted sequence")
    _exact_int(release.get("oemirot_size"), EXPECTED_RELEASE_CLOSED_SIZE,
               "transaction ReleaseClosed OEMiROT size")
    _require(release.get("oemirot_sha256") == EXPECTED_RELEASE_CLOSED_SHA256,
             "transaction ReleaseClosed OEMiROT hash is not reviewed")
    tool = manifest.get("tool")
    _require(type(tool) is dict and set(tool) == {
        "stm32_programmer_realpath", "stm32_programmer_sha256",
        "stm32_programmer_version"},
        "CLOSED transaction programmer identity fields are not canonical")
    programmer_path = tool.get("stm32_programmer_realpath")
    programmer_hash = tool.get("stm32_programmer_sha256")
    _require(type(programmer_path) is str and Path(programmer_path).is_absolute(),
             "transaction Programmer path is not absolute")
    _require(type(programmer_hash) is str and
             re.fullmatch(r"[0-9a-f]{64}", programmer_hash) is not None,
             "transaction Programmer hash is invalid")
    _require(tool.get("stm32_programmer_version") == "2.23.0",
             "transaction Programmer version is not exactly 2.23.0")
    files = manifest.get("files")
    _require(type(files) is dict and set(files) == set(TRANSACTION_FILE_SIZES),
             "CLOSED transaction file set is incomplete or contains unknown inputs")
    for name, (minimum, maximum) in TRANSACTION_FILE_SIZES.items():
        entry = files.get(name)
        _require(type(entry) is dict and set(entry) == {"size", "sha256"},
                 f"transaction manifest entry is malformed: {name}")
        size = entry.get("size")
        digest = entry.get("sha256")
        _require(type(size) is int and size >= minimum and
                 (maximum is None or size <= maximum),
                 f"transaction file size is invalid: {name}")
        _require(type(digest) is str and
                 re.fullmatch(r"[0-9a-f]{64}", digest) is not None,
                 f"transaction file hash is invalid: {name}")
        path = root / name
        _require(path.is_file() and not path.is_symlink(),
                 f"transaction file is missing or a symbolic link: {name}")
        try:
            data = path.read_bytes()
        except OSError as error:
            raise ClosedPreflightError(
                f"cannot read transaction file {name}: {error}") from error
        _require(len(data) == size, f"transaction file size changed: {name}")
        _require(hashlib.sha256(data).hexdigest() == digest,
                 f"transaction file hash changed: {name}")
        if name == "ReleaseClosed-ECU_OEMiROT.bin":
            _require(size == EXPECTED_RELEASE_CLOSED_SIZE and
                     digest == EXPECTED_RELEASE_CLOSED_SHA256,
                     "transaction ReleaseClosed OEMiROT is not the reviewed artifact")
    return manifest


def _load_json(path: Path) -> dict[str, Any]:
    def no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            _require(key not in result,
                     f"validated JSON {path} has duplicate field: {key}")
            result[key] = value
        return result

    try:
        value = json.loads(path.read_text(encoding="utf-8"),
                           object_pairs_hook=no_duplicates)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ClosedPreflightError(f"cannot read validated JSON {path}: {error}") from error
    _require(type(value) is dict, f"validated JSON {path} is not an object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=(
        "programmer-uid", "runtime-state", "ota-status", "primary-audit",
        "installed-primary-audit",
        "phase-journal", "resume-decision", "transaction-manifest"))
    parser.add_argument("input", type=Path)
    parser.add_argument("--probe")
    parser.add_argument("--lifecycle")
    args = parser.parse_args()

    try:
        if args.kind == "programmer-uid":
            actual = validate_programmer_uid(args.input.read_bytes())
            print(f"Physical Programmer MCU UID verified: {actual}")
        elif args.kind == "runtime-state":
            validate_runtime_state(_load_json(args.input))
            print("Fresh reviewed MCU/ATECC identity, paired confirmation, and zero outputs verified")
        elif args.kind == "ota-status":
            validate_ota_status(_load_json(args.input))
            print("OTA IDLE/result=0/ota_result=0/accepted_sequence=15 verified")
        elif args.kind == "primary-audit":
            validate_primary_audit(_load_json(args.input))
            print("Paired primary identity 1.0.15+0/counter15 and image_ok=0x01/0x01 verified")
        elif args.kind == "installed-primary-audit":
            validate_installed_primary_audit(_load_json(args.input))
            print("Exact installed 1.0.15 post-swap pair and trailer state verified")
        elif args.kind == "phase-journal":
            phases = validate_phase_journal(args.input.read_text(encoding="utf-8"))
            print(f"CLOSED transaction phase journal verified: {phases[-1]}")
        elif args.kind == "resume-decision":
            if not args.lifecycle:
                raise ClosedPreflightError("resume decision requires --lifecycle")
            phases = validate_phase_journal(args.input.read_text(encoding="utf-8"))
            print(decide_resume_path(phases, args.lifecycle))
        else:
            if not args.probe:
                raise ClosedPreflightError("transaction manifest requires --probe")
            manifest = validate_transaction_manifest(args.input, args.probe)
            print(f"CLOSED transaction manifest verified: {manifest['transaction_uuid']}")
    except (OSError, ClosedPreflightError) as error:
        parser.exit(1, f"CLOSED preflight rejected: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
