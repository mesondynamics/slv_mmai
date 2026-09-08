#!/usr/bin/env python3
"""Bounded SN-EJAHGJQ CLOSED field-service DA acceptance.

The NonSecure 320-KiB slot is read in the same bounded segments used by the
previous ECU because CubeProgrammer may fail one monolithic upload. This tool
does not alter lifecycle, OBKeys, firmware or parameters and cannot regress.
It always attempts close-debug after an authentication attempt.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import re

import finalize_device_closed as closed
import manufacture_open_device as m

SECURE = ("secure-primary", 0x0C030000, 0x30000, 0x30000)
# A running application can make a large CubeProgrammer upload fail only after
# its progress bar reaches 100 %.  Repeated short uploads, however, repeatedly
# reconfigure the debugger SAU and have produced a transient SAU_RBAR error on
# the fifth connection.  Halt once and read the complete contiguous slot in a
# single connection, then explicitly resume before closing the DA window.
NONSECURE = (("nonsecure-primary", 0x08100000, 0, 0x50000),)


def service_command(action):
    return ["python3", str(m.PROJECT / "tools/ecu_debug_auth.py"), action,
            "--accept-closed-target", "--programmer", str(m.CLI),
            "--probe", m.PROBE, "--pki-dir", str(m.PKI),
            "--assets-dir", str(m.PROJECT / "artifacts/security-provisioning")]


def denied(root, name):
    target = root / f"{name}.bin"
    command = [str(m.CLI), "-c", "port=SWD", f"sn={m.PROBE}", "ap=1",
               "mode=UR", "reset=HWrst", "-u", "0x0C030400", "0x100", str(target)]
    output = closed.raw(root, name, command, accept_disconnect=True)
    receipt = closed.json_read(root / f"{name}.receipt.json")
    m.require(receipt["returncode"] != 0 and "Data read successfully" not in output and
              not target.exists(), f"unauthenticated read unexpectedly succeeded: {name}")
    return receipt["returncode"]


def accept_baseline(baseline: bytes, root: Path):
    """Exercise CLOSED DA against one already provenance-checked Flash image.

    The normal acceptance entry point below owns the immutable OPEN-to-CLOSED
    transaction checks.  A Full Regression recovery transaction can reuse the
    exact same hardware test only after independently proving its reconstructed
    CLOSED image and fresh OBKey receipts.
    """
    m.require(not root.exists(), "DA evidence directory already exists")
    m.require(len(baseline) == 0x200000, "CLOSED DA baseline must be exactly 2 MiB")
    closed.require_programmer()
    m.verify_pki_backup()
    root.mkdir(parents=True, mode=0o700)
    opened = False
    halted = False
    close_ok = False
    report = None
    primary_error = None
    cleanup_errors = []
    stage = "preflight"
    try:
        stage = "closed-discovery"
        discovery = closed.raw(root, "01-closed-discovery", service_command("discover"))
        closed.common_discovery(discovery, "ST_LIFECYCLE_CLOSED")
        stage = "ordinary-read-denied-before"
        before_rc = denied(root, "02-ordinary-read-denied-before")
        stage = "open-hdpl3-debug"
        output = closed.raw(root, "03-open-hdpl3-debug", service_command("open-app-debug"))
        m.require("Debug Authentication Success" in output, "DA opening lacks success evidence")
        opened = True

        regions = []
        assembled = bytearray()
        for index, (name, address, offset, size) in enumerate(NONSECURE):
            stage = f"read-{name}"
            path = root / f"{name}.bin"
            # Keep halt and the large upload in one AP1 connection.  Separate
            # connections can eventually fail while CubeProgrammer rewrites
            # its temporary SAU regions, even though the upload itself is
            # byte-exact.
            halted = True
            m.programmer(root, f"04-halt-read-{index:02d}-{name}",
                         ["-halt", "-u", hex(address), hex(size), str(path)],
                         reset=False)
            data = m.regular_bytes(path)
            m.require(data == baseline[0x100000 + offset:0x100000 + offset + size],
                      f"NonSecure segment differs: {name}")
            assembled += data
            regions.append(dict(name=name, address=hex(address), size=size, sha256=m.digest(data)))
        m.require(len(assembled) == 0x50000 and assembled == baseline[0x100000:0x150000],
                  "segmented NonSecure primary is not exact/contiguous")
        stage = "read-secure-primary"
        name, address, offset, size = SECURE
        path = root / f"{name}.bin"
        m.programmer(root, "05-read-secure-primary",
                     ["-u", hex(address), hex(size), str(path)], reset=False)
        data = m.regular_bytes(path)
        m.require(data == baseline[offset:offset + size], "Secure primary differs")
        regions.append(dict(name=name, address=hex(address), size=size, sha256=m.digest(data)))
        closed_target = dict(m.TARGET, product_state="0x72 CLOSED")
        report = dict(result="PASS", target=closed_target, product_state="0x72 CLOSED",
                      permission="c / bit6 / Level 3 Intrusive Debug",
                      secure_primary_sha256=m.digest(m.regular_bytes(root / "secure-primary.bin")),
                      nonsecure_primary_sha256=m.digest(bytes(assembled)), regions=regions,
                      nonsecure_monolithic_upload_while_halted=True,
                      halt_and_nonsecure_upload_single_connection=True,
                      core_halted_for_consistent_readback=True,
                      matches_preclosed_flash_exactly=True, ordinary_read_before_rc=before_rc)
    except (OSError, RuntimeError, ValueError) as error:
        primary_error = error
    finally:
        if halted:
            try:
                m.programmer(root, "06-resume-core", ["-run"], reset=False)
                halted = False
            except (OSError, RuntimeError, ValueError) as error:
                cleanup_errors.append(f"resume-core: {error}")
        if opened:
            try:
                output = closed.raw(root, "07-close-debug", service_command("close-debug"))
                close_ok = "Locking Debug" in output and \
                    "Debug is locked and strict CLOSED discovery passed" in output
                closed.common_discovery(output, "ST_LIFECYCLE_CLOSED")
            except (OSError, RuntimeError, ValueError) as error:
                cleanup_errors.append(f"close-debug: {error}")
        if primary_error is not None or cleanup_errors:
            m.write_json(root / "failure.json", dict(
                result="FAIL", stage=stage,
                error=str(primary_error) if primary_error is not None else None,
                cleanup_errors=cleanup_errors, debug_relocked=close_ok,
                lifecycle_or_flash_write_attempted=False))
    if primary_error is not None:
        raise primary_error
    m.require(not cleanup_errors and opened and close_ok and not halted and report is not None,
              "DA session was not safely resumed and re-locked")
    report["ordinary_read_after_rc"] = denied(root, "08-ordinary-read-denied-after")
    report["debug_relocked"] = True
    report["cold_power_cycle_required"] = True
    m.write_json(root / "evidence.json", report)
    os.sync()
    print(json.dumps(report, indent=2))


def accept(transaction: Path, root: Path):
    manifest = closed.verify_transaction(transaction)
    m.require(closed.phases(transaction) == closed.PHASES,
              "source transaction has not reached verified CLOSED")
    backup = m.USER_BACKUP / transaction.name
    m.require(closed.verify_transaction(backup) == manifest,
              "independent pre-mutation transaction backup differs")
    closed.verify_open_obkey_receipts()
    baseline = m.regular_bytes(transaction / "closed-loader-full-flash.bin")
    expected = bytearray(m.regular_bytes(transaction / "immutable/capture/full-flash-before.bin"))
    loader = m.regular_bytes(transaction / "immutable/recovery-inputs/ReleaseClosed-ECU_OEMiROT.bin")
    expected[:len(loader)] = loader
    m.require(baseline == bytes(expected), "CLOSED Flash baseline is not exact")
    accept_baseline(baseline, root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transaction", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--accept-closed-target", action="store_true")
    args = parser.parse_args()
    m.require(args.accept_closed_target, "requires --accept-closed-target")
    os.umask(0o077)
    with Path("/tmp/roller-ecu-closed-transition.lock").open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        accept(args.transaction.absolute(), args.evidence.absolute())


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f"CLOSED DA acceptance stopped: {error}")
        raise SystemExit(1)
