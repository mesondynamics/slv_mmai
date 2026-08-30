#!/usr/bin/env python3
"""Operate the reviewed STM32H563 certificate Debug Authentication policy."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec


PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_PROGRAMMER = Path(
    "/home/plac/.local/share/stm32cube/bundles/programmer/"
    "2.23.0/bin/STM32_Programmer_CLI"
)
DEFAULT_PKI = Path("/home/plac/.local/share/roller-ecu-pki")
DEFAULT_ASSETS = PROJECT / "artifacts/security-provisioning"
DEFAULT_PROBE = "066BFF565456857187210935"
FIELD_SERVICE_PERMISSION = "0x00004040"


ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
DISCOVERY_FIELDS = {
    "target ID": (
        re.compile(
            r"^[ \t]*discovery:[ \t]*target ID\.*[ \t]*:[ \t]*(\S.*?)[ \t]*$",
            re.MULTILINE,
        ),
        "0x484",
    ),
    "SDA version": (
        re.compile(
            r"^[ \t]*discovery:[ \t]*SDA version\.*[ \t]*:[ \t]*(\S.*?)[ \t]*$",
            re.MULTILINE,
        ),
        "2.4.0",
    ),
    "Vendor ID": (
        re.compile(
            r"^[ \t]*discovery:[ \t]*Vendor ID\.*[ \t]*:[ \t]*(\S.*?)[ \t]*$",
            re.MULTILINE,
        ),
        "STMicroelectronics",
    ),
    "PSA lifecycle": (
        re.compile(
            r"^[ \t]*discovery:[ \t]*PSA lifecycle\.*[ \t]*:[ \t]*(\S.*?)[ \t]*$",
            re.MULTILINE,
        ),
        "ST_LIFECYCLE_CLOSED",
    ),
    "cryptosystems": (
        re.compile(
            r"^[ \t]*discovery:[ \t]*cryptosystems\.*[ \t]*:[ \t]*(\S.*?)[ \t]*$",
            re.MULTILINE,
        ),
        "Ecdsa-P256 SHA256",
    ),
    "ST provisioning integrity status": (
        re.compile(
            r"^[ \t]*discovery:[ \t]*ST provisioning integrity status\.*"
            r"[ \t]*:[ \t]*(\S.*?)[ \t]*$",
            re.MULTILINE,
        ),
        "0xEAEAEAEA",
    ),
    "ST provisioning integrity status message": (
        re.compile(
            r"^[ \t]*discovery:[ \t]*ST provisioning integrity status message\.*"
            r"[ \t]*:[ \t]*(\S.*?)[ \t]*$",
            re.MULTILINE,
        ),
        "VALID",
    ),
}


@dataclass(frozen=True)
class CliResult:
    returncode: int
    output: str


def normalize_cli_output(output: str) -> str:
    """Remove terminal controls and normalize line endings for evidence parsing."""
    return ANSI_ESCAPE.sub("", output).replace("\r\n", "\n").replace("\r", "\n")


def run_cli(command: list[str]) -> CliResult:
    """Run CubeProgrammer, preserve output order, and echo sanitized evidence."""
    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    output = normalize_cli_output(completed.stdout or "")
    if output:
        sys.stdout.write(output)
        if not output.endswith("\n"):
            sys.stdout.write("\n")
        sys.stdout.flush()
    return CliResult(completed.returncode, output)


def require_strict_closed_discovery(output: str) -> None:
    """Fail closed unless one exact, internally consistent H563 DA record exists."""
    normalized = normalize_cli_output(output)
    for name, (pattern, expected) in DISCOVERY_FIELDS.items():
        values = pattern.findall(normalized)
        if len(values) != 1:
            raise RuntimeError(
                f"DA discovery must contain exactly one {name}; found {len(values)}"
            )
        if values[0] != expected:
            raise RuntimeError(
                f"DA discovery {name} is {values[0]!r}; expected {expected!r}"
            )


def discover_strict_closed(connect: list[str]) -> CliResult:
    result = run_cli(connect + ["debugauth=2"])
    if result.returncode != 0:
        raise RuntimeError(
            f"DA discovery failed with exit code {result.returncode}"
        )
    require_strict_closed_discovery(result.output)
    return result


def verify_assets(pki: Path, assets: Path) -> tuple[bytes, ec.EllipticCurvePrivateKey]:
    manifest_path = assets / "security-assets.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (manifest.get("obk_package_device_bound") is not False or
            manifest.get("debug_reopening") is not True or
            manifest.get("debug_scope") != "HDPL3 secure and nonsecure" or
            manifest.get("full_regression") is not True or
            manifest.get("partial_regression") is not False or
            manifest.get("permission_mask") != FIELD_SERVICE_PERMISSION):
        raise RuntimeError("security-assets.json does not contain the reviewed DA policy")
    for name, expected in manifest["files"].items():
        path = assets / name
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise RuntimeError(f"security asset hash mismatch: {name}")

    password = (pki / "key-passphrase.txt").read_bytes().strip()
    key = serialization.load_pem_private_key(
        (pki / "da-leaf.pem").read_bytes(), password=password
    )
    if not isinstance(key, ec.EllipticCurvePrivateKey) or \
            key.curve.name != "secp256r1":
        raise TypeError("DA leaf private key is not P-256")
    expected_public = serialization.load_pem_public_key(
        (pki / "da-leaf-public.pem").read_bytes()
    )
    if not isinstance(expected_public, ec.EllipticCurvePublicKey) or \
            key.public_key().public_numbers() != expected_public.public_numbers():
        raise RuntimeError("DA leaf private/public keys do not match")
    return password, key


def clear_private_key(key: ec.EllipticCurvePrivateKey, destination: Path) -> None:
    destination.write_bytes(key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.TraditionalOpenSSL,
        serialization.NoEncryption(),
    ))
    destination.chmod(stat.S_IRUSR | stat.S_IWUSR)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=(
        "verify-assets", "discover", "open-app-debug", "close-debug",
        "full-regression-to-open",
    ))
    parser.add_argument("--programmer", type=Path, default=DEFAULT_PROGRAMMER)
    parser.add_argument("--pki-dir", type=Path, default=DEFAULT_PKI)
    parser.add_argument("--assets-dir", type=Path, default=DEFAULT_ASSETS)
    parser.add_argument("--probe", default=DEFAULT_PROBE)
    parser.add_argument(
        "--accept-full-device-erase", action="store_true",
        help="required for full regression; erases Flash, OBKeys and secure storage",
    )
    parser.add_argument(
        "--accept-closed-target", action="store_true",
        help=(
            "required for DA discovery/open/close/full regression; "
            "do not use on an OPEN target"
        ),
    )
    args = parser.parse_args(argv)

    if args.action in (
            "discover", "open-app-debug", "close-debug",
            "full-regression-to-open") and not args.accept_closed_target:
        parser.error(
            "this action triggers RSS-DA and requires a CLOSED target; "
            "add --accept-closed-target"
        )
    if args.action == "full-regression-to-open" and \
            not args.accept_full_device_erase:
        parser.error(
            "full regression destroys all Flash, OBKeys and secure storage; "
            "add --accept-full-device-erase"
        )

    if not args.programmer.is_file():
        raise FileNotFoundError(args.programmer)
    _, leaf_key = verify_assets(args.pki_dir, args.assets_dir)
    if args.action == "verify-assets":
        print(f"DA assets verified: permission={FIELD_SERVICE_PERMISSION}")
        return 0

    connect = [
        str(args.programmer), "-c", "port=SWD", f"sn={args.probe}",
        "speed=fast",
    ]
    if args.action == "discover":
        discover_strict_closed(connect)
        return 0
    if args.action == "close-debug":
        result = run_cli(connect + ["debugauth=3"])
        if result.returncode != 0:
            raise RuntimeError(
                f"close-debug failed with exit code {result.returncode}"
            )
        if "Locking Debug" not in result.output:
            raise RuntimeError(
                "close-debug did not report the required 'Locking Debug' evidence"
            )
        # Discovery is information-only.  It proves that debug was re-locked
        # without granting a debug permission; a cold power cycle is still
        # required to leave RSS-DA before returning the ECU to service.
        discover_strict_closed(connect)
        print(
            "Debug is locked and strict CLOSED discovery passed. "
            "Fully remove ECU power for at least 10 seconds, "
            "then verify Ethernet, ATECC authentication, and zero outputs; "
            "NRST alone may leave this power cycle in RSS-DA."
        )
        return 0

    # Authentication and destructive full regression are only permitted after
    # exact identification of this reviewed CLOSED STM32H563 DA configuration.
    discover_strict_closed(connect)

    temporary_root = Path("/dev/shm") if Path("/dev/shm").is_dir() else None
    with tempfile.TemporaryDirectory(
            prefix="roller-ecu-da-", dir=temporary_root) as temp_name:
        clear_key = Path(temp_name) / "da-leaf-clear.pem"
        clear_private_key(leaf_key, clear_key)
        # CubeProgrammer's symbolic permissions are not the permission bit
        # numbers.  For STM32H563, `c` requests bit 6 (HDPL3 S/NS intrusive
        # debug); `g` requests bit 1 (HDPL2 nonsecure debug) and is outside
        # this product's reviewed 0x4040 policy.
        permission = "c" if args.action == "open-app-debug" else "a"
        command = connect + [
            f"per={permission}", f"key={clear_key}",
            f"cert={args.assets_dir / 'cert-leaf-chain.b64'}", "debugauth=1",
        ]
        try:
            result = run_cli(command)
        finally:
            # Best-effort overwrite before TemporaryDirectory removes the file,
            # including when spawning CubeProgrammer itself fails.
            try:
                clear_key.write_bytes(os.urandom(clear_key.stat().st_size))
            except OSError:
                pass
        if result.returncode != 0:
            raise RuntimeError(
                f"Debug Authentication failed with exit code {result.returncode}"
            )
        if args.action == "open-app-debug" and \
                "Authentication Success" not in result.output:
            raise RuntimeError(
                "open-app-debug did not report the required "
                "'Authentication Success' evidence"
            )
    if args.action == "open-app-debug":
        print("Temporary HDPL3 Secure+NonSecure debug is open until close-debug or power-off")
    else:
        print("Full regression requested: device contents are erased and product state returns OPEN")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"Debug Authentication failed: {error}", file=sys.stderr)
        raise SystemExit(1)
