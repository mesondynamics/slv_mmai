#!/usr/bin/env python3
"""Prepare/install the explicitly reviewed SN-EJAHGJQ OPEN boot-chain transaction.

This tool cannot set PRODUCT_STATE, perform DA regression, or create root keys.
An immutable staged input set and a verified second copy are mandatory before
the OPEN-only Flash rebuild. OBKey provisioning and CLOSED acceptance are
separate hardware-gated phases; successful installation here is not delivery.
The provision-obkeys phase remains OPEN and stops for JP1 removal; it does
not prove CLOSED Debug Authentication or authorize a lifecycle transition.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from cryptography.hazmat.primitives import serialization
from check_device_profile import read_profile
from ecu_debug_auth import normalize_cli_output, verify_assets
from ecu_readonly_snapshot import capture, require_paired_safe
from ethernet_ota import load_package
from package_firmware import IMGTOOL, strict_json_object
from verify_open_loader_backup import parse_primary, ReleaseIdentity
from verify_pairing_store import REVIEWED_IDENTITIES, verify_dual_store

PROJECT = Path(__file__).resolve().parents[1]
SERIAL = "SN-EJAHGJQ"
IDENTITY = REVIEWED_IDENTITIES[SERIAL]
PROBE = "066BFF565456857187210935"
CLI = Path("/home/plac/.local/share/stm32cube/bundles/programmer/2.23.0/bin/STM32_Programmer_CLI")
PKI = Path("/home/plac/.local/share/roller-ecu-pki")
USER_BACKUP = Path("/home/plac/Documents/ECU_PKI")
VERSION = "1.0.21"
SCHEMA = "roller-ecu-open-manufacturing-v1"
# Immutable artifacts reviewed for this exact board/release, not values learned
# from whichever binary happens to occupy a mutable build directory.
PINNED = {
    "ReleaseOpen-ECU_OEMiROT.bin": (52276, "e9c19356c184feb0b0d104b6384a2667f0db364548f6f683ee35a18a11fae239"),
    "ReleaseClosed-ECU_OEMiROT.bin": (52292, "135f5b4e07e5e1ebe714d247be711ae217231a953dbf977b99dad9cfb2914c10"),
    "secure-initial.bin": (196608, "2e15c823f25dcaf0851c49ba7b24cd60a98f088ad92c349a3fef1c9f94d67cd4"),
    "nonsecure-initial.bin": (327680, "7e96d665a7875018e2e5b4a6f2f4a6717afd2d9147d68ef1ff87ea32d30cf3a3"),
    "roller-ecu-1.0.21.recu": (525371, "cfad6ea6fc8336f4e79c6a0a3c0e5a7960808ff61ef2516dac65edde0146d418"),
    "metadata.json": (367, "ae5719640fa42161733a57084d56331f0a07690a58e32010cc5323478ded2915"),
}
PINNED_OBKS = {
    "DA_Config.obk": (108, "4a9fd310af6ca1b56ab4031695104fc8c78fe271c7c117b074849fb220e7582e"),
    "OEMiRoT_Config.obk": (300, "91faa2578ab1b92badf916f0ebf99647edec15ed6add48000f703f3c61a64f8d"),
    "OEMiRoT_Data.obk": (204, "58cc08d4ab62f378f3cd93fadc04fec5135593116abee8cc1afe06f22c478681"),
}
ASSET_NAMES = ("DA_Config.obk", "OEMiRoT_Config.obk", "OEMiRoT_Data.obk",
               "cert-root.b64", "cert-intermediate.b64", "cert-leaf.b64",
               "cert-leaf-chain.b64", "security-assets.json")
PUBLIC_NAMES = ("oemirot-auth-s-public.pem", "oemirot-auth-ns-public.pem",
                "oemirot-encryption-public.pem", "ota-transport-public.pem")
EVIDENCE_NAMES = ("factory-flash.bin", "secure-persistent.bin", "pairing-store.bin",
                  "pairing-audit.json", "initial-image-audit.json", "obk-reproduction.json")
TARGET = dict(serial=SERIAL, mcu_uid="003900443434511232383537",
              atecc_serial="01236acf4e275ef9ee", config_crc32c=0x6165A970,
              pairing_generation=1, ecu_ip="172.16.0.21", domain_ip="172.16.0.22",
              probe=PROBE, product_state="OPEN", permission_mask="0x00004040")
INSTALL_PHASES = (
    "exact_open_target_and_physical_estop_verified",
    "fresh_full_flash_and_dual_pairing_match_verified_backup",
    "open_flash_rebuild_started", "open_flash_erased_backups_preserved",
    "verified_secure-initial.bin", "verified_nonsecure-initial.bin",
    "verified_secure-persistent.bin", "verified_ReleaseOpen-ECU_OEMiROT.bin",
    "boot_write_and_hide_protection_enabled_awaiting_jp1_rss",
)
OBK_STEPS = (("da_config", "DA_Config.obk"),
             ("oemirot_config", "OEMiRoT_Config.obk"),
             ("oemirot_data", "OEMiRoT_Data.obk"))
OBK_PHASES = ("rss_target_and_installed_images_verified",) + tuple(
    f"{stem}_{suffix}" for stem, _ in OBK_STEPS
    for suffix in ("submission_started", "provisioned")) + ("awaiting_jp1_open",)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(data: bytes):
    return hashlib.sha256(data).hexdigest()


def regular_bytes(path: Path):
    require(path.is_file() and not path.is_symlink(), f"not a regular input: {path}")
    return path.read_bytes()


def write_new(path: Path, data: bytes):
    with path.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    path.chmod(0o600)


def write_json(path: Path, data):
    write_new(path, (json.dumps(data, indent=2, sort_keys=True) + "\n").encode())


def verify_pki_backup():
    # Require all existing key pairs. Never call key generation to fill gaps.
    names = ["key-passphrase.txt"]
    stems = ("da-root", "da-intermediate", "da-leaf", "oemirot-auth-s",
             "oemirot-auth-ns", "oemirot-encryption", "ota-transport")
    names += [stem + suffix for stem in stems for suffix in (".pem", "-public.pem")]
    for name in names:
        require(regular_bytes(PKI / name) == regular_bytes(USER_BACKUP / "roller-ecu-pki" / name),
                f"shared PKI/backup mismatch: {name}")
    password = (PKI / "key-passphrase.txt").read_bytes().strip()
    for stem in stems:
        key = serialization.load_pem_private_key((PKI / f"{stem}.pem").read_bytes(), password)
        pub = serialization.load_pem_public_key((PKI / f"{stem}-public.pem").read_bytes())
        require(key.public_key().public_numbers() == pub.public_numbers(),
                f"shared private/public mismatch: {stem}")


def verify_staged(root: Path):
    require(root.is_dir() and not root.is_symlink(), "transaction must be a real directory")
    manifest = json.loads(regular_bytes(root / "manifest.json"), object_pairs_hook=strict_json_object)
    require(set(manifest) == {"schema", "target", "release", "files"} and
            manifest["schema"] == SCHEMA and manifest["target"] == TARGET and
            manifest["release"] == dict(version=VERSION, counter=21, sequence=21),
            "transaction target/release/schema mismatch")
    expected_names = set(PINNED) | set(ASSET_NAMES) | set(PUBLIC_NAMES) | set(EVIDENCE_NAMES)
    require(set(manifest["files"]) == expected_names, "incomplete/extraneous staged file set")
    for name, entry in manifest["files"].items():
        data = regular_bytes(root / name)
        require(entry == {"size": len(data), "sha256": digest(data)}, f"staged file changed: {name}")
        if name in PINNED:
            require((len(data), digest(data)) == PINNED[name], f"unreviewed release artifact: {name}")
        if name in PINNED_OBKS:
            require((len(data), digest(data)) == PINNED_OBKS[name], f"unreviewed shared OBK: {name}")
    flash = regular_bytes(root / "factory-flash.bin")
    persistent = regular_bytes(root / "secure-persistent.bin")
    paired = regular_bytes(root / "pairing-store.bin")
    require(len(flash) == 0x200000 and persistent == flash[0xE0000:0x100000] and
            paired == persistent[:0x4000], "factory/persistent/pairing evidence mismatch")
    verify_dual_store(paired, identity=IDENTITY)
    for name, size, offset in (("secure", 0x30000, 0x30000), ("nonsecure", 0x50000, 0x100000)):
        image = (root / f"{name}-initial.bin").read_bytes()
        report = parse_primary(name, image, offset, size)
        require(report.identity == ReleaseIdentity(1, 0, 21, 0, 21), "initial identity mismatch")
    _, _, _, metadata = load_package(root / f"roller-ecu-{VERSION}.recu", root / "ota-transport-public.pem")
    require(metadata["version"] == VERSION and metadata["update_sequence"] == 21 and
            metadata["security_counter"] == 21, "signed OTA identity mismatch")
    return manifest


def prepare(root: Path, factory_flash: Path):
    require(not root.exists(), "transaction already exists; use verify, never overwrite it")
    profile = read_profile()
    require(profile["serial"] == SERIAL and profile["mcu_uid"] == TARGET["mcu_uid"],
            "active source profile is not the reviewed new board")
    verify_pki_backup()
    assets = PROJECT / "artifacts/security-provisioning"
    verify_assets(PKI, assets)
    flash = regular_bytes(factory_flash)
    require(len(flash) == 0x200000, "factory full-Flash backup must be 2 MiB")
    pairing_report = verify_dual_store(flash[0xE0000:0xE4000], identity=IDENTITY)
    root.mkdir(parents=True, mode=0o700)
    sources = {name: PROJECT / "artifacts/firmware" / VERSION / name for name in PINNED}
    for profile_name in ("ReleaseOpen", "ReleaseClosed"):
        sources[f"{profile_name}-ECU_OEMiROT.bin"] = (
            PROJECT / "Bootloader/OEMiROT/build" / profile_name / "ECU_OEMiROT.bin")
    sources.update({name: assets / name for name in ASSET_NAMES})
    sources.update({name: PKI / name for name in PUBLIC_NAMES})
    for name, path in sources.items():
        data = regular_bytes(path)
        if name in PINNED:
            require((len(data), digest(data)) == PINNED[name], f"unreviewed artifact: {name}")
        write_new(root / name, data)
    write_new(root / "factory-flash.bin", flash)
    write_new(root / "secure-persistent.bin", flash[0xE0000:0x100000])
    write_new(root / "pairing-store.bin", flash[0xE0000:0xE4000])
    write_json(root / "pairing-audit.json", pairing_report)
    # Reproduce TPC OBKs from existing shared keys in a NEW scratch directory;
    # generated certificates are discarded, production certificates unchanged.
    with tempfile.TemporaryDirectory(prefix="ecu-obk-reproduce-") as temporary:
        result = subprocess.run([sys.executable, str(PROJECT / "tools/generate_security_provisioning.py"),
                                 "--pki-dir", str(PKI), "--output-dir", temporary],
                                capture_output=True, text=True, timeout=60)
        require(result.returncode == 0, "offline OBK reproduction failed; no hardware was written")
        hashes = {}
        for name in ASSET_NAMES[:3]:
            regenerated = regular_bytes(Path(temporary) / name)
            require(regenerated == (root / name).read_bytes(), f"OBK differs from shared PKI: {name}")
            hashes[name] = digest(regenerated)
        write_json(root / "obk-reproduction.json", dict(result="PASS", sha256=hashes,
                                                        permission_mask="0x00004040"))
    reports = {}
    for name, key, size, offset in (("secure", "s", 0x30000, 0x30000),
                                    ("nonsecure", "ns", 0x50000, 0x100000)):
        path = root / f"{name}-initial.bin"
        env = dict(os.environ, PYTHONPATH=str(PROJECT / "tools/imgtool_compat"))
        result = subprocess.run([sys.executable, str(IMGTOOL), "verify", "-k",
                                 str(root / f"oemirot-auth-{key}-public.pem"), str(path)],
                                env=env, capture_output=True, text=True, timeout=20)
        require(result.returncode == 0 and "valid" in result.stdout.lower(),
                f"{name} initial signature failed")
        reports[name] = asdict(parse_primary(name, path.read_bytes(), offset, size))
    write_json(root / "initial-image-audit.json", reports)
    names = set(PINNED) | set(ASSET_NAMES) | set(PUBLIC_NAMES) | set(EVIDENCE_NAMES)
    manifest = dict(schema=SCHEMA, target=TARGET, release=dict(version=VERSION, counter=21, sequence=21),
                    files={name: dict(size=(root / name).stat().st_size,
                                      sha256=digest((root / name).read_bytes())) for name in sorted(names)})
    write_json(root / "manifest.json", manifest)
    verify_staged(root)
    destination = USER_BACKUP / root.name
    require(not destination.exists(), "second-copy destination already exists")
    shutil.copytree(root, destination)
    verify_staged(destination)
    require((root / "manifest.json").read_bytes() == (destination / "manifest.json").read_bytes(),
            "second-copy manifest mismatch")
    os.sync()
    print(f"Prepared and dual-copy verified: {root}\nSecond copy: {destination}", flush=True)


def require_open_options(output: str):
    required = {"PRODUCT_STATE": 0xED, "TZEN": 0xB4, "BOOT_UBE": 0xB4,
                "IWDG_SW": 1, "SWAP_BANK": 0, "EDATA1_EN": 0, "EDATA2_EN": 0}
    for field, expected in required.items():
        matches = re.findall(rf"^\s*{field}\s*:\s*0x([0-9a-fA-F]+)\b", output, re.M)
        require(len(matches) == 1 and int(matches[0], 16) == expected, f"unsafe option: {field}")
    require(re.search(r"Device ID\s*:\s*0x484\b", output), "not the reviewed H563")
    require(re.search(r"Revision ID\s*:\s*Rev X\b", output), "unreviewed silicon revision")
    require("STM32CubeProgrammer v2.23.0" in output, "unreviewed Programmer version")


def require_rss_options(output: str):
    require_open_options(output)
    require_programmer_uid(output)
    for pattern in (r"^\s*BL Version\s*:\s*0xE4\s*$",
                    r"^\s*SFSP Version\s*:\s*v2\.5\.0\s*$"):
        require(len(re.findall(pattern, output, re.M)) == 1,
                "ROM RSS/SFSP version is not the reviewed OPEN provisioning environment")
    required = dict(SECBOOT_LOCK=0xB4, SECBOOTADD=0xC0000,
                    SECWM1_STRT=0, SECWM1_END=0x7F, SECWM2_STRT=1, SECWM2_END=0,
                    WRPSGn1=0xFFFFFFF0, WRPSGn2=0xFFFFFFFF,
                    HDP1_STRT=0, HDP1_END=0x17, HDP2_STRT=1, HDP2_END=0,
                    SRAM2_RST=0, SRAM2_ECC=0)
    for field, expected in required.items():
        values = re.findall(rf"^\s*{field}\s*:\s*0x([0-9a-fA-F]+)\b", output, re.M)
        require(len(values) == 1 and int(values[0], 16) == expected,
                f"installed OPEN protection differs: {field}")


def require_programmer_uid(output: str):
    matches = re.findall(r"^0x08FFF800\s*:\s*([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s*$", output, re.M)
    require(len(matches) == 1 and "".join(matches[0]).lower() == TARGET["mcu_uid"],
            "direct hardware UID is not SN-EJAHGJQ")


def phase(root: Path, name: str):
    with (root / "install-phase.txt").open("a") as stream:
        stream.write(name + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    print(f"OPEN INSTALL: {name}", flush=True)


def programmer(root: Path, name: str, args: list[str], *, reset=True):
    command = [str(CLI), "-c", "port=SWD", f"sn={PROBE}", "ap=1"]
    command += ["mode=UR", "reset=HWrst"] if reset else ["mode=Hotplug"]
    # Reject reused log paths BEFORE any possible hardware mutation.
    require(not (root / f"{name}.log").exists(), "operation log exists; replay forbidden")
    try:
        result = subprocess.run(command + args, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=55)
    except subprocess.TimeoutExpired as error:
        partial = error.stdout or b""
        if isinstance(partial, bytes):
            partial = partial.decode(errors="replace")
        write_new(root / f"{name}.log", normalize_cli_output(partial).encode())
        raise RuntimeError(f"Programmer timed out ({name}); outcome ambiguous, replay forbidden") from error
    output = normalize_cli_output(result.stdout)
    write_new(root / f"{name}.log", output.encode())
    require(result.returncode == 0 and not re.search(r"(^|\n)\s*Error\b", output, re.I),
            f"Programmer failed ({name}); see durable log; do not blindly rerun")
    return output


def require_obkey_success(output: str):
    require(not re.search(r"\b(error|failed|failure)\b", output, re.I) and
            len(re.findall(r"^\s*OBKey Provisioned successfully\s*$", output, re.M)) == 1,
            "OBKey submission has no unambiguous successful provisioning receipt")


def append_obk_phase(root: Path, name: str):
    path = root / "obkeys-phase.txt"
    text = path.read_text() if path.exists() else ""
    phases = tuple(text.splitlines())
    require((not text or text.endswith("\n")) and
            phases == OBK_PHASES[:len(phases)] and
            len(phases) < len(OBK_PHASES) and OBK_PHASES[len(phases)] == name,
            "OBKey phase order is invalid; do not automatically replay")
    # An append interrupted mid-line is invalid on the next invocation.
    with path.open("a") as stream:
        stream.write(name + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    print(f"OPEN OBKEY: {name}", flush=True)


def wait_open_rss(root: Path, label: str):
    # Only retry a read-only connection probe; never retry SDP or wrong-UID /
    # unsafe-lifecycle results. A fresh exact probe is required after NRST.
    for attempt in range(10):
        try:
            output = programmer(root, f"{label}-rss-{attempt:02d}",
                                ["-ob", "displ", "-r32", "0x08FFF800", "12"], reset=False)
        except RuntimeError:
            if attempt == 9:
                raise
            time.sleep(0.2)
            continue
        require_rss_options(output)
        return
    raise RuntimeError("ROM RSS unavailable")


def provision_obkeys(root: Path):
    verify_staged(root)
    second = USER_BACKUP / root.name
    verify_staged(second)
    require(regular_bytes(root / "manifest.json") == regular_bytes(second / "manifest.json"),
            "independent transaction backup differs")
    verify_pki_backup()
    verify_assets(PKI, root)
    expected_install = ("\n".join(INSTALL_PHASES) + "\n").encode()
    require(regular_bytes(root / "install-phase.txt") == expected_install and
            regular_bytes(second / "install-phase.txt") == expected_install,
            "OPEN install and second-copy journals are not complete")
    require(not (root / "obkeys-phase.txt").exists() and not (root / "obkeys").exists(),
            "OBKey transaction already started; inspect exact receipts; replay forbidden")
    evidence = root / "obkeys"
    evidence.mkdir(mode=0o700)
    initial = programmer(evidence, "00-identity-before-reset",
                         ["-ob", "displ", "-r32", "0x08FFF800", "12"], reset=False)
    require_open_options(initial)
    require_programmer_uid(initial)
    # User holds JP1 high and ESTOP_NC disconnected. NRST samples BOOT0 even
    # if the user only changed the jumper without removing power.
    programmer(evidence, "00-enter-rss-reset", ["-hardRst"], reset=False)
    wait_open_rss(evidence, "01-before-readback")
    # Bind the live board to the exact installed images AND its own pairing
    # sectors before putting any keys into hardware-backed storage.
    for name, address, size, expected in (
            ("boot", "0x0C000000", 52276, root / "ReleaseOpen-ECU_OEMiROT.bin"),
            ("secure", "0x0C030000", 0x30000, root / "secure-initial.bin"),
            ("nonsecure", "0x08100000", 0x50000, root / "nonsecure-initial.bin"),
            ("pairing", "0x0C0E0000", 0x4000, root / "pairing-store.bin")):
        path = evidence / f"{name}-before-obkeys.bin"
        programmer(evidence, f"02-readback-{name}",
                   ["-u", address, hex(size), str(path)], reset=False)
        require(regular_bytes(path) == regular_bytes(expected), f"live installed {name} differs")
    verify_dual_store((evidence / "pairing-before-obkeys.bin").read_bytes(), identity=IDENTITY)
    append_obk_phase(root, OBK_PHASES[0])
    for stem, name in OBK_STEPS:
        programmer(evidence, f"{stem}-reset", ["-hardRst"], reset=False)
        wait_open_rss(evidence, stem)
        data = regular_bytes(root / name)
        require((len(data), digest(data)) == PINNED_OBKS[name], f"OBKey changed before submission: {name}")
        append_obk_phase(root, f"{stem}_submission_started")
        output = programmer(evidence, f"{stem}-sdp", ["-sdp", str(root / name)], reset=False)
        require_obkey_success(output)
        write_json(evidence / f"{stem}-receipt.json", dict(
            result="PASS", returncode=0, obk_sha256=digest(data),
            log_sha256=digest(output.encode()), target=TARGET))
        append_obk_phase(root, f"{stem}_provisioned")
    programmer(evidence, "final-rss-reset", ["-hardRst"], reset=False)
    wait_open_rss(evidence, "final-open")
    append_obk_phase(root, "awaiting_jp1_open")
    os.sync()
    print("All three OPEN OBKeys have explicit successful receipts. MCU remains OPEN.\n"
          "Power off, OPEN JP1, keep ESTOP_NC disconnected and ST-Link/NRST connected, power on.", flush=True)


def install(root: Path):
    verify_staged(root)
    second = USER_BACKUP / root.name
    verify_staged(second)
    require(regular_bytes(root / "manifest.json") == regular_bytes(second / "manifest.json"),
            "independent backup differs")
    verify_pki_backup()
    verify_assets(PKI, root)
    require(not (root / "install-phase.txt").exists(),
            "installation already started; inspect journal before recovery; automatic replay forbidden")
    state = capture(SERIAL)
    require_paired_safe(state, SERIAL, accepted_sequence=0)
    write_json(root / "runtime-before-install.json", state)
    options = programmer(root, "01-open-identity", ["-ob", "displ", "-r32", "0x08FFF800", "12"])
    require_open_options(options)
    require_programmer_uid(options)
    phase(root, "exact_open_target_and_physical_estop_verified")
    # Re-read the ACTUAL whole chip immediately before mutation. A changed
    # persistent journal, other factory image, or wrong board fails closed.
    programmer(root, "02-fresh-full-backup", ["-u", "0x08000000", "0x200000", str(root / "fresh-factory-flash.bin")])
    fresh = regular_bytes(root / "fresh-factory-flash.bin")
    require(fresh == (root / "factory-flash.bin").read_bytes(),
            "live Flash differs from the independently backed-up factory snapshot")
    verify_dual_store(fresh[0xE0000:0xE4000], identity=IDENTITY)
    phase(root, "fresh_full_flash_and_dual_pairing_match_verified_backup")
    phase(root, "open_flash_rebuild_started")
    programmer(root, "03-erase-open-flash", [
        "-ob", "SECWM1_STRT=0x1", "SECWM1_END=0x0", "SECWM2_STRT=0x1", "SECWM2_END=0x0",
        "WRPSGn1=0xFFFFFFFF", "WRPSGn2=0xFFFFFFFF", "HDP1_STRT=0x1", "HDP1_END=0x0",
        "HDP2_STRT=0x1", "HDP2_END=0x0", "SECBOOT_LOCK=0xC3", "SECBOOTADD=0xC0000",
        "SWAP_BANK=0", "SRAM2_RST=0", "SRAM2_ECC=0", "SRAM3_ECC=1", "BOOT_UBE=0xB4",
        "-e", "all"])
    phase(root, "open_flash_erased_backups_preserved")
    programmer(root, "04-watermarks", ["-ob", "SECWM1_STRT=0x0", "SECWM1_END=0x7F",
                                       "SECWM2_STRT=0x1", "SECWM2_END=0x0"])
    for index, name, address in ((5, "secure-initial.bin", "0x0C030000"),
                                 (6, "nonsecure-initial.bin", "0x08100000"),
                                 (7, "secure-persistent.bin", "0x0C0E0000"),
                                 (8, "ReleaseOpen-ECU_OEMiROT.bin", "0x0C000000")):
        output = programmer(root, f"{index:02d}-write-{name}", ["-d", str(root / name), address, "-v"])
        require("Download verified successfully" in output, f"missing readback verification: {name}")
        phase(root, f"verified_{name}")
    programmer(root, "09-readback-pairing", ["-u", "0x0C0E0000", "0x4000", str(root / "restored-pairing.bin")])
    require(regular_bytes(root / "restored-pairing.bin") == (root / "pairing-store.bin").read_bytes(),
            "restored pairing differs")
    verify_dual_store((root / "restored-pairing.bin").read_bytes(), identity=IDENTITY)
    programmer(root, "10-boot-protection", ["-ob", "WRPSGn1=0xFFFFFFF0", "WRPSGn2=0xFFFFFFFF",
                                             "HDP1_STRT=0x0", "HDP1_END=0x17", "HDP2_STRT=0x1",
                                             "HDP2_END=0x0", "SECBOOT_LOCK=0xB4"])
    phase(root, "boot_write_and_hide_protection_enabled_awaiting_jp1_rss")
    # No PRODUCT_STATE write and no reset-to-app: OBKs are not provisioned yet.
    os.sync()
    print("OPEN images restored and verified. Keep physical E-stop disconnected.\n"
          "Power off ECU, short JP1, keep ST-Link/NRST connected, power on for RSS OBKey phase.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "verify", "install", "provision-obkeys"))
    parser.add_argument("--transaction", type=Path, required=True)
    parser.add_argument("--factory-flash", type=Path)
    parser.add_argument("--accept-open-flash-rebuild", action="store_true")
    parser.add_argument("--accept-boot0-high-estop-disconnected", action="store_true")
    args = parser.parse_args()
    os.umask(0o077)
    root = args.transaction.absolute()
    require(root.is_absolute() and root.name.startswith("roller-ecu-SN-EJAHGJQ-open-"),
            "use an explicit new-board transaction path")
    if args.action == "prepare":
        require(args.factory_flash is not None, "prepare requires --factory-flash")
        prepare(root, args.factory_flash)
    elif args.action == "verify":
        verify_staged(root)
        print("Exact new-board staged transaction verified; no hardware accessed")
    else:
        if args.action == "install":
            require(args.accept_open_flash_rebuild, "install requires --accept-open-flash-rebuild")
        else:
            require(args.accept_boot0_high_estop_disconnected,
                    "provision-obkeys requires --accept-boot0-high-estop-disconnected")
        # Same lock as the historical CLOSED writer; board-specific tools
        # must not interleave lifecycle commands on this shared probe.
        with Path("/tmp/roller-ecu-closed-transition.lock").open("a+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if args.action == "install":
                install(root)
            else:
                provision_obkeys(root)


if __name__ == "__main__":
    main()
