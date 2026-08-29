#!/usr/bin/env python3
"""Generate product OEMiROT and regression-only Debug Authentication assets."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import stat
import subprocess
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec


PROJECT = Path(__file__).resolve().parents[1]
CUBE_H5 = Path("/home/plac/STM32Cube/Repository/STM32Cube_FW_H5_V1.7.0")
PROGRAMMER_BIN = Path(
    "/home/plac/Applications/STMicroelectronics/STM32Cube/"
    "STM32CubeProgrammer/bin"
)
TPC = PROGRAMMER_BIN / "STM32TrustedPackageCreator_CLI"
PSA_ADAC_SOURCE = Path(
    "/home/plac/Applications/STM32CubeMX/utilities/"
    "STM32TrustedPackageCreator/bin/Utilities/Linux/PSA_ADAC"
)
DEFAULT_PKI = Path("/home/plac/.local/share/roller-ecu-pki")
DEFAULT_OUTPUT = PROJECT / "artifacts/security-provisioning"
FULL_REGRESSION_ONLY = 0x00004000


def private_bytes(key: ec.EllipticCurvePrivateKey, password: bytes) -> bytes:
    return key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.BestAvailableEncryption(password),
    )


def public_bytes(key: ec.EllipticCurvePrivateKey) -> bytes:
    return key.public_key().public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )


def ensure_da_key(pki: Path, stem: str, password: bytes) -> None:
    private_path = pki / f"{stem}.pem"
    public_path = pki / f"{stem}-public.pem"
    if private_path.exists() != public_path.exists():
        raise RuntimeError(f"incomplete DA key pair: {stem}")
    if not private_path.exists():
        key = ec.generate_private_key(ec.SECP256R1())
        private_path.write_bytes(private_bytes(key, password))
        public_path.write_bytes(public_bytes(key))
        private_path.chmod(stat.S_IRUSR | stat.S_IWUSR)
        public_path.chmod(stat.S_IRUSR | stat.S_IWUSR)


def load_private(path: Path, password: bytes) -> ec.EllipticCurvePrivateKey:
    key = serialization.load_pem_private_key(path.read_bytes(), password=password)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        raise TypeError(f"{path} is not a P-256 private key")
    return key


def write_clear_key(source: Path, destination: Path, password: bytes) -> None:
    key = load_private(source, password)
    destination.write_bytes(key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.TraditionalOpenSSL,
        serialization.NoEncryption(),
    ))
    destination.chmod(stat.S_IRUSR | stat.S_IWUSR)


def obk_xml(title: str, destination: str, output: Path, body: str,
            *, obk_start: str | None = None, obk_size: str | None = None) -> str:
    range_fields = ""
    if obk_start is not None and obk_size is not None:
        range_fields = f"""
        <FlashStart>0x0C000000</FlashStart><FlashSize>0x200000</FlashSize>
        <FlashSectorSize>0x2000</FlashSectorSize>
        <OBKStart>{obk_start}</OBKStart><OBKSize>{obk_size}</OBKSize>
        <OBKSectorSize>0x10</OBKSectorSize>
        <AuthOBKStart>{obk_start}</AuthOBKStart><AuthOBKSize>{obk_size}</AuthOBKSize>"""
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<Root><Obdata><Info><Title>{title}</Title>
<ObDestAddress>{destination}</ObDestAddress><DoEncryption>0</DoEncryption>
<GlobalAlign>16</GlobalAlign>{range_fields}</Info><Hash></Hash>{body}
<Output><Name>Output File</Name><Value>{output}</Value>
<Default>{output}</Default><Tooltip>Generated ECU product OBKey</Tooltip></Output>
</Obdata></Root>
"""


def file_element(name: str, value: Path, file_type: str = "Public") -> str:
    return f"""<File><Name>{name}</Name><Value>{value}</Value><Align>4</Align>
<KeyType>ecdsa-p256</KeyType><Type>{file_type}</Type><Default>{value}</Default>
<Tooltip>Roller ECU product key</Tooltip></File>"""


def run(command: list[str]) -> None:
    subprocess.run(command, check=True)


