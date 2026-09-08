#!/usr/bin/env python3
"""SN-EJAHGJQ OPEN-to-CLOSED transaction with immutable recovery gates.

The board's three OBKeys were already provisioned in OPEN with individual
successful receipts. This transaction never resubmits an OBKey. It installs
the reviewed production loader, enters PROVISIONING, requires authoritative
ROM DA provisioning-integrity evidence, and only then requests CLOSED.
LOCKED and regression commands are intentionally absent.
"""
from __future__ import annotations

import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import uuid

import ecu_debug_auth as da
import manufacture_open_device as m
import prepare_device_recovery as recovery
import verify_device_security_preflight as open_preflight

SCHEMA = "roller-ecu-SN-EJAHGJQ-closed-transition-v1"
RECOVERY = m.PROJECT / ("artifacts/device-backups/SN-EJAHGJQ/"
                        "roller-ecu-SN-EJAHGJQ-recovery-inputs-1.0.22-20260906")
MANUFACTURING = m.PROJECT / ("artifacts/device-backups/SN-EJAHGJQ/"
                             "roller-ecu-SN-EJAHGJQ-open-1.0.21-20260905T121300Z")
OPEN_PREFLIGHT = m.PROJECT / ("artifacts/device-backups/SN-EJAHGJQ/"
                              "stlink-preflight-20260906")
EXPECTED_PROGRAMMER_SHA256 = "ccd4876a664edadd07a897b0e308fcdedb4fddaa93a293ca049bad21d19c13a9"
PHASES = (
    "transaction_prepared",
    "actual_target_reverified",
    "loader_unprotect_started", "loader_unprotected",
    "loader_write_started", "loader_readback_verified",
    "loader_protection_started", "loader_protection_verified",
    "release_closed_halt_started", "release_closed_halted",
    "provisioning_request_started", "product_state_provisioning",
    "preclose_da_discovery_started", "preclose_da_integrity_verified",
    "closed_request_started", "product_state_closed_verified",
)
OBKEYS = ("DA_Config.obk", "OEMiRoT_Config.obk", "OEMiRoT_Data.obk")
RECEIPTS = ("da_config", "oemirot_config", "oemirot_data")


