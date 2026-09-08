#!/usr/bin/env python3
"""Offline audit of SN-EJAHGJQ OPEN readback/NRST evidence, not a CLOSED writer.

Evidence consistency is not a signature or proof of current physical continuity.
A subsequent lifecycle transaction must independently check the live target,
its latest persistent records, both backups and explicit operator authority.
This tool never connects, resets, programs, changes lifecycle or reads keys.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import json
from pathlib import Path
import re

import manufacture_open_device as m
import prepare_device_recovery as recovery

SCHEMA = "roller-ecu-new-board-open-preflight-audit-v1"
PULSE_COMMAND = [str(m.CLI), "-c", "port=SWD", f"sn={m.PROBE}", "mode=HWRSTPULSE"]


def require_safe(state):
    recovery.require_runtime_baseline(state)
    m.require(state["diagnostic"]["valid_control_frames"] == 0 and
              state["diagnostic"]["telemetry_frames_sent"] == 0,
              "control or high-rate telemetry was active during manufacturing")
    m.require(state["security"]["flags"] & 0x10 and
              not state["security"]["flags"] & 0x20,
              "security authentication/quarantine flags inconsistent")
    for name in ("update_sequence", "secure_received", "nonsecure_received",
                 "secure_image_size", "nonsecure_image_size"):
        m.require(state["ota"][name] == 0, f"OTA not fully idle: {name}")


def utc(state):
    return datetime.strptime(state["captured_utc"], "%Y-%m-%dT%H:%M:%SZ")


def verify_pulse(before, after, intent, result, log):
    require_safe(before)
    require_safe(after)
    old = before["diagnostic"]["secure_uptime_ms"]
    new = after["diagnostic"]["secure_uptime_ms"]
    m.require(intent == dict(command=PULSE_COMMAND, before_uptime_ms=old,
                             software_reset_fallback=False),
              "NRST intent must select hardware pulse only, no reset fallback")
    m.require(result["target"] == m.TARGET and result["command"] == PULSE_COMMAND and
              result["result"] == "PASS" and type(result["returncode"]) is int and
              result["returncode"] == 0 and result["before_uptime_ms"] == old and
              result["after_uptime_ms"] == new and
              all(result[k] is False for k in ("software_reset_fallback",
                                                "lifecycle_written", "flash_written")),
              "NRST receipt/target does not match original observations")
    elapsed = result["elapsed_s"]
    m.require(type(elapsed) in (int, float) and 0 < elapsed <= 45 and
              old >= 30000 and 0 < new < old and new <= elapsed * 1000 + 1000 and
              abs((utc(after) - utc(before)).total_seconds() - elapsed) < 2,
              "NRST uptime/time evidence does not establish a fresh boot")
    m.require(re.search(r"^Connect mode:\s*hwRstPulse\s*$", log, re.M) and
              re.search(rf"^ST-LINK SN\s*:\s*{m.PROBE}\s*$", log, re.M) and
              not re.search(r"\b(error|failed|failure)\b", log, re.I),
              "no successful hardware pulse receipt from the reviewed probe")
    m.require(before["valve_config"]["config"] == after["valve_config"]["config"],
              "parameters changed across NRST")
    return dict(before_uptime_ms=old, after_uptime_ms=new, elapsed_s=elapsed)


def verify_stability(root, after):
    report = recovery.read_json(root / "stability-PASS.json")
    samples = report["samples"]
    m.require(report["result"] == "PASS" and report["hardware_commands_sent"] is False and
              report["control_sent"] is False and len(samples) == 13 and
              180 <= report["duration_s"] <= 210,
              "incomplete passive observation window")
    previous = None
    for i, sample in enumerate(samples):
        m.require(sample["file"] == f"stability-{i:02d}.json", "noncanonical observation file")
        state = recovery.read_json(root / sample["file"])
        require_safe(state)
        m.require(state["valve_config"]["config"] == after["valve_config"]["config"],
                  "PI configuration changed during observation")
        uptime = state["diagnostic"]["secure_uptime_ms"]
        m.require(sample["uptime_ms"] == uptime and 0 < sample["elapsed_s"] <= report["duration_s"],
                  "observation summary not supported by snapshot")
        reference = previous[1] if previous else after
        delta = uptime - reference["diagnostic"]["secure_uptime_ms"]
        wall = (utc(state) - utc(reference)).total_seconds() * 1000
        m.require(delta > 0 and abs(delta - wall) < 2000,
                  "unexpected reboot, stopped uptime or inconsistent observation timestamp")
        if previous:
            interval = sample["elapsed_s"] - previous[0]["elapsed_s"]
            m.require(10 < interval < 20 and abs(interval * 1000 - delta) < 2000,
                      "inconsistent monotonic observation interval")
        previous = sample, state
    m.require(samples[-1]["elapsed_s"] - samples[0]["elapsed_s"] >= 179,
              "observation samples span less than three minutes")
    return report


def verify(root, inputs):
    m.require(root.is_dir() and not root.is_symlink(), "not a regular preflight directory")
    m.require(not any(path.is_symlink() for path in root.rglob("*")), "symlink evidence is forbidden")
    inputs_report = recovery.verify(inputs)
    option_log = m.regular_bytes(root / "01-fresh-open-identity.log").decode()
    m.require_rss_options(option_log)
    m.require(re.findall(r"^\s*LOCKBL\s*:\s*0x([0-9a-fA-F]+)\b", option_log, re.M) == ["0"],
              "bootloader lock is not the reviewed recoverable setting")
    flash = m.regular_bytes(root / "full-flash.bin")
    recovery.require_captured_flash(flash)
    m.require(flash == m.regular_bytes(inputs / "source-open-flash.bin") and
              m.regular_bytes(root / "secure-persistent.bin") == flash[0xE0000:0x100000],
              "fresh full Flash/persistent records differ from recovery baseline")
    # Do not trust the stored PASS or its hashes instead of re-verifying images.
    flash_audit = recovery.images.audit_flash(flash, inputs / recovery.VERSION, inputs,
                                              "installed", "ReleaseOpen")
    m.require(recovery.read_json(root / "flash-audit.json") == flash_audit,
              "stored Flash audit differs from independently verified data")
    before = recovery.read_json(root / "runtime-before-nrst.json")
    after = recovery.read_json(root / "runtime-after-nrst.json")
    for name in ("runtime-before.json", "runtime-before-readback.json"):
        state = recovery.read_json(root / name)
        require_safe(state)
        m.require(state["valve_config"]["config"] == before["valve_config"]["config"] and
                  utc(state) <= utc(before), "pre-readback observation order or config mismatch")
    pulse = verify_pulse(before, after, recovery.read_json(root / "physical-nrst-intent.json"),
                         recovery.read_json(root / "physical-nrst-PASS.json"),
                         m.regular_bytes(root / "04-physical-nrst.log").decode())
    stability = verify_stability(root, after)
    return dict(result="PASS", schema=SCHEMA, target=m.TARGET,
                source_flash_sha256=flash_audit["full_flash_sha256"],
                recovery_flash_sha256=inputs_report["recovery_flash_sha256"],
                physical_nrst=pulse, stability_duration_s=stability["duration_s"],
                last_observation_utc=recovery.read_json(root / "stability-12.json")["captured_utc"],
                readiness=dict(historical_evidence_only=True, lifecycle_write_authorized=False,
                               hardware_da_tested=False, hardware_full_regression_tested=False,
                               fresh_live_target_and_backups_required=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--recovery-inputs", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(verify(args.directory, args.recovery_inputs), indent=2))


if __name__ == "__main__":
    main()
