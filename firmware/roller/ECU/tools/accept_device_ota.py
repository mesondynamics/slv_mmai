#!/usr/bin/env python3
"""Bounded SN-EJAHGJQ / 1.0.22 bench OTA acceptance, with physical E-stop held.

Not a general updater or lifecycle tool. Each phase has a new durable evidence
directory; interrupted phases must be inspected, never automatically replayed.
No actuator, parameter SAVE, Option Byte, Flash erase or ST-Link command is sent
by this host tool. Valid BEGIN does erase the ECU's OTA secondary slots.
Between partial and transfer, an independently verified hardware reset/readback
must prove the original initial images still boot. CLOSED/DA are separate gates.
"""
from __future__ import annotations

import argparse
import fcntl
from pathlib import Path
import struct
import time

import ethernet_ota as ota
import manufacture_open_device as m
import verify_device_release as audit

VERSION = "1.0.22"
RELEASE = m.PROJECT / "artifacts/firmware" / VERSION
PACKAGE = RELEASE / f"roller-ecu-{VERSION}.recu"


def require_state(status, *, accepted, state, secure_received, nonsecure_received=0):
    expected = dict(result=0, accepted_sequence=accepted, state=state,
                    secure_received=secure_received, nonsecure_received=nonsecure_received)
    m.require(all(status.get(k) == v for k, v in expected.items()),
              f"unexpected OTA state: expected {expected}, received {status}")


def chunk(data, *, offset=0, crc_error=False):
    return struct.pack(ota.CHUNK_FORMAT, 22, 0, offset, len(data), 0,
                       ota.crc32c(data) ^ int(crc_error), data.ljust(512, b"\0"))


def partial(root, client, begin, secure):
    monitor = ota.OtaSafetyMonitor(m.TARGET["ecu_ip"])
    records = []
    try:
        monitor.arm()

        def check(name, kind, payload, *, result, state, received):
            monitor.require_held()
            m.require(monitor.latest["requested_relay_mask"] == 0 and
                      monitor.latest["applied_relay_mask"] == 0,
                      "nonzero relay request/output during acceptance")
            m.write_json(root / f"{name}-intent.json", dict(message=kind,
                         payload_sha256=m.digest(payload), expected_result=result))
            reply = client.request(kind, payload, timeout=20 if kind == ota.MSG_BEGIN else 0.6,
                                   attempts=1)
            m.write_json(root / f"{name}-reply.json", reply)
            m.require(reply["result"] == result, f"{name} result mismatch: {reply}")
            status = client.status()
            m.write_json(root / f"{name}-status.json", status)
            require_state(status, accepted=0, state=state, secure_received=received)
            monitor.require_held()
            records.append(name)
            print(f"PASS {name}: result={result}, state={state}, secure_received={received}", flush=True)

        bad = bytearray(begin)
        bad[128] ^= 1
        check("01-bad-transport-signature", ota.MSG_BEGIN, bad, result=-16, state=0, received=0)
        bad = bytearray(begin)
        bad[24] ^= 1  # Version field changed, still structurally valid.
        check("02-manifest-tampering", ota.MSG_BEGIN, bad, result=-16, state=0, received=0)
        bad = bytearray(begin)
        bad[112] = 1  # Reserved manifest field: must reject before erase.
        check("03-reserved-field", ota.MSG_BEGIN, bad, result=-1, state=0, received=0)
        check("04-valid-begin", ota.MSG_BEGIN, begin, result=0, state=1, received=0)
        data = secure[:512]
        check("05-bad-chunk-crc", ota.MSG_CHUNK, chunk(data, crc_error=True),
              result=-17, state=1, received=0)
        check("06-out-of-order", ota.MSG_CHUNK, chunk(secure[512:1024], offset=512),
              result=-11, state=1, received=0)
        check("07-first-chunk", ota.MSG_CHUNK, chunk(data), result=0, state=1, received=512)
        check("08-identical-retry", ota.MSG_CHUNK, chunk(data), result=0, state=1, received=512)
        changed = bytes([data[0] ^ 1]) + data[1:]
        check("09-changed-retry", ota.MSG_CHUNK, chunk(changed), result=-11, state=1, received=512)
        check("10-premature-finish", ota.MSG_FINISH, struct.pack("<I", 22),
              result=-11, state=1, received=512)
    finally:
        monitor.close()
    # The normal updater's BEGIN must resume at 512, not erase/restart.
    events = []
    try:
        ota.transfer(client, PACKAGE, m.PKI / "ota-transport-public.pem",
                     progress=events.append, stop_after_bytes=8192)
    except ota.IntentionalInterruption as error:
        print(str(error), flush=True)
    else:
        raise RuntimeError("intentional stop unexpectedly finished")
    m.write_json(root / "resume-events.json", events)
    progress = next(event for event in events if event["stage"] == "transfer")
    m.require(progress["transferred"] == 512, "updater did not resume existing staged bytes")
    status = client.status()
    require_state(status, accepted=0, state=1, secure_received=8192)
    m.write_json(root / "partial-PASS.json", dict(result="PASS", checks=records,
                 resumed_at=512, deliberately_stopped_at=8192, status=status,
                 next_gate="hardware-reset-and-exact-initial-image-readback"))