def phase(root: Path, name: str):
    path = root / "phase.txt"
    text = m.regular_bytes(path).decode() if path.exists() else ""
    current = tuple(text.splitlines())
    m.require(text.endswith("\n") if text else True, "incomplete transaction phase record")
    m.require(current == PHASES[:len(current)] and len(current) < len(PHASES) and
              PHASES[len(current)] == name, f"out-of-order phase: {name}")
    with path.open("a") as stream:
        stream.write(name + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    print(f"CLOSED PHASE: {name}", flush=True)


def phases(root: Path):
    text = m.regular_bytes(root / "phase.txt").decode()
    m.require(text.endswith("\n"), "incomplete transaction phase record")
    result = tuple(text.splitlines())
    m.require(result == PHASES[:len(result)], "unknown, skipped or duplicated phase")
    return result


def done(root: Path, name: str):
    return name in phases(root)


def json_read(path: Path):
    return json.loads(m.regular_bytes(path))


def require_programmer():
    m.require(m.CLI.is_file() and not m.CLI.is_symlink(), "unreviewed Programmer path")
    m.require(m.digest(m.regular_bytes(m.CLI)) == EXPECTED_PROGRAMMER_SHA256,
              "STM32CubeProgrammer binary changed")
    result = subprocess.run([str(m.CLI), "--version"], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=10)
    output = da.normalize_cli_output(result.stdout)
    m.require(result.returncode == 0 and
              re.search(r"STM32CubeProgrammer version:\s*2\.23\.0\b", output),
              "requires STM32CubeProgrammer exactly 2.23.0")


def verify_open_obkey_receipts():
    m.verify_staged(MANUFACTURING)
    backup = m.USER_BACKUP / MANUFACTURING.name
    m.verify_staged(backup)
    m.require(m.regular_bytes(MANUFACTURING / "manifest.json") ==
              m.regular_bytes(backup / "manifest.json"), "manufacturing copies differ")
    expected = "\n".join(m.OBK_PHASES) + "\n"
    for source in (MANUFACTURING, backup):
        m.require(m.regular_bytes(source / "obkeys-phase.txt").decode() == expected,
                  "OPEN OBKey phase journal incomplete")
        for stem, name in zip(RECEIPTS, OBKEYS):
            receipt = json_read(source / "obkeys" / f"{stem}-receipt.json")
            log = m.regular_bytes(source / "obkeys" / f"{stem}-sdp.log")
            data = m.regular_bytes(source / name)
            m.require(receipt == dict(result="PASS", returncode=0,
                      obk_sha256=m.digest(data), log_sha256=m.digest(log), target=m.TARGET),
                      f"invalid original OBKey receipt: {name}")
            m.require((len(data), m.digest(data)) == m.PINNED_OBKS[name] and
                      "OBKey Provisioned successfully" in log.decode(),
                      f"no authoritative OPEN provisioning evidence: {name}")


def require_safe(state):
    open_preflight.require_safe(state)
    m.require(state["device_serial"] == m.SERIAL and state["ecu_ip"] == m.TARGET["ecu_ip"],
              "wrong live device mapping")


def immutable_files(root: Path):
    return {str(path.relative_to(root / "immutable")): path for path in
            (root / "immutable").rglob("*") if path.is_file()}


def write_manifest(root: Path):
    files = immutable_files(root)
    manifest = dict(
        schema=SCHEMA, transaction_uuid=str(uuid.uuid4()),
        created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        target=m.TARGET, release=dict(version="1.0.22", counter=22, accepted_sequence=22,
                                      closed_loader_size=m.PINNED["ReleaseClosed-ECU_OEMiROT.bin"][0],
                                      closed_loader_sha256=m.PINNED["ReleaseClosed-ECU_OEMiROT.bin"][1]),
        tool=dict(realpath=str(m.CLI.resolve()), sha256=EXPECTED_PROGRAMMER_SHA256,
                  version="2.23.0"),
        policy=dict(existing_open_obkeys_only=True, obkey_resubmission=False,
                    da_permission="0x00004040", locked_forbidden=True,
                    full_regression_separate_transaction=True),
        files={name: dict(size=path.stat().st_size, sha256=m.digest(m.regular_bytes(path)))
               for name, path in sorted(files.items())})
    m.write_json(root / "manifest.json", manifest)


def verify_transaction(root: Path):
    m.require(root.is_dir() and not root.is_symlink() and
              not any(path.is_symlink() for path in root.rglob("*")),
              "transaction/symlink input rejected")
    manifest = json_read(root / "manifest.json")
    m.require(set(manifest) == {"schema", "transaction_uuid", "created_utc", "target",
              "release", "tool", "policy", "files"} and manifest["schema"] == SCHEMA and
              manifest["target"] == m.TARGET and manifest["release"] == dict(
                  version="1.0.22", counter=22, accepted_sequence=22,
                  closed_loader_size=52292,
                  closed_loader_sha256=m.PINNED["ReleaseClosed-ECU_OEMiROT.bin"][1]) and
              manifest["tool"] == dict(realpath=str(m.CLI.resolve()),
                  sha256=EXPECTED_PROGRAMMER_SHA256, version="2.23.0") and
              manifest["policy"] == dict(existing_open_obkeys_only=True,
                  obkey_resubmission=False, da_permission="0x00004040",
                  locked_forbidden=True, full_regression_separate_transaction=True),
              "transaction identity/release/tool/policy mismatch")
    uuid.UUID(manifest["transaction_uuid"], version=4)
    actual = immutable_files(root)
    m.require(set(manifest["files"]) == set(actual), "immutable file set changed")
    for name, path in actual.items():
        data = m.regular_bytes(path)
        m.require(manifest["files"][name] == dict(size=len(data), sha256=m.digest(data)),
                  f"immutable transaction input changed: {name}")
    inputs = root / "immutable/recovery-inputs"
    recovery.verify(inputs)
    flash = m.regular_bytes(root / "immutable/capture/full-flash-before.bin")
    recovery.require_captured_flash(flash)
    m.require(flash == m.regular_bytes(inputs / "source-open-flash.bin") and
              m.regular_bytes(root / "immutable/capture/persistent-before.bin") ==
              flash[0xE0000:0x100000], "captured persistent/Flash baseline mismatch")
    m.require_rss_options(m.regular_bytes(root / "immutable/capture/options-and-uid.log").decode())
    for name in ("runtime-before.json", "runtime-after-readback.json"):
        require_safe(json_read(root / "immutable" / name))
    proof = json_read(root / "immutable/open-obkey-proof.json")
    m.require(proof == dict(result="PASS", target=m.TARGET,
              manufacturing_manifest_sha256=m.digest(m.regular_bytes(MANUFACTURING / "manifest.json")),
              obkeys={name: m.PINNED_OBKS[name][1] for name in OBKEYS}),
              "OPEN OBKey proof mismatch")
    phases(root)
    return manifest


def prepare(root: Path):
    m.require(not root.exists() and root.name.startswith("roller-ecu-SN-EJAHGJQ-closed-"),
              "use a new explicit SN-EJAHGJQ CLOSED transaction")
    require_programmer()
    m.verify_pki_backup()
    recovery.verify(RECOVERY)
    recovery.verify(m.USER_BACKUP / RECOVERY.name)
    open_preflight.verify(OPEN_PREFLIGHT, RECOVERY)
    verify_open_obkey_receipts()
    state = m.capture(m.SERIAL)
    require_safe(state)
    root.mkdir(parents=True, mode=0o700)
    immutable = root / "immutable"
    immutable.mkdir(mode=0o700)
    shutil.copytree(RECOVERY, immutable / "recovery-inputs")
    m.write_json(immutable / "runtime-before.json", state)
    capture = immutable / "capture"
    capture.mkdir(mode=0o700)
    options = m.programmer(capture, "options-and-uid", ["-ob", "displ", "-r32",
                            "0x08FFF800", "12"])
    m.require_rss_options(options)
    m.programmer(capture, "full-flash-read", ["-u", "0x08000000", "0x200000",
                                               str(capture / "full-flash-before.bin")])
    m.programmer(capture, "persistent-read", ["-u", "0x080E0000", "0x20000",
                                               str(capture / "persistent-before.bin")])
    try:
        flash = m.regular_bytes(capture / "full-flash-before.bin")
        recovery.require_captured_flash(flash)
        m.require(flash == m.regular_bytes(RECOVERY / "source-open-flash.bin") and
                  m.regular_bytes(capture / "persistent-before.bin") == flash[0xE0000:0x100000],
                  "fresh board data differs from reviewed recovery baseline")
    finally:
        m.programmer(capture, "return-to-application", ["-rst"])
    time.sleep(3)
    after = m.capture(m.SERIAL)
    require_safe(after)
    m.write_json(immutable / "runtime-after-readback.json", after)
    m.write_json(immutable / "open-obkey-proof.json", dict(
        result="PASS", target=m.TARGET,
        manufacturing_manifest_sha256=m.digest(m.regular_bytes(MANUFACTURING / "manifest.json")),
        obkeys={name: m.PINNED_OBKS[name][1] for name in OBKEYS}))
    write_manifest(root)
    m.write_new(root / "phase.txt", b"transaction_prepared\n")
    verify_transaction(root)
    backup = m.USER_BACKUP / root.name
    m.require(not backup.exists(), "independent transaction backup already exists")
    shutil.copytree(root, backup)
    m.require(verify_transaction(backup) == verify_transaction(root), "transaction backup differs")
    os.sync()
    print(f"Prepared and independently backed up: {root}\nBackup: {backup}", flush=True)


def option_value(output, field):
    found = re.findall(rf"^\s*{field}\s*:\s*0x([0-9a-fA-F]+)\b", output, re.M)
    m.require(len(found) == 1, f"missing/duplicate option {field}")
    return int(found[0], 16)


def require_protections(output, product):
    # In the dedicated PROVISIONING RSS view the secure boot lock/address are
    # deliberately masked as zero. This is an architectural state view, not a
    # loss of the previously verified physical option bytes. WRP/HDP, TZEN,
    # BOOT_UBE, watchdog policy and data-area settings remain authoritative.
    secure_boot_lock = 0 if product == 0x17 else 0xB4
    secure_boot_address = 0 if product == 0x17 else 0xC0000
    expected = dict(PRODUCT_STATE=product, TZEN=0xB4, BOOT_UBE=0xB4,
                    IWDG_SW=1, SWAP_BANK=0, SECBOOT_LOCK=secure_boot_lock,
                    SECBOOTADD=secure_boot_address, SECWM1_STRT=0, SECWM1_END=0x7F,
                    SECWM2_STRT=1, SECWM2_END=0, WRPSGn1=0xFFFFFFF0,
                    WRPSGn2=0xFFFFFFFF, HDP1_STRT=0, HDP1_END=0x17,
                    HDP2_STRT=1, HDP2_END=0, EDATA1_EN=0, EDATA2_EN=0,
                    LOCKBL=0)
    for field, value in expected.items():
        m.require(option_value(output, field) == value, f"unsafe {field}")
    m.require(re.search(r"Device ID\s*:\s*0x484\b", output) and
              re.search(r"Revision ID\s*:\s*Rev X\b", output) and
              re.search(r"SFSP Version\s*:\s*v2\.5\.0\b", output), "wrong H563 ROM target")


def require_unprotected(output):
    require_protections(output, 0xED) if False else None
    for field, value in dict(PRODUCT_STATE=0xED, TZEN=0xB4, BOOT_UBE=0xB4,
                             IWDG_SW=1, SECBOOT_LOCK=0xB4, SECBOOTADD=0xC0000,
                             WRPSGn1=0xFFFFFFFF, WRPSGn2=0xFFFFFFFF,
                             HDP1_STRT=1, HDP1_END=0, HDP2_STRT=1,
                             HDP2_END=0).items():
        m.require(option_value(output, field) == value, f"unexpected unprotected {field}")


def common_discovery(output, lifecycle):
    values = dict(target_ID="0x484", SDA_version="2.4.0", Vendor_ID="STMicroelectronics",
                  PSA_lifecycle=lifecycle, cryptosystems="Ecdsa-P256 SHA256",
                  ST_provisioning_integrity_status="0xEAEAEAEA",
                  ST_provisioning_integrity_status_message="VALID")
    labels = {"target_ID": "target ID", "SDA_version": "SDA version", "Vendor_ID": "Vendor ID",
              "PSA_lifecycle": "PSA lifecycle", "cryptosystems": "cryptosystems",
              "ST_provisioning_integrity_status": "ST provisioning integrity status",
              "ST_provisioning_integrity_status_message": "ST provisioning integrity status message"}
    for key, expected in values.items():
        found = re.findall(rf"^\s*discovery:\s*{re.escape(labels[key])}\.*\s*:\s*(\S.*?\S|\S)\s*$",
                           da.normalize_cli_output(output), re.M)
        m.require(found == [expected], f"DA discovery {labels[key]} is not {expected}")


def raw(root: Path, name: str, command, *, accept_disconnect=False):
    intent = root / f"{name}.intent.json"
    log = root / f"{name}.log"
    receipt = root / f"{name}.receipt.json"
    m.require(not any(path.exists() for path in (intent, log, receipt)),
              f"operation {name} already attempted; inspect hardware before resume")
    m.write_json(intent, dict(command=command, accept_disconnect=accept_disconnect))
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=45)
        output = da.normalize_cli_output(result.stdout or "")
        m.write_new(log, output.encode())
        m.write_json(receipt, dict(returncode=result.returncode,
                     log_sha256=m.digest(output.encode())))
    except subprocess.TimeoutExpired as error:
        partial = error.stdout or b""
        if isinstance(partial, bytes):
            partial = partial.decode(errors="replace")
        m.write_new(log, da.normalize_cli_output(partial).encode())
        raise RuntimeError(f"ambiguous timeout in {name}; never blindly replay") from error
    if not accept_disconnect:
        m.require(result.returncode == 0 and not re.search(r"\b(error|failed|failure)\b", output, re.I),
                  f"Programmer operation failed: {name}")
    return output


