#!/usr/bin/env python3
"""Bounded unloaded-bench valve tests; never apply or save PI parameters.

These host guards supplement, but do not replace, the ECU's Secure protection.
They intentionally stop below the firmware's >=300 mA open-load threshold.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path
import signal
import statistics
import subprocess
import time

import ecu_debug_ui as ui
import ethernet_ota as ota
import ecu_bench_targets as targets


class GuardFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GuardFailure(message)


def sample_guard(sample: dict, target: int, elapsed: float) -> None:
    forward = sample["forward_current_ma"]
    reverse = sample["reverse_current_ma"]
    fd = sample["forward_duty_permille"]
    rd = sample["reverse_duty_permille"]
    require(not sample["fault_flags"], f"valve fault: {sample['fault_flags']:#x}")
    require(not (fd and rd), "both PWM channels active")
    require(max(fd, rd) <= 300, f"bench duty guard exceeded: {fd}/{rd} permille")
    # A zero command can initially retain the preceding 200 mA coil current.
    # Allow its normal decay, then enforce <50 mA below after 300 ms.
    current_limit = 350 if target == 0 else max(200, abs(target) * 1.5 + 50)
    require(max(forward, reverse) <= current_limit,
            f"bench current guard exceeded: {forward}/{reverse} mA")
    if target:
        current, other = (forward, reverse) if target > 0 else (reverse, forward)
        require((rd if target > 0 else fd) == 0, "wrong-direction PWM")
        require(other < 50, f"inactive channel current: {other} mA")
        if elapsed >= 0.35:
            require(sample["requested_target_ma"] == target,
                    "ECU is not following this test's target/session")
            require(current >= 20,
                    f"no current response at {elapsed:.3f}s: "
                    f"target={target}, feedback={current} mA")
    elif elapsed >= 0.30:
        require(fd == rd == 0 and max(forward, reverse) < 50,
                "zero target did not settle to zero PWM/current")


def run(args: argparse.Namespace) -> int:
    device = targets.resolve_target(args.device_serial, args.ecu_ip)
    preflight = targets.passive_preflight(device)
    # Create a new evidence directory; never overwrite a previous run.
    args.output_dir.mkdir(mode=0o700, parents=False, exist_ok=False)
    report: dict = {"ecu_ip": args.ecu_ip, "direction": args.direction,
                    "device_serial": device.serial, "passive_preflight": preflight,
                    "passed": False, "cases": [], "parameters_written": False}
    samples: list[dict] = []
    bridge = ui.BenchBridge(args.ecu_ip)
    bridge.start()
    token = f"guarded-valve-{time.time_ns()}"
    last_ui_sequence = 0
    last_sample_sequence: int | None = None
    baseline_dropped: int | None = None
    zero_verified = False

    def status_guard(*, require_armed: bool = True) -> dict:
        snap = bridge.snapshot()
        for name in ("status", "diagnostic", "security", "steering"):
            age = snap[name + "_age_ms"]
            require(age is not None and age < 300, f"stale {name}: {age} ms")
        require(not snap["physical_estop_active"], "physical E-stop active")
        require(not snap["network_estop_latched"], "network E-stop latched")
        require(not snap["critical_inhibit"], "critical safety inhibit")
        require(targets.security_matches(snap["security"], device), "bench target identity changed")
        require(not snap["diagnostic"]["valve_fault_flags"], "latched valve fault")
        steering = snap["steering_status"]
        require(not steering["command_enable"] and not steering["speed_command_permille"],
                "steering is not passive")
        if require_armed:
            require(snap["diagnostic"]["safety_status"] & (1 << 3), "outputs not armed")
            require(snap["status"]["control_mode"] == 2 and
                    snap["status"]["active_sender_id"] == 2, "control authority lost")
        return snap

    def observe(stage: str, target: int, duration: float) -> list[dict]:
        nonlocal last_ui_sequence, last_sample_sequence, baseline_dropped
        start = time.monotonic()
        stage_samples: list[dict] = []
        while time.monotonic() - start < duration:
            # Only the supervising thread renews the deadman and waveform lease.
            bridge.heartbeat()
            bridge.set_tuning_session(token, True)
            snap = status_guard()
            require(snap["telemetry_age_ms"] is not None and
                    snap["telemetry_age_ms"] < 100, "waveform feedback stale")
            with bridge.lock:
                fresh = [dict(s) for s in bridge.telemetry_history
                         if s["ui_sequence"] > last_ui_sequence]
            elapsed = time.monotonic() - start
            for sample in fresh:
                last_ui_sequence = sample["ui_sequence"]
                sample.update(stage=stage, stage_elapsed_s=round(elapsed, 6),
                              host_monotonic_s=time.monotonic())
                # Preserve the triggering sample before a guard can raise.
                samples.append(sample)
                stage_samples.append(sample)
                if last_sample_sequence is not None:
                    require((sample["sequence"] - last_sample_sequence) & 0xFFFFFFFF == 1,
                            "waveform sample sequence gap")
                last_sample_sequence = sample["sequence"]
                if baseline_dropped is None:
                    baseline_dropped = sample["dropped_samples"]
                require(sample["dropped_samples"] == baseline_dropped,
                        "ECU dropped waveform samples")
                sample_guard(sample, target, elapsed)
            time.sleep(0.01)
        require(len(stage_samples) >= duration * 700, "too few waveform samples")
        return stage_samples

    try:
        time.sleep(0.6)
        entry = status_guard(require_armed=False)
        report["entry"] = entry
        require(entry["status"]["control_mode"] == 0, "another controller is active")
        require(entry["diagnostic"]["applied_relay_mask"] == ui.SAFETY_RELAY_K12_RUN_PERMIT,
                "entry is not K12-only")
        require(all(entry["status"][name] == 0 for name in
                    ("valve_requested_target_ma", "valve_applied_target_ma",
                     "forward_duty_permille", "reverse_duty_permille")),
                "entry valve output not zero")
        require(max(entry["status"]["forward_current_ma"],
                    entry["status"]["reverse_current_ma"]) < 50,
                "entry current not near zero")
        client = ota.OtaClient("172.16.0.10", args.ecu_ip)
        try:
            report["ota"] = client.status()
        finally:
            client.close()
        require(report["ota"]["accepted_sequence"] ==
                targets.ACCEPTED_SEQUENCES[device.serial] and
                report["ota"]["state"] == 0 and report["ota"]["ota_result"] == 0,
                "reviewed confirmed release is required")
        before = bridge.config_request(ui.MSG_CONFIG_GET)
        require(before["result"] == 0 and
                (before["persisted_valid"] or before["using_defaults"]) and not before["dirty"],
                "PI configuration is neither clean persisted data nor clean defaults")
        report["config_before"] = before
        bridge.configure({"sender_id": 2, "priority": 2, "enabled": False})
        bridge.set_tuning_session(token, True)
        deadline = time.monotonic() + 1.0
        while bridge.snapshot()["latest_telemetry"] is None and time.monotonic() < deadline:
            time.sleep(0.02)
        require(bridge.snapshot()["latest_telemetry"] is not None, "no waveform baseline")
        with bridge.lock:
            last_ui_sequence = bridge.telemetry_generation
        bridge.clear_fault()  # Neutral-only CLEAR_FAULT and rearm.
        time.sleep(0.25)
        observe("neutral-arm", 0, 0.45)
        directions = [1, -1] if args.direction == "both" else [
            1 if args.direction == "forward" else -1]
        for magnitude in (100, 200):
            for sign in directions:
                target = magnitude * sign
                name = f"{'forward' if sign > 0 else 'reverse'}-{magnitude}mA"
                bridge.patch_control({"valve_current_target_ma": target})
                wave = observe(name, target, 0.85 if magnitude == 100 else 1.1)
                bridge.patch_control({"valve_current_target_ma": 0})
                field = "forward_current_ma" if sign > 0 else "reverse_current_ma"
                duty_field = "forward_duty_permille" if sign > 0 else "reverse_duty_permille"
                steady = [s for s in wave if s["stage_elapsed_s"] >= 0.45]
                require(len(steady) >= 200, "insufficient settling window")
                currents = [s[field] for s in steady]
                result = {"name": name, "target_ma": target,
                          "steady_feedback_min_mean_max_ma": [
                              min(currents), round(statistics.mean(currents), 2), max(currents)],
                          "steady_duty_min_mean_max_permille": [
                              min(s[duty_field] for s in steady),
                              round(statistics.mean(s[duty_field] for s in steady), 2),
                              max(s[duty_field] for s in steady)]}
                report["cases"].append(result)
                tolerance = max(25, magnitude * 0.20)
                require(all(abs(value - magnitude) <= tolerance for value in currents),
                        f"{name}: not continuously within +/-{tolerance} mA: {result}")
                print("PASS " + json.dumps(result), flush=True)
                observe(name + "-zero", 0, 0.55)
        if args.check_latency:
            report["latency_cases"] = []
            for target in (0, 200, -200):
                name = f"waveform-latency-{target}mA"
                bridge.patch_control({"valve_current_target_ma": target})
                with subprocess.Popen(
                    [str(Path(__file__).with_name("check_ecu_latency.sh"))],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                    env=dict(os.environ, ECU_IP=args.ecu_ip),
                ) as process:
                    wave = observe(name, target, 2.5)
                    output = process.communicate(timeout=2.0)[0]
                    bridge.patch_control({"valve_current_target_ma": 0})
                    print(output, flush=True)
                    require(process.returncode == 0, f"{name}: ping latency gate failed")
                tail = [s for s in wave if s["stage_elapsed_s"] >= 2.0]
                require(len(tail) >= 300, "insufficient final steady-state samples")
                field = "reverse_current_ma" if target < 0 else "forward_current_ma"
                duty = "reverse_duty_permille" if target < 0 else "forward_duty_permille"
                item = {"name": name, "ping_output": output,
                        "final_feedback_min_mean_max_ma": [
                            min(s[field] for s in tail),
                            round(statistics.mean(s[field] for s in tail), 2),
                            max(s[field] for s in tail)],
                        "final_duty_min_mean_max_permille": [
                            min(s[duty] for s in tail),
                            round(statistics.mean(s[duty] for s in tail), 2),
                            max(s[duty] for s in tail)]}
                report["latency_cases"].append(item)
                if target:
                    require(all(abs(s[field] - abs(target)) <= 40 for s in tail),
                            f"{name}: steady-state current outside +/-40 mA")
                print("PASS " + json.dumps({k: v for k, v in item.items()
                                           if k != "ping_output"}), flush=True)
                observe(name + "-zero", 0, 0.55)
        bridge.release_control()
        time.sleep(0.4)
        final = status_guard(require_armed=False)
        require(final["status"]["control_mode"] == 0 and
                final["diagnostic"]["applied_relay_mask"] == ui.SAFETY_RELAY_K12_RUN_PERMIT,
                "RELEASE did not restore K12-only")
        require(final["status"]["forward_duty_permille"] ==
                final["status"]["reverse_duty_permille"] == 0, "PWM did not stop")
        zero_verified = True
        after = bridge.config_request(ui.MSG_CONFIG_GET)
        report["config_after"] = after
        require(all(after[k] == before[k] for k in before if k != "request_sequence"),
                "PI configuration changed during test")
        report["passed"] = True
    except (GuardFailure, OSError, TimeoutError, KeyboardInterrupt) as error:
        bridge.release_control()
        report["error"] = str(error) or type(error).__name__
        print("STOP " + report["error"], flush=True)
    finally:
        bridge.release_control()
        bridge.set_tuning_session(token, False)
        time.sleep(0.6)
        report["final"] = bridge.snapshot()
        final_status = report["final"].get("status") or {}
        report["zero_verified"] = (
            report["final"].get("status_age_ms") is not None and
            report["final"]["status_age_ms"] < 300 and
            all(final_status.get(k) == 0 for k in
                ("valve_requested_target_ma", "valve_applied_target_ma",
                 "forward_duty_permille", "reverse_duty_permille")) and
            max(final_status.get("forward_current_ma", 9999),
                final_status.get("reverse_current_ma", 9999)) < 50)
        bridge.close()
        report["captured_samples"] = len(samples)
        with (args.output_dir / "report.json").open("x") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        with (args.output_dir / "waveform.csv").open("x", newline="") as stream:
            if samples:
                writer = csv.DictWriter(stream, fieldnames=list(samples[0]))
                writer.writeheader()
                writer.writerows(samples)
            stream.flush()
            os.fsync(stream.fileno())
        print(json.dumps({"passed": report["passed"],
                          "zero_verified": report["zero_verified"],
                          "captured_samples": len(samples),
                          "output_dir": str(args.output_dir)}), flush=True)
    return 0 if report["passed"] and zero_verified and report["zero_verified"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    targets.add_target_arguments(parser)
    parser.add_argument("--direction", choices=("both", "forward", "reverse"), default="both")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--check-latency", action="store_true",
                        help="also check ping during zero/+200/-200 mA waveforms")
    parser.add_argument("--accept-unloaded-actuation", action="store_true")
    args = parser.parse_args()
    if not args.accept_unloaded_actuation:
        parser.error("--accept-unloaded-actuation is required")
    try:
        args.ecu_ip = targets.resolve_target(args.device_serial, args.ecu_ip).ecu_ip
    except ValueError as error:
        parser.error(str(error))
    os.umask(0o077)
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
