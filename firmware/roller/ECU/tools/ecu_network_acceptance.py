#!/usr/bin/env python3
"""Unloaded 1.0.20 bench: real .9/.10/.12 arbitration and .13 rejection.

Requires these temporary addresses on the isolated bench NIC. Sends no valve
current or steering request. Headlamp relay commands identify source binding.
"""

import argparse
import json
import socket
import struct
import time

import ecu_bench_test as bench_module
import ethernet_ota as ota

u = bench_module.UI
K12 = u.SAFETY_RELAY_K12_RUN_PERMIT


class Host:
    def __init__(self, suffix, sender, priority):
        self.ip = f"172.16.0.{suffix}"
        self.sender = sender
        self.priority = priority
        self.sequence = int(time.time() * 1000) & 0xFFFFFFFF
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind((self.ip, 0))

    def send(self, ecu, *, flags=0, emergency=False, patch=None):
        self.sequence = (self.sequence + 1) & 0xFFFFFFFF
        control = u.neutral_control()
        control.update(patch or {})
        if emergency:
            control = u.neutral_control()
            control["emergency_stop_request"] = 1
        payload = u.build_control_payload(control, self.sender,
                                         255 if emergency else self.priority)
        self.socket.sendto(u.encode_v2(u.MSG_CONTROL, flags, self.sequence, payload),
                           (ecu, u.CONTROL_PORT))

    def close(self):
        self.socket.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ecu-ip", default="172.16.0.11")
    parser.add_argument("--accept-isolated-unloaded-bench", action="store_true")
    args = parser.parse_args()
    if not args.accept_isolated_unloaded_bench:
        parser.error("--accept-isolated-unloaded-bench is required")
    # Bind all source addresses before any control mutation.
    remote = Host(9, 1, 20)
    service = Host(10, 2, 2)
    domain = Host(12, 3, 10)
    stranger = Host(13, 2, 200)
    duplicate = Host(9, 3, 200)
    hosts = (remote, service, domain, stranger, duplicate)
    b = bench_module.Bench(args.ecu_ip)
    passed = []

    def check(condition, message):
        if not condition:
            raise bench_module.BenchFailure(message)

    def guard():
        if b.status is None or b.diagnostic is None:
            return
        check(time.monotonic() - b.status_received_at < .30 and
              time.monotonic() - b.diagnostic_received_at < .30, "stale feedback")
        check(not (b.diagnostic["safety_status"] & u.SAFETY_STATUS_PHYSICAL_ESTOP),
              "physical E-stop changed")
        check(bool(b.diagnostic["safety_status"] & (1 << 22)), "identity not authenticated")
        check(not b.diagnostic["valve_fault_flags"], "valve fault")
        check(all(b.status[k] == 0 for k in
                  ("valve_requested_target_ma", "valve_applied_target_ma",
                   "forward_duty_permille", "reverse_duty_permille")),
              "unexpected valve output")
        if b.steering_status:
            check(not b.steering_status["command_enable"] and
                  not b.steering_status["speed_command_permille"],
                  "unexpected steering output")

    def cycle(commands=(), duration=.45):
        end = time.monotonic() + duration
        next_tx = 0
        while time.monotonic() < end:
            now = time.monotonic()
            if now >= next_tx:
                for host, options in commands:
                    host.send(args.ecu_ip, **options)
                next_tx = now + .04
            b.receive(.01)
            guard()

    def record(name, predicate):
        check(predicate(), name)
        passed.append(name)
        print("PASS " + name, flush=True)

    def baseline():
        return b.status["control_mode"] == 0 and b.diagnostic["applied_relay_mask"] == K12

    def latched():
        return (bool(b.diagnostic["safety_status"] & u.SAFETY_STATUS_NETWORK_ESTOP)
                and b.diagnostic["applied_relay_mask"] == 0)

    def release_all():
        for host in hosts:
            host.send(args.ecu_ip, flags=u.CONTROL_FLAG_RELEASE)
        cycle(duration=.45)

    def reset_latch():
        # No automatic reset in finally: preserve an E-stop if the test fails.
        cycle(duration=.40)
        service.send(args.ecu_ip)
        cycle(duration=.10)
        service.send(args.ecu_ip, flags=u.CONTROL_FLAG_ESTOP_RESET)
        cycle(duration=.40)
        check(not latched() and baseline(), "explicit reset did not restore K12-only")

    def service_access(host, allowed):
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.bind((host.ip, 0))
        probe.settimeout(.35)
        try:
            requests = [(u.MSG_CONFIG_GET, b"", u.TUNING_PORT)]
            if not allowed:
                # Invalid config cannot mutate RAM even if the IP gate is broken.
                requests += [(u.MSG_CONFIG_APPLY, bytes(48), u.TUNING_PORT),
                             (u.MSG_TELEMETRY_SUBSCRIBE,
                              struct.pack("<HHI", u.TELEMETRY_PORT, 1000, 1000),
                              u.TUNING_PORT)]
            for msg, payload, port in requests:
                probe.sendto(u.encode_v2(msg, 0, 123, payload), (args.ecu_ip, port))
                try:
                    data, source = probe.recvfrom(2048)
                except socket.timeout:
                    check(not allowed, f"{host.ip} allowed service timed out")
                else:
                    check(allowed and source == (args.ecu_ip, port),
                          f"{host.ip} unexpectedly received service {msg:#x}")
                    decoded = u.decode_v2(data)
                    check(decoded is not None, "invalid service reply")
                    config = u.unpack_config_reply(decoded[1])
                    check(config["result"] == 0, "allowed GET rejected")
            client = ota.OtaClient(host.ip, args.ecu_ip, timeout=.35, retries=1)
            try:
                try:
                    reply = client.status()
                except TimeoutError:
                    check(not allowed, f"{host.ip} allowed OTA status timed out")
                else:
                    check(allowed and reply["accepted_sequence"] == 20,
                          f"{host.ip} unauthorized OTA reply")
            finally:
                client.close()
        finally:
            probe.close()
        cycle(duration=.15)

    try:
        b.wait(1.0)
        check(b.status is not None and b.diagnostic is not None, "missing initial status")
        guard()
        check(baseline(), "test entry is not IDLE/K12-only")
        cycle([(service, {})])
        record(".10 neutral authority", lambda: b.status["active_sender_id"] == 2)
        cycle([(service, {}), (domain, {})])
        record(".12 priority 10 preempts .10 priority 2",
               lambda: b.status["active_sender_id"] == 3)
        cycle([(service, {}), (domain, {}), (remote, {})])
        record(".9 priority 20 preempts .12 priority 10",
               lambda: b.status["active_sender_id"] == 1)
        remote.priority = service.priority = domain.priority = 10
        cycle([(service, {}), (domain, {}), (remote, {})])
        record("equal priority chooses smaller sender ID",
               lambda: b.status["active_sender_id"] == 1)
        cycle([(service, {}), (domain, {})], duration=.65)
        record("remote timeout falls back to sender 2",
               lambda: b.status["active_sender_id"] == 2)
        release_all()
        record("all RELEASE restores K12-only", baseline)
        rejected = b.diagnostic["rejected_control_frames"]
        cycle([(stranger, {})])
        record(".13 ordinary control is rejected",
               lambda: baseline() and b.diagnostic["rejected_control_frames"] > rejected)
        release_all()
        cycle([(domain, {})])
        cycle([(domain, {"patch": {"headlamp_front_on": 1}})])
        rejected = b.diagnostic["rejected_control_frames"]
        cycle([(domain, {"patch": {"headlamp_front_on": 1}}),
               (duplicate, {"patch": {"headlamp_rear_on": 1}})])
        record("different IP cannot overwrite live sender 3 binding",
               lambda: b.status["headlamp_front_on"] == 1 and
               b.status["headlamp_rear_on"] == 0 and
               b.diagnostic["rejected_control_frames"] > rejected)
        release_all()
        domain.priority, remote.priority = 30, 5
        cycle([(domain, {}), (remote, {})])
        record("autonomous .12 owns ordinary control",
               lambda: b.status["active_sender_id"] == 3)
        cycle([(domain, {}), (remote, {"emergency": True})])
        record("lower ordinary-priority remote E-stop overrides autonomous", latched)
        cycle([(domain, {})], duration=.55)
        record("remote disconnect and continuing autonomous neutral retain latch", latched)
        remote.send(args.ecu_ip, flags=u.CONTROL_FLAG_RELEASE)
        service.send(args.ecu_ip, flags=u.CONTROL_FLAG_CLEAR_FAULT)
        cycle(duration=.45)
        record("RELEASE and CLEAR_FAULT do not reset E-stop", latched)
        reset_latch()
        record("explicit .10 RESET restores only K12", baseline)
        cycle([(domain, {"emergency": True}), (remote, {"emergency": True})])
        remote.send(args.ecu_ip, flags=u.CONTROL_FLAG_ESTOP_RESET)
        cycle([(domain, {"emergency": True})], duration=.15)
        record("another live emergency sender blocks RESET", latched)
        release_all()
        record("releasing both emergency senders still retains latch", latched)
        reset_latch()
        cycle([(stranger, {"emergency": True})])
        record("valid emergency remains dominant even from non-whitelisted .13", latched)
        stranger.send(args.ecu_ip, flags=u.CONTROL_FLAG_ESTOP_RESET)
        cycle(duration=.2)
        record("non-whitelisted .13 cannot reset emergency", latched)
        reset_latch()
        for host, allowed in ((service, True), (remote, False),
                              (domain, False), (stranger, False)):
            service_access(host, allowed)
            record(f"{host.ip} service IP gate allowed={allowed}", baseline)
        record("final IDLE/K12-only, zero valves, zero steering", baseline)
        print(json.dumps({"passed": len(passed), "cases": passed,
                          "final_status": b.status, "final_diagnostic": b.diagnostic},
                         ensure_ascii=False, indent=2), flush=True)
        return 0
    except (bench_module.BenchFailure, OSError) as error:
        print(json.dumps({"error": str(error), "passed": passed,
                          "status": b.status, "diagnostic": b.diagnostic},
                         ensure_ascii=False, indent=2), flush=True)
        return 1
    finally:
        # Never reset a latch automatically after failure.
        for host in hosts:
            try:
                host.send(args.ecu_ip, flags=u.CONTROL_FLAG_RELEASE)
            except OSError:
                pass
            host.close()
        b.close()


if __name__ == "__main__":
    raise SystemExit(main())