def complete_provisioning_to_closed(root: Path):
    current = phases(root)
    m.require(current and current[-1] in ("provisioning_request_started",
              "product_state_provisioning"),
              "PROVISIONING continuation requires the exact write-ahead phase")
    if not done(root, "product_state_provisioning"):
        provisioning = m.programmer(root, "23-resume-provisioning-options",
                                    ["-ob", "displ"], reset=False)
        require_protections(provisioning, 0x17)
        phase(root, "product_state_provisioning")

    phase(root, "preclose_da_discovery_started")
    connect_da = [str(m.CLI), "-c", "port=SWD", f"sn={m.PROBE}", "speed=fast"]
    discovered = raw(root, "24-provisioning-da-discovery", connect_da + ["debugauth=2"])
    common_discovery(discovered, "ST_LIFECYCLE_PROVISIONING")
    raw(root, "25-post-discovery-nrst", [str(m.CLI), "-c", "port=SWD",
        f"sn={m.PROBE}", "mode=HWRSTPULSE"], accept_disconnect=True)
    provisioned = m.programmer(root, "26-provisioning-recheck", ["-ob", "displ"], reset=False)
    require_protections(provisioned, 0x17)
    phase(root, "preclose_da_integrity_verified")

    phase(root, "closed_request_started")
    connect_hotplug = [str(m.CLI), "-c", "port=SWD", f"sn={m.PROBE}", "ap=1", "mode=Hotplug"]
    raw(root, "27-request-closed", connect_hotplug + ["-ob", "PRODUCT_STATE=0x72"],
        accept_disconnect=True)
    closed = raw(root, "28-closed-da-discovery", connect_da + ["debugauth=2"])
    common_discovery(closed, "ST_LIFECYCLE_CLOSED")
    phase(root, "product_state_closed_verified")
    actual = m.regular_bytes(root / "closed-loader-full-flash.bin")
    m.write_json(root / "closed-handoff.json", dict(result="PASS", target=m.TARGET,
        product_state="0x72 CLOSED", provisioning_integrity="0xEAEAEAEA VALID",
        da_permission="0x00004040", locked=False, flash_sha256=m.digest(actual),
        cold_power_cycle_required=True, hardware_da_open_not_yet_tested=True,
        hardware_full_regression_not_yet_tested=True))
    os.sync()
    print("CLOSED verified by strict ROM DA discovery. Cold power cycle is required before service acceptance.")


