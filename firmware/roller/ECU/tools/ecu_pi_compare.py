#!/usr/bin/env python3
"""Compare one volatile PI candidate on SN-EJAHGJI; always reload saved config.

No firmware, PWM frequency, slew, calibration, duty ceiling, or Flash writes.
Only Kp/Ki are changed in RAM. This is an unloaded 100/200/500 mA bench test.
"""

import argparse
import copy
import csv
import json
import os
from pathlib import Path
import signal
import statistics
import time

import ecu_debug_ui as u
from ecu_valve_guarded_test import GuardFailure, require


def metrics(wave, target):
    rows = [s for s in wave if s["requested_target_ma"] == target]
    require(len(rows) >= 1000, "too few samples at target")
    key = "forward_current_ma" if target > 0 else "reverse_current_ma"
    duty = "forward_duty_permille" if target > 0 else "reverse_duty_permille"
    t0 = rows[0]["timestamp_us"]
    times = [((s["timestamp_us"] - t0) & 0xFFFFFFFF) / 1000 for s in rows]
    currents = [s[key] for s in rows]
    magnitude = abs(target)
    # Average over 20 ms to separate loop response from isolated ADC noise.
    filtered = [statistics.mean(currents[max(0, i-19):i+1])
                for i in range(len(currents))]
    tolerance = max(5, magnitude * .05)
    outside = [i for i, value in enumerate(filtered) if abs(value-magnitude) > tolerance]
    settled = (0.0 if not outside else
               times[outside[-1]+1] if outside[-1]+1 < len(rows) else None)
    ramp = next((t for t, s in zip(times, rows)
                 if abs(s["applied_target_ma"]) == magnitude), None)
    tail = currents[-400:]
    return {
        "target_ma": target, "samples": len(rows),
        "request_ramp_ms": ramp,
        "first_90pct_ms": next((t for t, v in zip(times, filtered)
                               if v >= magnitude*.9), None),
        "settled_5pct_20ms_mean_ms": settled,
        "raw_peak_ma": max(currents),
        "overshoot_20ms_mean_percent": round(max(0, max(filtered)-magnitude)/magnitude*100, 2),
        "tail_feedback_min_mean_max_ma": [
            min(tail), round(statistics.mean(tail), 2), max(tail)],
        "tail_stddev_ma": round(statistics.pstdev(tail), 2),
        "tail_duty_mean_permille": round(statistics.mean(s[duty] for s in rows[-400:]), 2),
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--kp", type=int, required=True)
    p.add_argument("--ki", type=int, required=True)
    p.add_argument("--output-dir", type=Path, required=True)
    p.add_argument("--dynamic", action="store_true",
                   help="also test nonzero 100->200->500->200->100 mA transitions")
    p.add_argument("--accept-unloaded-actuation", action="store_true")
    args = p.parse_args()
    if not args.accept_unloaded_actuation:
        p.error("--accept-unloaded-actuation required")
    if not (0 <= args.kp <= 1000 and 0 <= args.ki <= 12000):
        p.error("candidate exceeds this experiment's bounded gain range")
    os.umask(0o077)
    args.output_dir.mkdir(mode=0o700, exist_ok=False)
    def interrupted(*_):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    b = u.BenchBridge("172.16.0.11")
    b.start()
    token = f"pi-compare-{time.time_ns()}"
    result = {"kp": args.kp, "ki": args.ki, "passed": False,
              "flash_written": False, "cases": []}
    all_samples = []
    last_ui = 0
    last_sequence = None
    dropped = None
    changed = False
    before = None

    def save_json(name, data):
        with (args.output_dir / name).open("x") as f:
            json.dump(data, f, indent=2)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())

    def safe(armed=True):
        s = b.snapshot()
        for name in ("status", "diagnostic", "security", "steering"):
            age = s[name+"_age_ms"]
            require(age is not None and age < 300, f"stale {name}")
        require(not s["physical_estop_active"] and not s["network_estop_latched"] and
                not s["critical_inhibit"], "safety inhibit")
        require(s["security"]["mcu_uid"] == "003800613434511232383537" and
                s["security"]["serial"] == "0123d47eb2ee0e9bee" and
                s["security"]["auth_result"] == 0, "identity mismatch")
        require(not s["diagnostic"]["valve_fault_flags"], "ECU valve fault")
        require(not s["steering_status"]["command_enable"] and
                not s["steering_status"]["speed_command_permille"], "steering active")
        if armed:
            require(s["status"]["active_sender_id"] == 2 and
                    bool(s["diagnostic"]["safety_status"] & 8), "authority lost")
        return s

    def observe(stage, target, previous, duration):
        nonlocal last_ui, last_sequence, dropped
        start = time.monotonic()
        wave = []
        while time.monotonic()-start < duration:
            b.heartbeat()
            b.set_tuning_session(token, True)
            s = safe()
            require(s["telemetry_age_ms"] is not None and s["telemetry_age_ms"] < 100,
                    "waveform stale")
            with b.lock:
                fresh = [dict(row) for row in b.telemetry_history if row["ui_sequence"] > last_ui]
            elapsed = time.monotonic()-start
            for row in fresh:
                last_ui = row["ui_sequence"]
                row["stage"] = stage
                all_samples.append(row)
                wave.append(row)
                if last_sequence is not None:
                    require((row["sequence"]-last_sequence)&0xFFFFFFFF == 1, "sample gap")
                last_sequence = row["sequence"]
                if dropped is None:
                    dropped = row["dropped_samples"]
                require(row["dropped_samples"] == dropped and not row["fault_flags"],
                        "sample drop or fault")
                fwd, rev = row["forward_current_ma"], row["reverse_current_ma"]
                fd, rd = row["forward_duty_permille"], row["reverse_duty_permille"]
                limit = max(200, max(abs(target), abs(previous))*1.35+50)
                require(max(fwd, rev) <= limit and max(fd, rd) <= 350 and not (fd and rd),
                        f"host envelope: I={fwd}/{rev}, PWM={fd}/{rd}")
                if target and elapsed > .35:
                    active = fwd if target > 0 else rev
                    require(row["requested_target_ma"] == target and active >= 20,
                            f"no response: target={target}, feedback={active}")
                    require((rd if target > 0 else fd) == 0, "wrong PWM direction")
                if target == 0 and elapsed > .40:
                    require(fd == rd == 0 and max(fwd, rev) < 50, "failed to stop")
            time.sleep(.01)
        return wave

    try:
        time.sleep(.6)
        s = safe(False)
        require(s["status"]["control_mode"] == 0 and
                s["diagnostic"]["applied_relay_mask"] == u.SAFETY_RELAY_K12_RUN_PERMIT,
                "entry not IDLE/K12-only")
        require(s["status"]["forward_duty_permille"] == s["status"]["reverse_duty_permille"] == 0
                and max(s["status"]["forward_current_ma"], s["status"]["reverse_current_ma"]) < 50,
                "entry current not zero")
        before = b.config_request(u.MSG_CONFIG_GET)
        require(before["result"] == 0 and before["persisted_valid"] and not before["dirty"],
                "clean persisted config required")
        save_json("before.json", before)  # Durable backup before RAM mutation.
        candidate = copy.deepcopy(before["config"])
        for channel in ("forward", "reverse"):
            candidate[channel]["kp_permille_per_amp"] = args.kp
            candidate[channel]["ki_permille_per_amp_second"] = args.ki
        result["candidate"] = candidate
        if candidate != before["config"]:
            changed = True  # An ambiguous acknowledgement also requires reload.
            reply = b.config_request(u.MSG_CONFIG_APPLY, u.pack_valve_config(candidate))
            require(reply["result"] == 0 and reply["config"] == candidate, "APPLY failed")
        b.configure({"sender_id": 2, "priority": 2, "enabled": False})
        b.set_tuning_session(token, True)
        time.sleep(.35)
        require(b.snapshot()["latest_telemetry"] is not None, "missing waveform")
        with b.lock:
            last_ui = b.telemetry_generation
        b.clear_fault()
        time.sleep(.25)
        observe("neutral", 0, 0, .45)
        sequences = ([[sign*x for x in (100, 200, 500, 200, 100)]
                      for sign in (1, -1)] if args.dynamic else
                     [[sign*magnitude] for magnitude in (100, 200, 500)
                      for sign in (1, -1)])
        for sequence in sequences:
            previous = 0
            for target in sequence:
                magnitude = abs(target)
                b.patch_control({"valve_current_target_ma": target})
                wave = observe(f"{previous}-to-{target}", target, previous, 1.8)
                item = metrics(wave, target)
                item["previous_target_ma"] = previous
                result["cases"].append(item)
                require(item["settled_5pct_20ms_mean_ms"] is not None,
                        "did not settle within observation")
                require((abs(previous) > magnitude) or
                        item["overshoot_20ms_mean_percent"] <= 10,
                        f"excess overshoot: {item}")
                require(abs(item["tail_feedback_min_mean_max_ma"][1]-magnitude) <= max(5,.03*magnitude),
                        f"steady error: {item}")
                print(json.dumps(item), flush=True)
                previous = target
            b.patch_control({"valve_current_target_ma": 0})
            observe(str(previous)+"-zero", 0, previous, .7)
        result["passed"] = True
    except (GuardFailure, OSError, TimeoutError, KeyboardInterrupt) as error:
        result["error"] = str(error) or type(error).__name__
        b.release_control()
        print("STOP "+result["error"], flush=True)
    finally:
        b.release_control()
        b.set_tuning_session(token, False)
        time.sleep(.7)
        final = b.snapshot()
        status = final.get("status") or {}
        result["zero_verified"] = (
            final.get("status_age_ms") is not None and final["status_age_ms"] < 300 and
            all(status.get(k) == 0 for k in ("valve_requested_target_ma", "valve_applied_target_ma",
                                            "forward_duty_permille", "reverse_duty_permille")) and
            max(status.get("forward_current_ma", 9999), status.get("reverse_current_ma", 9999)) < 50)
        if changed and result["zero_verified"]:
            try:
                reloaded = b.config_request(u.MSG_CONFIG_RELOAD)
                result["reload"] = reloaded
                result["original_restored"] = (
                    reloaded["result"] == 0 and reloaded["config"] == before["config"] and
                    not reloaded["dirty"] and
                    reloaded["persisted_generation"] == before["persisted_generation"])
            except (OSError, TimeoutError) as error:
                result["reload_error"] = str(error)
                result["original_restored"] = False
        else:
            result["original_restored"] = not changed
        result["final"] = b.snapshot()
        b.close()
        save_json("report.json", result)
        with (args.output_dir / "waveform.csv").open("x", newline="") as f:
            if all_samples:
                writer = csv.DictWriter(f, fieldnames=list(all_samples[0]))
                writer.writeheader()
                writer.writerows(all_samples)
            f.flush()
            os.fsync(f.fileno())
        print(json.dumps({k: result[k] for k in
                          ("passed", "flash_written", "zero_verified", "original_restored")}), flush=True)
    return 0 if result["passed"] and result["zero_verified"] and result["original_restored"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