def certificate_yaml(name: Path, issuer: Path, subject: Path, role: int) -> str:
    return f"""name: {name}
issuer-key: {issuer}
subject-key: {subject}
role: {role}
usage: 1
lifecycle: 0x0000
oem_constraint: 0
soc_class: 0
soc_id: 0
permissions_mask: 0x00000000_00000000_00000000_{FULL_REGRESSION_ONLY:08x}
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pki-dir", type=Path, default=DEFAULT_PKI)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    if not TPC.is_file() or not PSA_ADAC_SOURCE.is_file():
        raise FileNotFoundError("STM32 security tooling is not installed")
    args.pki_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    password_path = args.pki_dir / "key-passphrase.txt"
    if not password_path.is_file():
        raise FileNotFoundError(password_path)
    password = password_path.read_bytes().strip()
    for stem in ("da-root", "da-intermediate", "da-leaf"):
        ensure_da_key(args.pki_dir, stem, password)
    args.output_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    outputs = [args.output_dir / name for name in (
        "OEMiRoT_Config.obk", "OEMiRoT_Data.obk", "DA_Config.obk",
        "cert-root.b64", "cert-intermediate.b64", "cert-leaf.b64",
        "cert-leaf-chain.b64", "security-assets.json")]
    if not args.force and any(path.exists() for path in outputs):
        raise FileExistsError("security output exists; use --force to regenerate")

    with tempfile.TemporaryDirectory(prefix="ecu-security-") as temp_name:
        temp = Path(temp_name)
        encryption_clear = temp / "oemirot-encryption.pem"
        write_clear_key(args.pki_dir / "oemirot-encryption.pem",
                        encryption_clear, password)
        config_xml = temp / "OEMiRoT_Config.xml"
        config_body = "".join((
            file_element("Authentication secure key",
                         args.pki_dir / "oemirot-auth-s-public.pem"),
            file_element("Authentication non secure key",
                         args.pki_dir / "oemirot-auth-ns-public.pem"),
            file_element("Encryption key", encryption_clear, ""),
        ))
        config_xml.write_text(obk_xml(
            "Roller ECU OEMiRoT configuration", "0x0FFD0160",
            args.output_dir / "OEMiRoT_Config.obk", config_body,
            obk_start="0xFFD0900", obk_size="0x2F0"))
        run([str(TPC), "-obk", str(config_xml)])

        da_xml = temp / "DA_Config.xml"
        # The empty DA tag selects TPC's H5 Debug Authentication transform:
        # store the root-key hash and permission mask, never the public key.
        da_body = "<DA></DA>" + file_element(
            "Debug Authentication root key",
            args.pki_dir / "da-root-public.pem") + f"""
<Permission><Name>Permission</Name><Value>0x{FULL_REGRESSION_ONLY:08x}</Value>
<Width>4</Width><Default>0x{FULL_REGRESSION_ONLY:08x}</Default>
<Tooltip>Full regression only; all debug reopening disabled</Tooltip></Permission>"""
        da_xml.write_text(obk_xml(
            "Roller ECU regression-only Debug Authentication", "0x0FFD0100",
            args.output_dir / "DA_Config.obk", da_body))
        run([str(TPC), "-obk", str(da_xml)])

        official_data = (CUBE_H5 / "Projects/NUCLEO-H563ZI/ROT_Provisioning/"
                         "OEMiROT/Binary/OEMiRoT_Data.obk")
        shutil.copyfile(official_data, args.output_dir / "OEMiRoT_Data.obk")

        psa_adac = temp / "PSA_ADAC"
        shutil.copyfile(PSA_ADAC_SOURCE, psa_adac)
        psa_adac.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        clear_keys: dict[str, Path] = {}
        for stem in ("root", "intermediate", "leaf"):
            clear = temp / f"da-{stem}.pem"
            write_clear_key(args.pki_dir / f"da-{stem}.pem", clear, password)
            clear_keys[stem] = clear
        cert_dir = temp / "certificates"
        cert_dir.mkdir()
        configs = (
            ("root", clear_keys["root"], args.pki_dir / "da-root-public.pem", 1),
            ("intermediate", clear_keys["root"],
             args.pki_dir / "da-intermediate-public.pem", 2),
            ("leaf", clear_keys["intermediate"],
             args.pki_dir / "da-leaf-public.pem", 3),
        )
        for name, issuer, subject, role in configs:
            yml = temp / f"{name}.yml"
            yml.write_text(certificate_yaml(cert_dir / name, issuer, subject, role))
            run([str(psa_adac), "sign", str(yml)])
            cert = cert_dir / f"{name}.cert"
            run([str(psa_adac), "chain", "-o", str(args.output_dir / f"cert-{name}.b64"),
                 str(cert)])
        run([str(psa_adac), "chain", "-o",
             str(args.output_dir / "cert-leaf-chain.b64"),
             str(cert_dir / "root.cert"), str(cert_dir / "intermediate.cert"),
             str(cert_dir / "leaf.cert")])
        decoded = subprocess.run(
            [str(psa_adac), "decode", str(args.output_dir / "cert-leaf-chain.b64")],
            check=True, text=True, capture_output=True).stdout
        if decoded.count("permissions_mask:") != 3 or decoded.count("00004000") != 3:
            raise RuntimeError("generated DA chain is not regression-only")

    expected_sizes = {"OEMiRoT_Config.obk": 300, "OEMiRoT_Data.obk": 204,
                      "DA_Config.obk": 108}
    for name, expected in expected_sizes.items():
        path = args.output_dir / name
        if path.stat().st_size != expected:
            raise RuntimeError(f"unexpected {name} size: {path.stat().st_size}")
    manifest = {
        "policy": "CLOSED with certificate-authenticated full regression only",
        "debug_reopening": False,
        "permission_mask": f"0x{FULL_REGRESSION_ONLY:08x}",
        "files": {path.name: sha256(path) for path in outputs[:-1]},
    }
    (args.output_dir / "security-assets.json").write_text(
        json.dumps(manifest, indent=2) + "\n")
    for path in outputs:
        path.chmod(stat.S_IRUSR | stat.S_IWUSR)
    print(f"Generated product security assets: {args.output_dir}")
    print("DA policy: full regression (mass erase) only; debug reopening disabled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
