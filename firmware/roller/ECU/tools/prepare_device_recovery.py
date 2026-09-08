#!/usr/bin/env python3
"""Offline SN-EJAHGJQ 1.0.22 recovery INPUTS; cannot close, erase or program a chip.

The fixed source is the reviewed OPEN OTA readback, not a mutable build output.
Canonical recovery uses signed INITIAL images, the board's own persistent
records and an empty scratch/secondary area. It does not replay an old swap.
Preparing these inputs is NOT a validated Full Regression procedure or a
CLOSED authorization. Fresh hardware identity/readback, DA and lifecycle
transaction gates remain mandatory. No hardware command is implemented here.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil

import manufacture_open_device as m
import verify_device_release as images
from package_firmware import strict_json_object

VERSION = "1.0.22"
SCHEMA = "roller-ecu-new-board-recovery-inputs-v1"
SOURCE_FLASH_SHA256 = "09d1efdbe3b6cc9697ef5d1e4d45e7358ffb42c4d8e18f121bf333da904a315e"
SOURCE = m.PROJECT / "artifacts/device-backups/SN-EJAHGJQ/open-1.0.22-acceptance"
MANUFACTURING = m.PROJECT / ("artifacts/device-backups/SN-EJAHGJQ/"
                             "roller-ecu-SN-EJAHGJQ-open-1.0.21-20260905T121300Z")
SUPPORT = {name: value for name, value in m.PINNED.items() if name.startswith("Release")}
SUPPORT.update(m.PINNED_OBKS)
SUPPORT.update({
    "cert-root.b64": (296, "bd51ea5fac02f65baea3e3ca0c38edc6466ce22206e3a2c9b6ef0a8cfc6d0069"),
    "cert-intermediate.b64": (296, "8a86f656be2d5e4175174d9bcb4b11e36181448608a89c91a2d52c828c9ca269"),
    "cert-leaf.b64": (296, "66147302a78fc4bc70ac3f46d4f3418fd3e73acb91ee82abb030efb3da1d0968"),
    "cert-leaf-chain.b64": (880, "34054034f2a26e46343aefbb4d0696d9a6b831726af6574ddaa59404182791e9"),
    "security-assets.json": (1083, "e31905517fafed90dd5d1c2fcad283e74e600524d7b488006cde0560a242a573"),
    "oemirot-auth-s-public.pem": (178, "6729d0f9e12328912e63ee7d6f509505d1821262e220d5ac53a0ede004cedf37"),
    "oemirot-auth-ns-public.pem": (178, "e4f8604410ec42ddfd058eb08793c3ed45094039a7099cfc05560e0b2c7bf09e"),
    "oemirot-encryption-public.pem": (178, "e43f30d2be979619bccbd579c78b859c204dfe401737feb222d671324330bed8"),
    "ota-transport-public.pem": (178, "9c26918e79143d8721cc24b0bae1aa118eb0c1b7565f5e54e5faff9352e403c4"),
})
EVIDENCE = {"source-open-flash.bin", "source-option-bytes-and-uid.log",
            "source-runtime-final.json", "source-runtime-pinless.json"}
DERIVED = {"secure-persistent.bin", "pairing-store.bin", "recovery-open-flash.bin"}
FILES = set(SUPPORT) | EVIDENCE | DERIVED | {
    f"{VERSION}/{name}" for name in images.RELEASES[VERSION]}
READINESS = dict(offline_inputs_only=True, lifecycle_write_authorized=False,
                 hardware_full_regression_tested=False,
                 source_parameter_state="unsaved-defaults-generation-0",
                 requires_fresh_target_and_persistent_readback=True,
                 shared_private_keys="../roller-ecu-pki (separate backup required)")


def read_json(path):
    return json.loads(m.regular_bytes(path), object_pairs_hook=strict_json_object)


def require_captured_flash(data):
    m.require(len(data) == 0x200000 and m.digest(data) == SOURCE_FLASH_SHA256,
              "source is not the exact reviewed new-board OPEN 1.0.22 readback")
    m.verify_dual_store(data[0xE0000:0xE4000], identity=m.IDENTITY)
    m.require(data[0xFA000:0xFE000] == b"\xff" * 0x4000,
              "reviewed recovery baseline expects no saved PI records")


def build_recovery_flash(source, boot, secure, nonsecure):
    require_captured_flash(source)
    m.require((len(boot), m.digest(boot)) == SUPPORT["ReleaseOpen-ECU_OEMiROT.bin"],
              "wrong recovery loader")
    for name, data in (("secure-initial.bin", secure), ("nonsecure-initial.bin", nonsecure)):
        m.require((len(data), m.digest(data)) == images.RELEASES[VERSION][name],
                  f"wrong signed recovery initial image: {name}")
    recovery = bytearray(b"\xff" * 0x200000)
    recovery[:len(boot)] = boot
    recovery[0x30000:0x60000] = secure
    recovery[0x100000:0x150000] = nonsecure
    recovery[0xE0000:0x100000] = source[0xE0000:0x100000]
    return bytes(recovery)


def require_runtime_baseline(state):
    m.require_paired_safe(state, m.SERIAL, accepted_sequence=22)
    m.require(state.get("device_serial") == m.SERIAL and state.get("ecu_ip") == m.TARGET["ecu_ip"] and
              state["ota"]["ota_result"] == 0 and
              state["diagnostic"]["safety_status"] & (1 << 22) and
              not state["diagnostic"]["safety_status"] & (7 << 23), "unconfirmed source runtime")
    config = state["valve_config"]
    m.require(config["result"] == 0 and config["persisted_generation"] == 0 and
              config["persisted_crc32c"] == 0 and config["persisted_valid"] == 0 and
              config["using_defaults"] == 1 and config["dirty"] == 0,
              "source PI state is not the captured unsaved default baseline")


def require_manifest(manifest):
    m.require(set(manifest) == {"schema", "target", "release", "readiness", "files"} and
              manifest["schema"] == SCHEMA and manifest["target"] == m.TARGET and
              manifest["release"] == dict(version=VERSION, counter=22, accepted_sequence=22) and
              manifest["readiness"] == READINESS and set(manifest["files"]) == FILES,
              "recovery inputs target/schema/readiness/file set mismatch")


def verify(root):
    m.require(root.is_dir() and not root.is_symlink(), "not a regular recovery input directory")
    paths = list(root.rglob("*"))
    m.require(not any(path.is_symlink() for path in paths), "symlink recovery input is forbidden")
    m.require({str(path.relative_to(root)) for path in paths if path.is_file()} == FILES | {"manifest.json"},
              "missing or extraneous recovery files")
    manifest = read_json(root / "manifest.json")
    require_manifest(manifest)
    for name in FILES:
        data = m.regular_bytes(root / name)
        m.require(manifest["files"][name] == dict(size=len(data), sha256=m.digest(data)),
                  f"recovery file changed: {name}")
        if name in SUPPORT:
            m.require((len(data), m.digest(data)) == SUPPORT[name], f"unreviewed recovery support: {name}")
    source = m.regular_bytes(root / "source-open-flash.bin")
    require_captured_flash(source)
    m.require_rss_options(m.regular_bytes(root / "source-option-bytes-and-uid.log").decode())
    for name in ("source-runtime-final.json", "source-runtime-pinless.json"):
        require_runtime_baseline(read_json(root / name))
    m.require(m.regular_bytes(root / "secure-persistent.bin") == source[0xE0000:0x100000] and
              m.regular_bytes(root / "pairing-store.bin") == source[0xE0000:0xE4000],
              "persistent/pairing extraction does not match the fixed source")
    installed = images.audit_flash(source, root / VERSION, root, "installed", "ReleaseOpen")
    canonical = build_recovery_flash(source, m.regular_bytes(root / "ReleaseOpen-ECU_OEMiROT.bin"),
                                    m.regular_bytes(root / VERSION / "secure-initial.bin"),
                                    m.regular_bytes(root / VERSION / "nonsecure-initial.bin"))
    m.require(m.regular_bytes(root / "recovery-open-flash.bin") == canonical,
              "noncanonical recovery image; never restore stale secondary/scratch/trailer state")
    initial = images.audit_flash(canonical, root / VERSION, root, "initial", "ReleaseOpen")
    return dict(result="PASS", schema=SCHEMA, target=m.TARGET,
                source_flash_sha256=installed["full_flash_sha256"],
                recovery_flash_sha256=initial["full_flash_sha256"],
                pairing_sha256=initial["pairing"]["store_sha256"], readiness=READINESS)


def prepare(root):
    m.require(not root.exists() and not (m.USER_BACKUP / root.name).exists(),
              "recovery output/backup already exists; never overwrite")
    m.verify_pki_backup()  # No key generation is used to repair missing inputs.
    m.verify_staged(MANUFACTURING)
    m.verify_assets(m.PKI, MANUFACTURING)
    source = m.regular_bytes(SOURCE / "full-flash.bin")
    require_captured_flash(source)
    images.audit_flash(source, m.PROJECT / "artifacts/firmware" / VERSION, m.PKI, "installed", "ReleaseOpen")
    for filename in ("runtime-final.json", "runtime-pinless-cold.json"):
        require_runtime_baseline(read_json(SOURCE / filename))
    root.mkdir(mode=0o700, parents=True)
    (root / VERSION).mkdir(mode=0o700)
    for name in SUPPORT:
        data = m.regular_bytes(MANUFACTURING / name)
        m.require((len(data), m.digest(data)) == SUPPORT[name], f"unreviewed input {name}")
        m.write_new(root / name, data)
    for name in images.RELEASES[VERSION]:
        m.write_new(root / VERSION / name, m.regular_bytes(m.PROJECT / "artifacts/firmware" / VERSION / name))
    for source_name, name in (("full-flash.bin", "source-open-flash.bin"),
                             ("01-open-identity.log", "source-option-bytes-and-uid.log"),
                             ("runtime-final.json", "source-runtime-final.json"),
                             ("runtime-pinless-cold.json", "source-runtime-pinless.json")):
        m.write_new(root / name, m.regular_bytes(SOURCE / source_name))
    m.write_new(root / "secure-persistent.bin", source[0xE0000:0x100000])
    m.write_new(root / "pairing-store.bin", source[0xE0000:0xE4000])
    canonical = build_recovery_flash(source, m.regular_bytes(root / "ReleaseOpen-ECU_OEMiROT.bin"),
                                    m.regular_bytes(root / VERSION / "secure-initial.bin"),
                                    m.regular_bytes(root / VERSION / "nonsecure-initial.bin"))
    m.write_new(root / "recovery-open-flash.bin", canonical)
    m.write_json(root / "manifest.json", dict(schema=SCHEMA, target=m.TARGET,
                 release=dict(version=VERSION, counter=22, accepted_sequence=22), readiness=READINESS,
                 files={name: dict(size=(root / name).stat().st_size,
                                    sha256=m.digest(m.regular_bytes(root / name))) for name in sorted(FILES)}))
    report = verify(root)
    backup = m.USER_BACKUP / root.name
    shutil.copytree(root, backup)
    m.require(verify(backup) == report and
              m.regular_bytes(root / "manifest.json") == m.regular_bytes(backup / "manifest.json"),
              "independent backup verification differs")
    os.sync()
    print(f"Offline recovery INPUTS verified in two copies: {root}\nBackup: {backup}\n"
          "No hardware command sent. CLOSED/Full Regression procedure NOT yet authorized or tested.")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "verify"))
    parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    report = prepare(args.directory.resolve()) if args.action == "prepare" else verify(args.directory)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