def finalize(root: Path):
    manifest = verify_transaction(root)
    backup = m.USER_BACKUP / root.name
    m.require(verify_transaction(backup) == manifest and phases(root) == (PHASES[0],),
              "requires pristine prepared transaction and backup")
    require_programmer()
    m.verify_pki_backup()
    verify_open_obkey_receipts()
    state = m.capture(m.SERIAL)
    require_safe(state)
    m.write_json(root / "runtime-final-gate.json", state)
    options = m.programmer(root, "01-final-options-uid", ["-ob", "displ", "-r32",
                           "0x08FFF800", "12"])
    m.require_rss_options(options)
    m.programmer(root, "02-final-full-flash", ["-u", "0x08000000", "0x200000",
                                                  str(root / "final-full-flash.bin")])
    try:
        flash = m.regular_bytes(root / "final-full-flash.bin")
        recovery.require_captured_flash(flash)
        m.require(flash == m.regular_bytes(root / "immutable/capture/full-flash-before.bin"),
                  "live target changed after immutable transaction backup")
    finally:
        m.programmer(root, "03-final-return", ["-rst"])
    time.sleep(3)
    require_safe(m.capture(m.SERIAL))
    phase(root, "actual_target_reverified")

    phase(root, "loader_unprotect_started")
    m.programmer(root, "10-loader-unprotect", ["-ob", "WRPSGn1=0xFFFFFFFF",
                 "WRPSGn2=0xFFFFFFFF", "HDP1_STRT=0x1", "HDP1_END=0x0",
                 "HDP2_STRT=0x1", "HDP2_END=0x0"])
    unprotected = m.programmer(root, "11-unprotected-options", ["-ob", "displ",
                               "-r32", "0x08FFF800", "12"])
    m.require_programmer_uid(unprotected)
    require_unprotected(unprotected)
    phase(root, "loader_unprotected")

    closed_loader = root / "immutable/recovery-inputs/ReleaseClosed-ECU_OEMiROT.bin"
    phase(root, "loader_write_started")
    output = m.programmer(root, "12-write-closed-loader", ["-d", str(closed_loader),
                           "0x0C000000", "-v"])
    m.require("Download verified successfully" in output, "closed loader not verified")
    m.programmer(root, "13-read-closed-loader", ["-u", "0x0C000000", "0xCC44",
                 str(root / "ReleaseClosed-readback.bin")])
    m.require(m.regular_bytes(root / "ReleaseClosed-readback.bin") == m.regular_bytes(closed_loader),
              "closed loader readback mismatch")
    phase(root, "loader_readback_verified")

    phase(root, "loader_protection_started")
    m.programmer(root, "14-loader-protect", ["-ob", "WRPSGn1=0xFFFFFFF0",
                 "WRPSGn2=0xFFFFFFFF", "HDP1_STRT=0x0", "HDP1_END=0x17",
                 "HDP2_STRT=0x1", "HDP2_END=0x0"])
    protected = m.programmer(root, "15-protected-options", ["-ob", "displ",
                             "-r32", "0x08FFF800", "12"])
    m.require_programmer_uid(protected)
    require_protections(protected, 0xED)
    m.programmer(root, "16-closed-loader-full-flash", ["-u", "0x08000000", "0x200000",
                 str(root / "closed-loader-full-flash.bin")])
    actual = m.regular_bytes(root / "closed-loader-full-flash.bin")
    expected = bytearray(flash)
    expected[:len(m.regular_bytes(closed_loader))] = m.regular_bytes(closed_loader)
    m.require(actual == bytes(expected), "unexpected Flash change while installing closed loader")
    recovery.images.audit_flash(actual, root / "immutable/recovery-inputs/1.0.22",
                                root / "immutable/recovery-inputs", "installed", "ReleaseClosed")
    phase(root, "loader_protection_verified")

    phase(root, "release_closed_halt_started")
    m.programmer(root, "20-halt-release-closed", ["-halt", "-score"])
    phase(root, "release_closed_halted")
    phase(root, "provisioning_request_started")
    connect_hotplug = [str(m.CLI), "-c", "port=SWD", f"sn={m.PROBE}", "ap=1", "mode=Hotplug"]
    raw(root, "21-request-provisioning", connect_hotplug + ["-ob", "PRODUCT_STATE=0x17"])
    raw(root, "22-provisioning-hard-reset", connect_hotplug + ["-hardRst"])
    provisioning = m.programmer(root, "23-provisioning-options", ["-ob", "displ"], reset=False)
    require_protections(provisioning, 0x17)
    phase(root, "product_state_provisioning")
    complete_provisioning_to_closed(root)