def transfer(root, client):
    events = []
    try:
        ota.transfer(client, PACKAGE, m.PKI / "ota-transport-public.pem", progress=events.append)
    finally:
        m.write_json(root / "transfer-events.json", events)
    status = client.status()
    require_state(status, accepted=22, state=0, secure_received=0)
    m.require(status["ota_result"] == 0, "post-update OTA error")
    m.write_json(root / "transfer-PASS.json", dict(result="PASS", status=status))


def replay(root, client, begin):
    monitor = ota.OtaSafetyMonitor(m.TARGET["ecu_ip"])
    try:
        monitor.arm()
        audit.load_release(m.PROJECT / "artifacts/firmware/1.0.21", m.PKI)
        previous, *_ = ota.load_package(m.PROJECT / "artifacts/firmware/1.0.21/roller-ecu-1.0.21.recu",
                                        m.PKI / "ota-transport-public.pem")
        for name, payload in (("same-sequence-22", begin), ("older-sequence-21", previous)):
            monitor.require_held()
            m.write_json(root / f"{name}-intent.json", dict(payload_sha256=m.digest(payload)))
            reply = client.request(ota.MSG_BEGIN, payload, timeout=2, attempts=1)
            m.write_json(root / f"{name}-reply.json", reply)
            m.require(reply["result"] == -19, f"rollback/replay was not rejected: {reply}")
            require_state(client.status(), accepted=22, state=0, secure_received=0)
            monitor.require_held()
            print(f"PASS {name}: rollback rejection -19, accepted sequence still 22", flush=True)
    finally:
        monitor.close()
    m.write_json(root / "replay-PASS.json", dict(result="PASS", accepted_sequence=22,
                 next_gate="reset-clear-last-test-result-then-signed-pair-readback"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("partial", "transfer", "replay"))
    parser.add_argument("--evidence-dir", type=Path, required=True)
    args = parser.parse_args()
    with open("/tmp/roller-ecu-closed-transition.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        audit.load_release(RELEASE, m.PKI)
        root = args.evidence_dir.resolve()
        m.require(root.is_relative_to(m.PROJECT / "artifacts/device-backups/SN-EJAHGJQ") and
                  not root.exists(), "require a NEW new-board evidence directory; no automatic replay")
        state = m.capture(m.SERIAL)
        m.require_paired_safe(state, m.SERIAL, accepted_sequence=22 if args.phase == "replay" else 0)
        root.mkdir(mode=0o700)
        m.write_json(root / "runtime-before.json", state)
        m.write_json(root / "phase-intent.json", dict(phase=args.phase, version=VERSION,
                     target=m.TARGET, utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())))
        begin, secure, _, _ = ota.load_package(PACKAGE, m.PKI / "ota-transport-public.pem")
        client = ota.OtaClient("172.16.0.10", m.TARGET["ecu_ip"])
        try:
            if args.phase == "partial":
                partial(root, client, begin, secure)
            elif args.phase == "transfer":
                transfer(root, client)
            else:
                replay(root, client, begin)
        finally:
            client.close()


if __name__ == "__main__":
    main()
