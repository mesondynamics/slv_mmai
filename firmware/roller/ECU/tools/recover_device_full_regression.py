#!/usr/bin/env python3
"""Transactional Full Regression recovery for the one reviewed SN-EJAHGJQ ECU.

This is deliberately not a generic programmer.  It is pinned to the MCU UID,
ATECC identity, ST-Link serial, CubeProgrammer binary, signed 1.0.22 images and
the board's own pairing store.  Each mutating phase has a durable write-ahead
journal and is refused on replay.  LOCKED and Partial Regression are absent.

The destructive regression/rebuild, physical JP1 OBKey phase, return to CLOSED,
DA service acceptance and final cold-start acceptance are separate actions.
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
import sys
import time
import uuid

import accept_device_closed_da as da_accept
import ecu_debug_auth as da
import finalize_device_closed as closed
import manufacture_open_device as m
import prepare_device_recovery as recovery


SCHEMA = "roller-ecu-SN-EJAHGJQ-full-regression-recovery-v1"
SOURCE_CLOSED = m.PROJECT / ("artifacts/device-backups/SN-EJAHGJQ/"
                             "roller-ecu-SN-EJAHGJQ-closed-20260906T064833Z")
SOURCE_ACCEPTANCE = m.PROJECT / ("artifacts/device-backups/SN-EJAHGJQ/"
                                 "closed-cold-and-da-20260906T071359Z")
INPUTS = m.PROJECT / ("artifacts/device-backups/SN-EJAHGJQ/"
                      "roller-ecu-SN-EJAHGJQ-recovery-inputs-1.0.22-20260906")
EXPECTED_CLOSED_UUID = "8c2bb8c3-492f-4469-bff2-8bf000339809"
EXPECTED_OPEN_FLASH = "06d4150b04668cf056533b35ef5e968fb46e09961ac6fc802091537265a0c677"
EXPECTED_CLOSED_FLASH = "6123f8dc64eef3ceb1af0a8a088e4e3aa7901b9a7f1723f2d2f17aae73752d99"
PHASES = (
    "transaction_prepared",
    "closed_target_reverified",
    "full_regression_started",
    "full_regression_open_verified",
    "open_rebuild_started",
    "open_rebuild_verified",
    "awaiting_jp1_high",
    "rss_open_target_verified",
    "da_config_submission_started", "da_config_provisioned",
    "oemirot_config_submission_started", "oemirot_config_provisioned",
    "oemirot_data_submission_started", "oemirot_data_provisioned",
    "awaiting_jp1_open",
    "open_target_verified",
    "open_runtime_verified",
    "closed_loader_write_started",
    "closed_loader_verified",
    "provisioning_request_started",
    "product_state_provisioning",
    "preclose_da_integrity_verified",
    "closed_request_started",
    "product_state_closed_verified",
    "closed_da_accepted",
    "final_cold_runtime_accepted",
)
OBKS = (("da_config", "DA_Config.obk"),
        ("oemirot_config", "OEMiRoT_Config.obk"),
        ("oemirot_data", "OEMiRoT_Data.obk"))


def read_json(path: Path):
    return json.loads(m.regular_bytes(path))


def phase(root: Path, name: str):
    path = root / "phase.txt"
    text = m.regular_bytes(path).decode() if path.exists() else ""
    current = tuple(text.splitlines())
    m.require((not text or text.endswith("\n")) and current == PHASES[:len(current)] and
              len(current) < len(PHASES) and PHASES[len(current)] == name,
              f"invalid or replayed recovery phase: {name}")
    with path.open("a") as stream:
        stream.write(name + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    print(f"FULL REGRESSION RECOVERY: {name}", flush=True)


def phases(root: Path):
    text = m.regular_bytes(root / "phase.txt").decode()
    result = tuple(text.splitlines())
    m.require(text.endswith("\n") and result == PHASES[:len(result)],
              "torn, skipped or unknown recovery phase journal")
    return result


def require_phase(root: Path, name: str):
    m.require(phases(root) == PHASES[:PHASES.index(name) + 1],
              f"action requires exact phase {name}")


def tree_files(root: Path):
    return {str(path.relative_to(root)): path for path in root.rglob("*") if path.is_file()}


def require_tree_equal(left: Path, right: Path):
    a, b = tree_files(left), tree_files(right)
    m.require(set(a) == set(b), "independent backup file set differs")
    for name in a:
        m.require(m.regular_bytes(a[name]) == m.regular_bytes(b[name]),
                  f"independent backup differs: {name}")


def expected_closed_flash(inputs: Path):
    image = bytearray(m.regular_bytes(inputs / "recovery-open-flash.bin"))
    loader = m.regular_bytes(inputs / "ReleaseClosed-ECU_OEMiROT.bin")
    image[:len(loader)] = loader
    return bytes(image)


def verify_source_provenance():
    closed_manifest = closed.verify_transaction(SOURCE_CLOSED)
    m.require(closed.phases(SOURCE_CLOSED) == closed.PHASES and
              closed_manifest["transaction_uuid"] == EXPECTED_CLOSED_UUID,
              "source CLOSED transaction is not exact and complete")
    source_backup = m.USER_BACKUP / SOURCE_CLOSED.name
    m.require(closed.verify_transaction(source_backup) == closed_manifest,
              "source CLOSED transaction backup differs")
    report = recovery.verify(INPUTS)
    m.require(recovery.verify(m.USER_BACKUP / INPUTS.name) == report and
              report["recovery_flash_sha256"] == EXPECTED_OPEN_FLASH,
              "dual recovery input copies are not exact")
    baseline = m.regular_bytes(SOURCE_CLOSED / "closed-loader-full-flash.bin")
    # The original close started from the reviewed source OPEN image rather
    # than the canonical blank-secondary recovery image.  Both contain the
    # same signed primaries/persistent identity; keep this distinction explicit.
    source_closed = bytearray(m.regular_bytes(INPUTS / "source-open-flash.bin"))
    loader = m.regular_bytes(INPUTS / "ReleaseClosed-ECU_OEMiROT.bin")
    source_closed[:len(loader)] = loader
    m.require(baseline == bytes(source_closed), "source CLOSED Flash provenance mismatch")
    acceptance = read_json(SOURCE_ACCEPTANCE / "acceptance-final.json")
    m.require(acceptance["result"] == "PASS" and
              acceptance["device"] == dict(
                  serial=m.SERIAL, mcu_uid=m.TARGET["mcu_uid"],
                  atecc_serial=m.TARGET["atecc_serial"], product_state="0x72 CLOSED",
                  permission_mask="0x00004040",
                  permission_exercised="c / bit6 / Level 3 Intrusive Debug") and
              acceptance["debug_authentication"]["authentication"] == "PASS" and
              acceptance["debug_authentication"]["unauthenticated_read_before"] == "DENIED" and
              acceptance["debug_authentication"]["unauthenticated_read_after"] == "DENIED" and
              acceptance["debug_authentication"]["close_debug"] == "PASS" and
              acceptance["runtime_after_close"]["atecc_authentication"] == "PASS" and
              acceptance["runtime_after_close"]["ota_accepted_sequence"] == 22,
              "source CLOSED DA acceptance is incomplete")
    ns = m.regular_bytes(SOURCE_ACCEPTANCE / "da-service-final/nonsecure-primary.bin")
    secure = m.regular_bytes(SOURCE_ACCEPTANCE / "da-service-final/secure-primary.bin")
    m.require(ns == baseline[0x100000:0x150000] and secure == baseline[0x30000:0x60000],
              "authenticated CLOSED reads do not match source Flash")
    return closed_manifest, acceptance, report


def immutable_files(root: Path):
    return {str(path.relative_to(root / "immutable")): path for path in
            (root / "immutable").rglob("*") if path.is_file()}


def write_manifest(root: Path):
    files = immutable_files(root)
    manifest = dict(
        schema=SCHEMA, transaction_uuid=str(uuid.uuid4()),
        created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        target=dict(m.TARGET, source_product_state="0x72 CLOSED",
                    final_product_state="0x72 CLOSED"),
        release=dict(version="1.0.22", counter=22, accepted_sequence=22,
                     open_flash_sha256=EXPECTED_OPEN_FLASH,
                     closed_flash_sha256=EXPECTED_CLOSED_FLASH),
        tool=dict(realpath=str(m.CLI.resolve()), sha256=closed.EXPECTED_PROGRAMMER_SHA256,
                  version="2.23.0"),
        policy=dict(permission="a / bit14 / Full Regression",
                    explicit_full_device_erase_required=True, partial_regression=False,
                    locked_forbidden=True, fresh_obkey_receipts_required=True,
                    physical_jp1_required=True, final_da_service_acceptance_required=True),
        files={name: dict(size=path.stat().st_size, sha256=m.digest(m.regular_bytes(path)))
               for name, path in sorted(files.items())})
    m.write_json(root / "manifest.json", manifest)


def verify_transaction(root: Path):
    m.require(root.is_dir() and not root.is_symlink() and
              not any(path.is_symlink() for path in root.rglob("*")),
              "recovery transaction/symlink rejected")
    manifest = read_json(root / "manifest.json")
    m.require(set(manifest) == {"schema", "transaction_uuid", "created_utc", "target",
              "release", "tool", "policy", "files"} and manifest["schema"] == SCHEMA and
              manifest["target"] == dict(m.TARGET, source_product_state="0x72 CLOSED",
                                         final_product_state="0x72 CLOSED") and
              manifest["release"] == dict(version="1.0.22", counter=22,
                  accepted_sequence=22, open_flash_sha256=EXPECTED_OPEN_FLASH,
                  closed_flash_sha256=EXPECTED_CLOSED_FLASH) and
              manifest["tool"] == dict(realpath=str(m.CLI.resolve()),
                  sha256=closed.EXPECTED_PROGRAMMER_SHA256, version="2.23.0") and
              manifest["policy"] == dict(permission="a / bit14 / Full Regression",
                  explicit_full_device_erase_required=True, partial_regression=False,
                  locked_forbidden=True, fresh_obkey_receipts_required=True,
                  physical_jp1_required=True, final_da_service_acceptance_required=True),
              "recovery transaction identity/release/tool/policy mismatch")
    uuid.UUID(manifest["transaction_uuid"], version=4)
    files = immutable_files(root)
    m.require(set(files) == set(manifest["files"]), "immutable recovery file set changed")
    for name, path in files.items():
        data = m.regular_bytes(path)
        m.require(manifest["files"][name] == dict(size=len(data), sha256=m.digest(data)),
                  f"immutable recovery input changed: {name}")
    inputs = root / "immutable/recovery-inputs"
    report = recovery.verify(inputs)
    m.require(report["recovery_flash_sha256"] == EXPECTED_OPEN_FLASH,
              "wrong canonical OPEN recovery image")
    m.require(m.digest(expected_closed_flash(inputs)) == EXPECTED_CLOSED_FLASH,
              "wrong canonical CLOSED recovery image")
    source = root / "immutable/source-closed"
    source_manifest = read_json(source / "manifest.json")
    m.require(source_manifest["transaction_uuid"] == EXPECTED_CLOSED_UUID and
              m.regular_bytes(source / "phase.txt") == ("\n".join(closed.PHASES) + "\n").encode(),
              "copied CLOSED provenance is not complete")
    acceptance = read_json(root / "immutable/source-da/acceptance-final.json")
    m.require(acceptance["result"] == "PASS" and
              acceptance["device"]["mcu_uid"] == m.TARGET["mcu_uid"] and
              acceptance["device"]["product_state"] == "0x72 CLOSED" and
              acceptance["debug_authentication"]["close_debug"] == "PASS",
              "copied DA acceptance is invalid")
    phases(root)
    return manifest


def verify_prepared_backup(root: Path, manifest):
    transaction_name = root.name.removesuffix("-completed")
    backup = m.USER_BACKUP / transaction_name
    m.require(verify_transaction(backup) == manifest and
              phases(backup) == PHASES[:1] and
              m.regular_bytes(root / "manifest.json") == m.regular_bytes(
                  backup / "manifest.json"),
              "independent pre-regression transaction backup is not pristine")
    require_tree_equal(root / "immutable", backup / "immutable")


def prepare(root: Path):
    m.require(not root.exists() and root.name.startswith("roller-ecu-SN-EJAHGJQ-full-regression-"),
              "use a fresh explicit SN-EJAHGJQ Full Regression transaction path")
    closed.require_programmer()
    m.verify_pki_backup()
    closed_manifest, acceptance, inputs_report = verify_source_provenance()
    root.mkdir(parents=True, mode=0o700)
    immutable = root / "immutable"
    immutable.mkdir(mode=0o700)
    shutil.copytree(INPUTS, immutable / "recovery-inputs")
    source_closed = immutable / "source-closed"
    source_closed.mkdir(mode=0o700)
    for name in ("manifest.json", "phase.txt", "closed-loader-full-flash.bin",
                 "ReleaseClosed-readback.bin", "28-closed-da-discovery.log",
                 "debug-policy-correction.json"):
        m.write_new(source_closed / name, m.regular_bytes(SOURCE_CLOSED / name))
    source_da = immutable / "source-da"
    source_da.mkdir(mode=0o700)
    for name in ("acceptance-final.json", "runtime-final-da-cold.json",
                 "runtime-after-final-da-close.json"):
        m.write_new(source_da / name, m.regular_bytes(SOURCE_ACCEPTANCE / name))
    for name in ("evidence.json", "nonsecure-primary.bin", "secure-primary.bin",
                 "07-close-debug.log", "08-ordinary-read-denied-after.log"):
        m.write_new(source_da / name,
                    m.regular_bytes(SOURCE_ACCEPTANCE / "da-service-final" / name))
    m.write_json(immutable / "provenance-audit.json", dict(
        result="PASS", closed_transaction_uuid=closed_manifest["transaction_uuid"],
        closed_manifest_sha256=m.digest(m.regular_bytes(SOURCE_CLOSED / "manifest.json")),
        da_acceptance_sha256=m.digest(m.regular_bytes(SOURCE_ACCEPTANCE / "acceptance-final.json")),
        source_closed_flash_sha256=m.digest(m.regular_bytes(
            SOURCE_CLOSED / "closed-loader-full-flash.bin")),
        recovery_open_flash_sha256=inputs_report["recovery_flash_sha256"],
        canonical_recovery_closed_flash_sha256=EXPECTED_CLOSED_FLASH,
        da_permission_exercised=acceptance["device"]["permission_exercised"]))
    write_manifest(root)
    m.write_new(root / "phase.txt", b"transaction_prepared\n")
    manifest = verify_transaction(root)
    backup = m.USER_BACKUP / root.name
    m.require(not backup.exists(), "prepared recovery backup already exists")
    shutil.copytree(root, backup)
    m.require(verify_transaction(backup) == manifest, "prepared recovery backup invalid")
    require_tree_equal(root, backup)
    os.sync()
    print(f"Prepared non-destructive recovery transaction:\n  {root}\n"
          f"Independent backup:\n  {backup}\nNo hardware lifecycle command was sent.")


def require_regressed_open(output: str):
    m.require("STM32CubeProgrammer v2.23.0" in output and
              re.search(r"Device ID\s*:\s*0x484\b", output) and
              re.search(r"Revision ID\s*:\s*Rev X\b", output),
              "post-regression target/silicon/programmer mismatch")
    values = re.findall(r"^\s*PRODUCT_STATE\s*:\s*0x([0-9A-Fa-f]+)\b", output, re.M)
    m.require(values == ["ED"], "Full Regression did not produce exactly OPEN")
    m.require_programmer_uid(output)


def regression_command():
    return [sys.executable, str(m.PROJECT / "tools/ecu_debug_auth.py"),
            "full-regression-to-open", "--accept-closed-target",
            "--accept-full-device-erase", "--programmer", str(m.CLI),
            "--probe", m.PROBE, "--pki-dir", str(m.PKI),
            "--assets-dir", str(m.PROJECT / "artifacts/security-provisioning")]


def regress_and_rebuild(root: Path):
    manifest = verify_transaction(root)
    verify_prepared_backup(root, manifest)
    require_phase(root, "transaction_prepared")
    closed.require_programmer()
    m.verify_pki_backup()
    state = m.capture(m.SERIAL)
    recovery.require_runtime_baseline(state)
    m.write_json(root / "runtime-before-regression.json", state)
    discovery = closed.raw(root, "01-closed-discovery", da_accept.service_command("discover"))
    closed.common_discovery(discovery, "ST_LIFECYCLE_CLOSED")
    phase(root, "closed_target_reverified")

    phase(root, "full_regression_started")
    output = closed.raw(root, "02-full-regression", regression_command())
    m.require("Full regression requested: device contents are erased and product state returns OPEN"
              in output, "Full Regression has no authoritative success result")
    time.sleep(1)
    options = m.programmer(root, "03-regressed-open-options-uid",
                           ["-ob", "displ", "-r32", "0x08FFF800", "12"])
    require_regressed_open(options)
    blank = root / "post-regression-blank-sample.bin"
    m.programmer(root, "04-post-regression-blank-sample",
                 ["-u", "0x08000000", "0x1000", str(blank)])
    m.require(m.regular_bytes(blank) == b"\xff" * 0x1000,
              "post-regression Flash is not blank; rebuild refused")
    phase(root, "full_regression_open_verified")

    inputs = root / "immutable/recovery-inputs"
    phase(root, "open_rebuild_started")
    m.programmer(root, "05-enable-trustzone", ["-ob", "TZEN=0xB4"], reset=False)
    m.programmer(root, "06-erase-and-normalize-open", [
        "-ob", "SECWM1_STRT=0x1", "SECWM1_END=0x0",
        "WRPSGn1=0xFFFFFFFF", "WRPSGn2=0xFFFFFFFF",
        "SECWM2_STRT=0x1", "SECWM2_END=0x0",
        "HDP1_STRT=0x1", "HDP1_END=0x0", "HDP2_STRT=0x1", "HDP2_END=0x0",
        "SECBOOT_LOCK=0xC3", "SECBOOTADD=0xC0000", "SWAP_BANK=0",
        "SRAM2_RST=0", "SRAM2_ECC=0", "SRAM3_ECC=1", "BOOT_UBE=0xB4",
        "-e", "all"])
    m.programmer(root, "07-secure-watermarks", ["-ob", "SECWM1_STRT=0x0",
                 "SECWM1_END=0x7F", "SECWM2_STRT=0x1", "SECWM2_END=0x0"])
    for index, name, address in (
            (8, "1.0.22/secure-initial.bin", "0x0C030000"),
            (9, "1.0.22/nonsecure-initial.bin", "0x08100000"),
            (10, "secure-persistent.bin", "0x0C0E0000"),
            (11, "ReleaseOpen-ECU_OEMiROT.bin", "0x0C000000")):
        result = m.programmer(root, f"{index:02d}-write-{Path(name).name}",
                              ["-d", str(inputs / name), address, "-v"])
        m.require("Download verified successfully" in result,
                  f"recovery write lacks verification: {name}")
    m.programmer(root, "12-restore-open-protections", ["-ob",
                 "WRPSGn1=0xFFFFFFF0", "WRPSGn2=0xFFFFFFFF",
                 "HDP1_STRT=0x0", "HDP1_END=0x17", "HDP2_STRT=0x1", "HDP2_END=0x0",
                 "SECBOOT_LOCK=0xB4"])
    rebuilt = root / "recovered-open-flash.bin"
    m.programmer(root, "13-recovered-open-full-read",
                 ["-u", "0x08000000", "0x200000", str(rebuilt)])
    expected = m.regular_bytes(inputs / "recovery-open-flash.bin")
    m.require(m.regular_bytes(rebuilt) == expected and m.digest(expected) == EXPECTED_OPEN_FLASH,
              "recovered OPEN Flash is not byte-exact")
    options = m.programmer(root, "14-recovered-open-options-uid",
                           ["-ob", "displ", "-r32", "0x08FFF800", "12"])
    closed.require_protections(options, 0xED)
    m.require_programmer_uid(options)
    phase(root, "open_rebuild_verified")
    phase(root, "awaiting_jp1_high")
    os.sync()
    print("Full Regression and byte-exact OPEN rebuild passed. Keep ESTOP_NC disconnected.\n"
          "Power off, short JP1, keep ST-Link/NRST connected, then power on.")


def provision_obkeys(root: Path):
    manifest = verify_transaction(root)
    verify_prepared_backup(root, manifest)
    require_phase(root, "awaiting_jp1_high")
    m.verify_pki_backup()
    inputs = root / "immutable/recovery-inputs"
    evidence = root / "fresh-obkeys"
    m.require(not evidence.exists(), "fresh OBKey evidence already exists; replay forbidden")
    evidence.mkdir(mode=0o700)
    m.programmer(evidence, "00-enter-rss-reset", ["-hardRst"], reset=False)
    m.wait_open_rss(evidence, "01-before-readback")
    for name, address, size, expected in (
            ("boot", "0x0C000000", 52276, inputs / "ReleaseOpen-ECU_OEMiROT.bin"),
            ("secure", "0x0C030000", 0x30000, inputs / "1.0.22/secure-initial.bin"),
            ("nonsecure", "0x08100000", 0x50000, inputs / "1.0.22/nonsecure-initial.bin"),
            ("persistent", "0x0C0E0000", 0x20000, inputs / "secure-persistent.bin")):
        path = evidence / f"{name}-before-obkeys.bin"
        m.programmer(evidence, f"02-readback-{name}",
                     ["-u", address, hex(size), str(path)], reset=False)
        m.require(m.regular_bytes(path) == m.regular_bytes(expected),
                  f"live recovered {name} differs before OBKeys")
    m.verify_dual_store(m.regular_bytes(evidence / "persistent-before-obkeys.bin")[:0x4000],
                        identity=m.IDENTITY)
    phase(root, "rss_open_target_verified")
    for stem, name in OBKS:
        m.programmer(evidence, f"{stem}-reset", ["-hardRst"], reset=False)
        m.wait_open_rss(evidence, stem)
        data = m.regular_bytes(inputs / name)
        m.require((len(data), m.digest(data)) == m.PINNED_OBKS[name],
                  f"unreviewed recovery OBKey: {name}")
        phase(root, f"{stem}_submission_started")
        output = m.programmer(evidence, f"{stem}-sdp", ["-sdp", str(inputs / name)],
                              reset=False)
        m.require_obkey_success(output)
        m.write_json(evidence / f"{stem}-receipt.json", dict(
            result="PASS", obk=name, obk_sha256=m.digest(data),
            log_sha256=m.digest(output.encode()), target=m.TARGET,
            transaction_uuid=read_json(root / "manifest.json")["transaction_uuid"]))
        phase(root, f"{stem}_provisioned")
    m.programmer(evidence, "final-rss-reset", ["-hardRst"], reset=False)
    m.wait_open_rss(evidence, "final-open")
    phase(root, "awaiting_jp1_open")
    os.sync()
    print("Fresh DA/OEMiROT OBKeys have three unambiguous receipts.\n"
          "Power off, open JP1, keep ESTOP_NC disconnected and ST-Link/NRST connected, power on.")


def verify_fresh_obkeys(root: Path):
    evidence = root / "fresh-obkeys"
    transaction_uuid = read_json(root / "manifest.json")["transaction_uuid"]
    for stem, name in OBKS:
        data = m.regular_bytes(root / "immutable/recovery-inputs" / name)
        log = m.regular_bytes(evidence / f"{stem}-sdp.log")
        receipt = read_json(evidence / f"{stem}-receipt.json")
        m.require(receipt == dict(result="PASS", obk=name, obk_sha256=m.digest(data),
                  log_sha256=m.digest(log), target=m.TARGET,
                  transaction_uuid=transaction_uuid) and
                  "OBKey Provisioned successfully" in log.decode() and
                  not re.search(r"\b(error|failed|failure)\b", log.decode(), re.I),
                  f"fresh OBKey receipt invalid: {name}")


def verify_open(root: Path):
    manifest = verify_transaction(root)
    verify_prepared_backup(root, manifest)
    require_phase(root, "awaiting_jp1_open")
    verify_fresh_obkeys(root)
    options = m.programmer(root, "20-normal-open-options-uid",
                           ["-ob", "displ", "-r32", "0x08FFF800", "12"])
    closed.require_protections(options, 0xED)
    m.require_programmer_uid(options)
    live = root / "normal-open-full-flash.bin"
    m.programmer(root, "21-normal-open-full-read",
                 ["-u", "0x08000000", "0x200000", str(live)])
    m.require(m.regular_bytes(live) == m.regular_bytes(
        root / "immutable/recovery-inputs/recovery-open-flash.bin"),
        "normal OPEN Flash differs after fresh OBKeys")
    phase(root, "open_target_verified")
    m.programmer(root, "22-reset-to-open-application", ["-rst"])
    time.sleep(3)
    state = m.capture(m.SERIAL)
    recovery.require_runtime_baseline(state)
    m.write_json(root / "runtime-after-open-recovery.json", state)
    phase(root, "open_runtime_verified")
    os.sync()
    print("Recovered OPEN application, ATECC pairing, accepted sequence 22 and zero outputs passed.\n"
          "The next action returns this same board to CLOSED; no OBKey is resubmitted.")


def close_recovered(root: Path):
    manifest = verify_transaction(root)
    verify_prepared_backup(root, manifest)
    require_phase(root, "open_runtime_verified")
    verify_fresh_obkeys(root)
    inputs = root / "immutable/recovery-inputs"
    state = m.capture(m.SERIAL)
    recovery.require_runtime_baseline(state)
    options = m.programmer(root, "30-preclose-options-uid",
                           ["-ob", "displ", "-r32", "0x08FFF800", "12"])
    closed.require_protections(options, 0xED)
    m.require_programmer_uid(options)
    current = root / "preclose-open-full-flash.bin"
    m.programmer(root, "31-preclose-open-full-read",
                 ["-u", "0x08000000", "0x200000", str(current)])
    m.require(m.regular_bytes(current) == m.regular_bytes(inputs / "recovery-open-flash.bin"),
              "live OPEN recovery image changed before close")

    phase(root, "closed_loader_write_started")
    m.programmer(root, "32-loader-unprotect", ["-ob", "WRPSGn1=0xFFFFFFFF",
                 "WRPSGn2=0xFFFFFFFF", "HDP1_STRT=0x1", "HDP1_END=0x0",
                 "HDP2_STRT=0x1", "HDP2_END=0x0"])
    result = m.programmer(root, "33-write-closed-loader",
                          ["-d", str(inputs / "ReleaseClosed-ECU_OEMiROT.bin"),
                           "0x0C000000", "-v"])
    m.require("Download verified successfully" in result, "CLOSED loader write not verified")
    m.programmer(root, "34-loader-protect", ["-ob", "WRPSGn1=0xFFFFFFF0",
                 "WRPSGn2=0xFFFFFFFF", "HDP1_STRT=0x0", "HDP1_END=0x17",
                 "HDP2_STRT=0x1", "HDP2_END=0x0"])
    options = m.programmer(root, "35-closed-loader-options-uid",
                           ["-ob", "displ", "-r32", "0x08FFF800", "12"])
    closed.require_protections(options, 0xED)
    m.require_programmer_uid(options)
    actual_path = root / "recovered-closed-full-flash.bin"
    m.programmer(root, "36-closed-loader-full-read",
                 ["-u", "0x08000000", "0x200000", str(actual_path)])
    actual = m.regular_bytes(actual_path)
    m.require(actual == expected_closed_flash(inputs) and
              m.digest(actual) == EXPECTED_CLOSED_FLASH,
              "canonical recovered CLOSED image is not byte-exact")
    phase(root, "closed_loader_verified")

    m.programmer(root, "37-halt-release-closed", ["-halt", "-score"])
    phase(root, "provisioning_request_started")
    hotplug = [str(m.CLI), "-c", "port=SWD", f"sn={m.PROBE}", "ap=1", "mode=Hotplug"]
    closed.raw(root, "38-request-provisioning", hotplug + ["-ob", "PRODUCT_STATE=0x17"])
    closed.raw(root, "39-provisioning-hard-reset", hotplug + ["-hardRst"])
    provisioning = m.programmer(root, "40-provisioning-options", ["-ob", "displ"], reset=False)
    closed.require_protections(provisioning, 0x17)
    phase(root, "product_state_provisioning")
    discovery = closed.raw(root, "41-provisioning-da-discovery",
                           [str(m.CLI), "-c", "port=SWD", f"sn={m.PROBE}",
                            "speed=fast", "debugauth=2"])
    closed.common_discovery(discovery, "ST_LIFECYCLE_PROVISIONING")
    # ES0565 / the reviewed H563 sequence: leave the discovery session with a
    # physical NRST pulse, then re-prove PROVISIONING protections before close.
    closed.raw(root, "41a-post-discovery-nrst", [str(m.CLI), "-c", "port=SWD",
               f"sn={m.PROBE}", "mode=HWRSTPULSE"], accept_disconnect=True)
    provisioning = m.programmer(root, "41b-provisioning-recheck", ["-ob", "displ"],
                                reset=False)
    closed.require_protections(provisioning, 0x17)
    phase(root, "preclose_da_integrity_verified")
    phase(root, "closed_request_started")
    closed.raw(root, "42-request-closed", hotplug + ["-ob", "PRODUCT_STATE=0x72"],
               accept_disconnect=True)
    discovered = closed.raw(root, "43-closed-da-discovery", da_accept.service_command("discover"))
    closed.common_discovery(discovered, "ST_LIFECYCLE_CLOSED")
    phase(root, "product_state_closed_verified")
    m.write_json(root / "closed-handoff.json", dict(
        result="PASS", serial=m.SERIAL, mcu_uid=m.TARGET["mcu_uid"],
        product_state="0x72 CLOSED", locked=False, da_permission="0x00004040",
        full_regression_permission_exercised="a / bit14",
        fresh_obkeys_verified=True, recovered_closed_flash_sha256=m.digest(actual),
        cold_power_cycle_before_da_acceptance_required=True))
    os.sync()
    print("Recovered device is strictly CLOSED, never LOCKED.\n"
          "One cold power cycle is now required before the final DA service acceptance.")


def accept_closed_da(root: Path):
    manifest = verify_transaction(root)
    verify_prepared_backup(root, manifest)
    require_phase(root, "product_state_closed_verified")
    verify_fresh_obkeys(root)
    baseline = m.regular_bytes(root / "recovered-closed-full-flash.bin")
    m.require(baseline == expected_closed_flash(root / "immutable/recovery-inputs"),
              "recovered CLOSED baseline changed")
    da_accept.accept_baseline(baseline, root / "recovered-closed-da")
    phase(root, "closed_da_accepted")
    os.sync()
    print("CLOSED DA temporary debug/readback/re-lock passed.\n"
          "Perform one final ECU cold power cycle; then run final-runtime.")


def archive_completed(root: Path):
    destination = m.USER_BACKUP / f"{root.name}-completed"
    m.require(not destination.exists(), "completed recovery archive already exists")
    shutil.copytree(root, destination)
    require_tree_equal(root, destination)
    os.sync()
    return destination


def verify_completed(root: Path):
    """Independently re-evaluate every material result without target access."""
    manifest = verify_transaction(root)
    verify_prepared_backup(root, manifest)
    m.require(phases(root) == PHASES, "Full Regression transaction is not complete")
    inputs = root / "immutable/recovery-inputs"
    opened = m.regular_bytes(root / "recovered-open-flash.bin")
    normal_open = m.regular_bytes(root / "normal-open-full-flash.bin")
    preclose = m.regular_bytes(root / "preclose-open-full-flash.bin")
    expected_open = m.regular_bytes(inputs / "recovery-open-flash.bin")
    closed_flash = m.regular_bytes(root / "recovered-closed-full-flash.bin")
    m.require(opened == normal_open == preclose == expected_open and
              m.digest(opened) == EXPECTED_OPEN_FLASH and
              closed_flash == expected_closed_flash(inputs) and
              m.digest(closed_flash) == EXPECTED_CLOSED_FLASH,
              "completed reconstructed Flash evidence is not exact")
    m.require(m.regular_bytes(root / "post-regression-blank-sample.bin") == b"\xff" * 0x1000,
              "completed transaction lacks the blank post-regression proof")
    regression_log = m.regular_bytes(root / "02-full-regression.log").decode()
    m.require("Debug Authentication Success" in regression_log and
              "Full regression requested: device contents are erased and product state returns OPEN"
              in regression_log, "completed transaction lacks Full Regression success evidence")
    require_regressed_open(m.regular_bytes(root / "03-regressed-open-options-uid.log").decode())
    verify_fresh_obkeys(root)
    recovery.require_runtime_baseline(read_json(root / "runtime-after-open-recovery.json"))
    recovery.require_runtime_baseline(read_json(root / "runtime-final-closed-cold.json"))
    closed.common_discovery(m.regular_bytes(root / "41-provisioning-da-discovery.log").decode(),
                            "ST_LIFECYCLE_PROVISIONING")
    closed.common_discovery(m.regular_bytes(root / "43-closed-da-discovery.log").decode(),
                            "ST_LIFECYCLE_CLOSED")
    evidence = root / "recovered-closed-da"
    da_report = read_json(evidence / "evidence.json")
    ns = m.regular_bytes(evidence / "nonsecure-primary.bin")
    secure = m.regular_bytes(evidence / "secure-primary.bin")
    m.require(da_report["result"] == "PASS" and
              da_report["target"] == dict(m.TARGET, product_state="0x72 CLOSED") and
              da_report["permission"] == "c / bit6 / Level 3 Intrusive Debug" and
              da_report["matches_preclosed_flash_exactly"] is True and
              da_report["debug_relocked"] is True and
              da_report["ordinary_read_before_rc"] != 0 and
              da_report["ordinary_read_after_rc"] != 0 and
              ns == closed_flash[0x100000:0x150000] and
              secure == closed_flash[0x30000:0x60000] and
              da_report["nonsecure_primary_sha256"] == m.digest(ns) and
              da_report["secure_primary_sha256"] == m.digest(secure),
              "completed CLOSED DA evidence is invalid")
    for name in ("02-ordinary-read-denied-before", "08-ordinary-read-denied-after"):
        receipt = read_json(evidence / f"{name}.receipt.json")
        m.require(receipt["returncode"] != 0 and not (evidence / f"{name}.bin").exists(),
                  f"ordinary read denial evidence invalid: {name}")
    expected_report = dict(
        result="PASS", schema=SCHEMA,
        target=dict(m.TARGET, product_state="0x72 CLOSED"),
        full_regression="hardware exercised with DA permission a / bit14",
        reconstructed_open_flash_sha256=EXPECTED_OPEN_FLASH,
        reconstructed_closed_flash_sha256=EXPECTED_CLOSED_FLASH,
        fresh_obkey_receipts=[name for _, name in OBKS],
        atecc_authentication="PASS", ota_accepted_sequence=22,
        outputs="zero", ordinary_stlink_read="DENIED",
        authenticated_hdpl3_readback="EXACT", debug_relocked=True,
        locked_state_used=False)
    m.require(read_json(root / "full-regression-acceptance.json") == expected_report,
              "completed acceptance summary is not canonical")
    return expected_report


def final_runtime(root: Path):
    manifest = verify_transaction(root)
    verify_prepared_backup(root, manifest)
    require_phase(root, "closed_da_accepted")
    state = m.capture(m.SERIAL)
    recovery.require_runtime_baseline(state)
    m.write_json(root / "runtime-final-closed-cold.json", state)
    phase(root, "final_cold_runtime_accepted")
    m.write_json(root / "full-regression-acceptance.json", dict(
        result="PASS", schema=SCHEMA, target=dict(m.TARGET, product_state="0x72 CLOSED"),
        full_regression="hardware exercised with DA permission a / bit14",
        reconstructed_open_flash_sha256=EXPECTED_OPEN_FLASH,
        reconstructed_closed_flash_sha256=EXPECTED_CLOSED_FLASH,
        fresh_obkey_receipts=[name for _, name in OBKS],
        atecc_authentication="PASS", ota_accepted_sequence=22,
        outputs="zero", ordinary_stlink_read="DENIED",
        authenticated_hdpl3_readback="EXACT", debug_relocked=True,
        locked_state_used=False))
    destination = archive_completed(root)
    m.require(verify_completed(root) == verify_completed(destination),
              "completed project/archive verification differs")
    print(f"Full Regression recovery acceptance PASS. Completed evidence backup:\n  {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "verify", "verify-completed", "regress-and-rebuild",
                        "provision-obkeys", "verify-open", "close-recovered",
                        "accept-closed-da", "final-runtime"))
    parser.add_argument("--transaction", type=Path, required=True)
    parser.add_argument("--accept-full-device-erase", action="store_true")
    parser.add_argument("--accept-exact-closed-target", action="store_true")
    parser.add_argument("--accept-boot0-high-estop-disconnected", action="store_true")
    parser.add_argument("--accept-return-to-closed", action="store_true")
    args = parser.parse_args()
    os.umask(0o077)
    root = args.transaction.absolute()
    m.require(root.name.startswith("roller-ecu-SN-EJAHGJQ-full-regression-"),
              "explicit SN-EJAHGJQ Full Regression transaction required")
    with Path("/tmp/roller-ecu-closed-transition.lock").open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.action == "prepare":
            prepare(root)
        elif args.action == "verify":
            print(json.dumps(verify_transaction(root), indent=2))
        elif args.action == "verify-completed":
            print(json.dumps(verify_completed(root), indent=2))
        elif args.action == "regress-and-rebuild":
            m.require(args.accept_full_device_erase and args.accept_exact_closed_target,
                      "regression requires both --accept-full-device-erase and "
                      "--accept-exact-closed-target")
            regress_and_rebuild(root)
        elif args.action == "provision-obkeys":
            m.require(args.accept_boot0_high_estop_disconnected,
                      "OBKeys require --accept-boot0-high-estop-disconnected")
            provision_obkeys(root)
        elif args.action == "verify-open":
            verify_open(root)
        elif args.action == "close-recovered":
            m.require(args.accept_return_to_closed,
                      "return to CLOSED requires --accept-return-to-closed")
            close_recovered(root)
        elif args.action == "accept-closed-da":
            m.require(args.accept_exact_closed_target,
                      "DA acceptance requires --accept-exact-closed-target")
            accept_closed_da(root)
        else:
            final_runtime(root)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f"SN-EJAHGJQ Full Regression recovery stopped: {error}", flush=True)
        raise SystemExit(1)