def resume_provisioning(root: Path):
    manifest = verify_transaction(root)
    backup = m.USER_BACKUP / root.name
    m.require(verify_transaction(backup) == manifest,
              "independent prepared transaction backup differs")
    m.require(phases(root) == PHASES[:PHASES.index("provisioning_request_started") + 1],
              "resume is allowed only at the observed PROVISIONING write-ahead phase")
    require_programmer()
    m.verify_pki_backup()
    verify_open_obkey_receipts()
    complete_provisioning_to_closed(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "verify", "finalize", "resume-provisioning"))
    parser.add_argument("--transaction", type=Path, required=True)
    parser.add_argument("--accept-irreversible-closed", action="store_true")
    args = parser.parse_args()
    os.umask(0o077)
    root = args.transaction.absolute()
    with Path("/tmp/roller-ecu-closed-transition.lock").open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.action == "prepare":
            prepare(root)
        elif args.action == "verify":
            print(json.dumps(verify_transaction(root), indent=2))
        elif args.action == "finalize":
            m.require(args.accept_irreversible_closed,
                      "finalize requires --accept-irreversible-closed")
            finalize(root)
        else:
            m.require(args.accept_irreversible_closed,
                      "resume-provisioning requires --accept-irreversible-closed")
            resume_provisioning(root)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f"SN-EJAHGJQ CLOSED transaction stopped: {error}", flush=True)
        raise SystemExit(1)
