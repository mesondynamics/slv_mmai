#!/usr/bin/env python3
"""Commit one hardware-validated valve PI candidate to the selected ECU.

The candidate must come from ecu_pi_compare.py, still be the active zero-output
RAM configuration, and pass the selected board's passive identity/safety gate.
This tool never actuates a valve and performs exactly one CONFIG_SAVE request.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import time

import ecu_bench_targets as targets
import ecu_debug_ui as ui


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_candidate(path: Path, device, kp: int, ki: int):
    require(path.is_file() and not path.is_symlink(), "candidate report must be a regular file")
    report = json.loads(path.read_text())
    require(report.get("passed") is True and report.get("zero_verified") is True and
            report.get("flash_written") is False and
            report.get("device_serial") == device.serial and
            report.get("ecu_ip") == device.ecu_ip and
            report.get("kp") == kp and report.get("ki") == ki,
            "candidate report identity/result/gain mismatch")
    candidate = report.get("candidate")
    require(isinstance(candidate, dict) and
            all(candidate[channel]["kp_permille_per_amp"] == kp and
                candidate[channel]["ki_permille_per_amp_second"] == ki
                for channel in ("forward", "reverse")),
            "candidate config does not contain the selected two-channel gains")
    cases = report.get("cases", [])
    require(len(cases) == 10 and
            {case["target_ma"] for case in cases} == {-500, -200, -100, 100, 200, 500} and
            all(case["settled_5pct_20ms_mean_ms"] is not None and
                abs(case["tail_feedback_min_mean_max_ma"][1] - abs(case["target_ma"])) <=
                max(5, 0.03 * abs(case["target_ma"])) for case in cases),
            "candidate lacks the complete passing dynamic test")
    return report, candidate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    targets.add_target_arguments(parser)
    parser.add_argument("--kp", type=int, required=True)
    parser.add_argument("--ki", type=int, required=True)
    parser.add_argument("--candidate-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--accept-parameter-flash-write", action="store_true")
    args = parser.parse_args()
    require(args.accept_parameter_flash_write,
            "CONFIG_SAVE requires --accept-parameter-flash-write")
    device = targets.resolve_target(args.device_serial, args.ecu_ip)
    require(not args.output.exists(), "commit evidence already exists; never overwrite")
    report, candidate = read_candidate(args.candidate_report, device, args.kp, args.ki)
    preflight = targets.passive_preflight(device)
    bridge = ui.BenchBridge(device.ecu_ip)
    bridge.start()
    evidence = dict(result="FAIL", device_serial=device.serial, ecu_ip=device.ecu_ip,
                    kp=args.kp, ki=args.ki, candidate_report=str(args.candidate_report),
                    candidate_report_size=args.candidate_report.stat().st_size,
                    passive_preflight=preflight, candidate_metrics=report["cases"])
    try:
        time.sleep(0.6)
        before = bridge.config_request(ui.MSG_CONFIG_GET)
        require(before["result"] == 0 and before["config"] == candidate and before["dirty"] and
                not before["persisted_valid"],
                "active RAM candidate is not the reviewed first-save state")
        saved = bridge.config_request(ui.MSG_CONFIG_SAVE)
        require(saved["result"] == 0 and saved["config"] == candidate and
                saved["persisted_valid"] and not saved["dirty"] and
                not saved["using_defaults"] and
                saved["persisted_generation"] == before["persisted_generation"] + 1 and
                saved["persisted_crc32c"] != 0,
                "CONFIG_SAVE did not atomically commit the reviewed candidate")
        time.sleep(0.25)
        verified = bridge.config_request(ui.MSG_CONFIG_GET)
        require(all(verified[key] == saved[key] for key in saved if key != "request_sequence"),
                "fresh CONFIG_GET differs after SAVE")
        snap = bridge.snapshot()
        status = snap.get("status") or {}
        require(snap.get("status_age_ms") is not None and snap["status_age_ms"] < 300 and
                all(status.get(key) == 0 for key in ("valve_requested_target_ma",
                    "valve_applied_target_ma", "forward_duty_permille", "reverse_duty_permille")),
                "outputs are not zero after parameter SAVE")
        evidence.update(result="PASS", before=before, saved=saved, verified=verified,
                        zero_output_after_save=True)
    finally:
        bridge.close()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x") as stream:
            json.dump(evidence, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
    print(json.dumps({key: evidence.get(key) for key in
                      ("result", "device_serial", "ecu_ip", "kp", "ki",
                       "zero_output_after_save")}, indent=2))
    return 0 if evidence["result"] == "PASS" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, TimeoutError, ValueError, json.JSONDecodeError) as error:
        print(f"PI commit stopped: {error}")
        raise SystemExit(1)
