#!/usr/bin/env python3
"""Build paired OEMiROT images and a signed/encrypted ECU Ethernet OTA file."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature


PROJECT = Path(__file__).resolve().parents[1]
CUBE_H5 = Path("/home/plac/STM32Cube/Repository/STM32Cube_FW_H5_V1.7.0")
IMGTOOL = CUBE_H5 / "Middlewares/Third_Party/mcuboot/scripts/imgtool.py"
OBJCOPY = Path(
    "/home/plac/.local/share/stm32cube/bundles/gnu-tools-for-stm32/"
    "14.3.1+st.2/bin/arm-none-eabi-objcopy"
)
DEFAULT_PKI = Path("/home/plac/.local/share/roller-ecu-pki")
MANIFEST_FORMAT = "<12I32s32s4I"
MANIFEST_MAGIC = 0x31544F52
MANIFEST_SCHEMA = 1
OTA_FLAGS = 0x3
BOOT_MAGIC = bytes.fromhex("77c295f360d2ef7f3552500f2cb67980")
IMAGE_ENCRYPTION_ALIGNMENT = 16
RELEASE_METADATA_TYPES = {
    "format": str,
    "version": str,
    "security_counter": int,
    "update_sequence": int,
    "layout_version": int,
    "secure_sha256": str,
    "nonsecure_sha256": str,
    "secure_size": int,
    "nonsecure_size": int,
}
RELEASE_FORMAT = "roller-ecu-ota-v1"
SHA256_HEX = re.compile(r"[0-9a-f]{64}")


class ReleaseIdentityError(RuntimeError):
    """Release history cannot safely accept the requested identity."""


def strict_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for name, value in pairs:
        if name in result:
            raise ValueError(f"duplicate JSON field: {name}")
        result[name] = value
    return result


def version_tuple(text: str) -> tuple[int, int, int, int]:
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:\+(\d+))?", text)
    if match is None:
        raise argparse.ArgumentTypeError("version must be MAJOR.MINOR.REVISION[+BUILD]")
    values = tuple(int(value or 0) for value in match.groups())
    if any(value > 0xFFFF for value in values[:3]) or values[3] > 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("version component is out of range")
    return values  # type: ignore[return-value]


def version_text(version: tuple[int, int, int, int]) -> str:
    text = f"{version[0]}.{version[1]}.{version[2]}"
    if version[3]:
        text += f"+{version[3]}"
    return text


def validate_release_identity(history_dir: Path, requested_version: str,
                              requested_security_counter: int,
                              requested_update_sequence: int,
                              output_dir: Path | None = None) -> None:
    """Require a canonical release ledger and a strictly newer identity."""
    if type(requested_version) is not str:
        raise ReleaseIdentityError("requested version must be a string")
    try:
        canonical_requested_version = version_text(version_tuple(requested_version))
    except (argparse.ArgumentTypeError, TypeError) as exc:
        raise ReleaseIdentityError(
            f"requested version is invalid: {requested_version!r}"
        ) from exc
    if canonical_requested_version != requested_version:
        raise ReleaseIdentityError(
            f"requested version is not canonical: {requested_version!r}"
        )
    if (type(requested_security_counter) is not int
            or requested_security_counter <= 0
            or requested_security_counter > 0xFFFFFFFF):
        raise ReleaseIdentityError("requested security counter is out of range")
    if (type(requested_update_sequence) is not int
            or requested_update_sequence <= 0
            or requested_update_sequence > 0xFFFFFFFF):
        raise ReleaseIdentityError("requested update sequence is out of range")

    try:
        resolved_history = history_dir.resolve()
    except OSError as exc:
        raise ReleaseIdentityError(
            f"cannot resolve release history directory {history_dir}: {exc}"
        ) from exc
    if output_dir is not None:
        try:
            resolved_output_parent = output_dir.parent.resolve()
        except OSError as exc:
            raise ReleaseIdentityError(
                f"cannot resolve release output directory {output_dir}: {exc}"
            ) from exc
        if (output_dir.name != requested_version
                or resolved_output_parent != resolved_history
                or output_dir.is_symlink()):
            raise ReleaseIdentityError(
                "release output directory must be the canonical history path "
                f"{history_dir / requested_version}"
            )

    if not history_dir.exists():
        return
    if not history_dir.is_dir() or history_dir.is_symlink():
        raise ReleaseIdentityError(
            f"release history path is not a real directory: {history_dir}"
        )

    historical_versions: list[tuple[int, int, int, int]] = []
    historical_counters: list[int] = []
    historical_sequences: list[int] = []
    try:
        release_entries = sorted(history_dir.iterdir(), key=lambda path: path.name)
    except OSError as exc:
        raise ReleaseIdentityError(
            f"cannot enumerate release history {history_dir}: {exc}"
        ) from exc

    for release_dir in release_entries:
        if not release_dir.is_dir() or release_dir.is_symlink():
            raise ReleaseIdentityError(
                f"release history entry is not a real directory: {release_dir}"
            )
        metadata_path = release_dir / "metadata.json"
        if not metadata_path.is_file() or metadata_path.is_symlink():
            raise ReleaseIdentityError(
                f"release history is incomplete: missing {metadata_path}"
            )
        try:
            metadata = json.loads(
                metadata_path.read_text(encoding="utf-8"),
                object_pairs_hook=strict_json_object,
            )
        except (OSError, UnicodeError, ValueError) as exc:
            raise ReleaseIdentityError(
                f"cannot trust release metadata {metadata_path}: {exc}"
            ) from exc
        if type(metadata) is not dict:
            raise ReleaseIdentityError(
                f"release metadata is not an object: {metadata_path}"
            )

        if set(metadata) != set(RELEASE_METADATA_TYPES):
            raise ReleaseIdentityError(
                f"release metadata schema is not exact: {metadata_path}"
            )
        for field, expected_type in RELEASE_METADATA_TYPES.items():
            if type(metadata[field]) is not expected_type:
                raise ReleaseIdentityError(
                    f"release metadata field {field!r} has invalid type: "
                    f"{metadata_path}"
                )
        if metadata["format"] != RELEASE_FORMAT:
            raise ReleaseIdentityError(
                f"release metadata has unsupported format: {metadata_path}"
            )

        historical_version = metadata.get("version")
        historical_counter = metadata.get("security_counter")
        historical_sequence = metadata.get("update_sequence")
        try:
            historical_version_tuple = version_tuple(historical_version)
            canonical_historical_version = version_text(historical_version_tuple)
        except (argparse.ArgumentTypeError, TypeError) as exc:
            raise ReleaseIdentityError(
                f"release metadata has invalid version: {metadata_path}"
            ) from exc
        if (canonical_historical_version != historical_version
                or release_dir.name != historical_version):
            raise ReleaseIdentityError(
                f"release directory/version mismatch: {metadata_path}"
            )
        if (historical_counter <= 0
                or historical_counter > 0xFFFFFFFF):
            raise ReleaseIdentityError(
                f"release metadata has invalid security_counter: {metadata_path}"
            )
        if historical_sequence <= 0 or historical_sequence > 0xFFFFFFFF:
            raise ReleaseIdentityError(
                f"release metadata has invalid update_sequence: {metadata_path}"
            )
        if metadata["layout_version"] <= 0 or \
                metadata["layout_version"] > 0xFFFFFFFF:
            raise ReleaseIdentityError(
                f"release metadata has invalid layout_version: {metadata_path}"
            )
        for field in ("secure_size", "nonsecure_size"):
            if metadata[field] <= 0 or metadata[field] > 0xFFFFFFFF:
                raise ReleaseIdentityError(
                    f"release metadata has invalid {field}: {metadata_path}"
                )
        for field in ("secure_sha256", "nonsecure_sha256"):
            if SHA256_HEX.fullmatch(metadata[field]) is None:
                raise ReleaseIdentityError(
                    f"release metadata has invalid {field}: {metadata_path}"
                )

        historical_versions.append(historical_version_tuple)
        historical_counters.append(historical_counter)
        historical_sequences.append(historical_sequence)

    requested_version_tuple = version_tuple(requested_version)
    if historical_versions and requested_version_tuple <= max(historical_versions):
        raise ReleaseIdentityError(
            f"version {requested_version} must be strictly greater than historical "
            f"maximum {version_text(max(historical_versions))}"
        )
    if historical_counters and requested_security_counter <= max(historical_counters):
        raise ReleaseIdentityError(
            f"security_counter {requested_security_counter} must be strictly greater "
            f"than historical maximum {max(historical_counters)}"
        )
    if historical_sequences and requested_update_sequence <= max(historical_sequences):
        raise ReleaseIdentityError(
            f"update_sequence {requested_update_sequence} must be strictly greater "
            f"than historical maximum {max(historical_sequences)}"
        )


def define_value(header: str, name: str) -> int:
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(0x[0-9A-Fa-f]+|\d+)",
                      header, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"missing canonical layout value {name}")
    return int(match.group(1), 0)


def run(command: list[str], *, env: dict[str, str] | None = None) -> None:
    subprocess.run(command, check=True, env=env)


def decrypt_key(source: Path, destination: Path, password: bytes) -> None:
    key = serialization.load_pem_private_key(source.read_bytes(), password=password)
    destination.write_bytes(key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    ))
    destination.chmod(stat.S_IRUSR | stat.S_IWUSR)


def align_raw_firmware_binary(path: Path) -> int:
    """Pad one raw payload before both plaintext and encrypted signing."""
    data = path.read_bytes()
    if not data:
        raise RuntimeError(f"refusing to sign an empty firmware payload: {path}")
    padding = (-len(data)) % IMAGE_ENCRYPTION_ALIGNMENT
    if padding:
        path.write_bytes(data + b"\xff" * padding)
    return padding


def sign_image(imgtool_env: dict[str, str], key: Path, encryption_key: Path,
               raw: Path, output: Path, version: str, dependency: str,
               security_counter: int, slot_size: int, initial: bool) -> None:
    command = [
        sys.executable, str(IMGTOOL), "sign", "-k", str(key),
        "-H", "0x400", "-S", hex(slot_size), "--pad-header",
        "--align", "16", "-M", "40", "-v", version,
        "-s", str(security_counter), "-d", dependency,
    ]
    if initial:
        command += ["-c", "--confirm"]
    else:
        command += ["-E", str(encryption_key), "--pad"]
    command += [str(raw), str(output)]
    run(command, env=imgtool_env)
    image = output.read_bytes()
    if len(image) != slot_size:
        raise RuntimeError(f"{output.name}: expected padded size {slot_size:#x}")
    if image[-16:] != BOOT_MAGIC:
        raise RuntimeError(f"{output.name}: MCUboot trailer magic missing")
    flags = struct.unpack_from("<I", image, 16)[0]
    if initial and flags & 0x4:
        raise RuntimeError(f"{output.name}: initial image was not cleared")
    if not initial and not flags & 0x4:
        raise RuntimeError(f"{output.name}: OTA image is not encrypted")


def copy_new(source: Path, destination: Path, force: bool) -> None:
    if destination.exists() and not force:
        raise FileExistsError(f"refusing to overwrite {destination}; use --force")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IRGRP | stat.S_IROTH)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, type=version_tuple)
    parser.add_argument("--security-counter", required=True, type=int)
    parser.add_argument("--update-sequence", required=True, type=int)
    parser.add_argument("--build-type", default="Release", choices=("Debug", "Release"))
    parser.add_argument("--pki-dir", type=Path, default=DEFAULT_PKI)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)

    version = args.version
    if args.security_counter <= 0 or args.security_counter > 0xFFFFFFFF:
        parser.error("security counter must be 1..0xffffffff")
    if args.update_sequence <= 0 or args.update_sequence > 0xFFFFFFFF:
        parser.error("update sequence must be 1..0xffffffff")
    release_version = version_text(version)
    dependency_version = f"{version[0]}.{version[1]}.{version[2]}"
    output_dir = args.output_dir or PROJECT / "artifacts/firmware" / release_version

    # Release version, rollback counter and transport sequence are immutable
    # monotonically increasing identities.
    # Check the canonical history before building or signing; --force may
    # replace ordinary files only and deliberately cannot bypass this gate.
    try:
        validate_release_identity(
            PROJECT / "artifacts/firmware",
            release_version,
            args.security_counter,
            args.update_sequence,
            output_dir,
        )
    except ReleaseIdentityError as exc:
        parser.error(str(exc))

    # A package is a release artifact, not a wrapper around whichever ELF
    # happened to be left in the build directory.  Always run the reviewed
    # OEMiROT build first so CubeMX/source changes cannot be omitted from a
    # newly signed package by a stale incremental artifact.
    run([str(PROJECT / "tools/build_signed_apps.sh"), args.build_type])

    required = [IMGTOOL, OBJCOPY,
                args.pki_dir / "key-passphrase.txt",
                args.pki_dir / "oemirot-auth-s.pem",
                args.pki_dir / "oemirot-auth-ns.pem",
                args.pki_dir / "oemirot-encryption-public.pem",
                args.pki_dir / "ota-transport.pem"]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise FileNotFoundError("missing packaging inputs: " + ", ".join(missing))

    layout_text = (PROJECT / "Shared/ecu_flash_layout.h").read_text()
    layout_version = define_value(layout_text, "ECU_LAYOUT_VERSION")
    secure_size = define_value(layout_text, "ECU_SECURE_SECONDARY_SIZE")
    nonsecure_size = define_value(layout_text, "ECU_NONSECURE_SECONDARY_SIZE")
    secure_elf = PROJECT / f"Secure/build/OEMiROT/{args.build_type}/ECU_S.elf"
    nonsecure_elf = PROJECT / f"NonSecure/build/OEMiROT/{args.build_type}/ECU_NS.elf"
    if not secure_elf.is_file() or not nonsecure_elf.is_file():
        raise FileNotFoundError(
            f"OEMiROT ELFs missing; run ./tools/build_signed_apps.sh {args.build_type}"
        )

    password = (args.pki_dir / "key-passphrase.txt").read_bytes().strip()
    if not password:
        raise RuntimeError("empty PKI passphrase")
    with tempfile.TemporaryDirectory(prefix="ecu-package-") as temp_name:
        temp = Path(temp_name)
        raw_s, raw_ns = temp / "secure-raw.bin", temp / "nonsecure-raw.bin"
        key_s, key_ns = temp / "auth-s.pem", temp / "auth-ns.pem"
        run([str(OBJCOPY), "-O", "binary", str(secure_elf), str(raw_s)])
        run([str(OBJCOPY), "-O", "binary", str(nonsecure_elf), str(raw_ns)])
        # MCUboot ECIES encrypts complete AES blocks.  Align the shared raw
        # input before either signing operation so initial and OTA records
        # have identical signed payload geometry for every linker size.
        align_raw_firmware_binary(raw_s)
        align_raw_firmware_binary(raw_ns)
        decrypt_key(args.pki_dir / "oemirot-auth-s.pem", key_s, password)
        decrypt_key(args.pki_dir / "oemirot-auth-ns.pem", key_ns, password)
        imgtool_env = dict(os.environ)
        compat = str(PROJECT / "tools/imgtool_compat")
        imgtool_env["PYTHONPATH"] = compat + os.pathsep + imgtool_env.get("PYTHONPATH", "")

        initial_s, initial_ns = temp / "secure-initial.bin", temp / "nonsecure-initial.bin"
        update_s, update_ns = temp / "secure-update.bin", temp / "nonsecure-update.bin"
        encryption_public = args.pki_dir / "oemirot-encryption-public.pem"
        sign_image(imgtool_env, key_s, encryption_public, raw_s, initial_s,
                   release_version, f"(1,{dependency_version})",
                   args.security_counter, secure_size, True)
        sign_image(imgtool_env, key_ns, encryption_public, raw_ns, initial_ns,
                   release_version, f"(0,{dependency_version})",
                   args.security_counter, nonsecure_size, True)
        sign_image(imgtool_env, key_s, encryption_public, raw_s, update_s,
                   release_version, f"(1,{dependency_version})",
                   args.security_counter, secure_size, False)
        sign_image(imgtool_env, key_ns, encryption_public, raw_ns, update_ns,
                   release_version, f"(0,{dependency_version})",
                   args.security_counter, nonsecure_size, False)

        secure_hash = hashlib.sha256(update_s.read_bytes()).digest()
        nonsecure_hash = hashlib.sha256(update_ns.read_bytes()).digest()
        manifest = struct.pack(
            MANIFEST_FORMAT, MANIFEST_MAGIC, MANIFEST_SCHEMA, layout_version,
            args.update_sequence, *version, args.security_counter, OTA_FLAGS,
            secure_size, nonsecure_size, secure_hash, nonsecure_hash,
            0, 0, 0, 0,
        )
        if len(manifest) != 128:
            raise AssertionError("canonical OTA manifest size changed")
        transport_key = serialization.load_pem_private_key(
            (args.pki_dir / "ota-transport.pem").read_bytes(), password=password
        )
        if not isinstance(transport_key, ec.EllipticCurvePrivateKey):
            raise TypeError("OTA transport key is not an EC private key")
        der_signature = transport_key.sign(manifest, ec.ECDSA(hashes.SHA256()))
        signature_r, signature_s = decode_dss_signature(der_signature)
        signature = signature_r.to_bytes(32, "big") + signature_s.to_bytes(32, "big")

        metadata = {
            "format": "roller-ecu-ota-v1",
            "version": release_version,
            "security_counter": args.security_counter,
            "update_sequence": args.update_sequence,
            "layout_version": layout_version,
            "secure_sha256": secure_hash.hex(),
            "nonsecure_sha256": nonsecure_hash.hex(),
            "secure_size": secure_size,
            "nonsecure_size": nonsecure_size,
        }
        package = temp / f"roller-ecu-{release_version}.recu"
        with zipfile.ZipFile(package, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr("metadata.json", json.dumps(metadata, indent=2) + "\n")
            archive.writestr("manifest.bin", manifest)
            archive.writestr("signature.bin", signature)
            archive.write(update_s, "secure.bin")
            archive.write(update_ns, "nonsecure.bin")

        copy_new(initial_s, output_dir / "secure-initial.bin", args.force)
        copy_new(initial_ns, output_dir / "nonsecure-initial.bin", args.force)
        copy_new(package, output_dir / package.name, args.force)
        metadata_path = temp / "metadata.json"
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
        copy_new(metadata_path, output_dir / "metadata.json", args.force)

    print(f"Initial images and signed encrypted OTA package: {output_dir}")
    print(f"Package SHA-256: {hashlib.sha256((output_dir / package.name).read_bytes()).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
