"""Offline checks for fixed serial/IP selection and pre-actuation identity gates."""
import argparse
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import ecu_bench_targets as targets
from check_device_profile import read_profile
from ecu_readonly_snapshot import IPS


class BenchTargetTests(unittest.TestCase):
    @staticmethod
    def state(serial):
        target = targets.TARGETS[serial]
        identity = targets.REVIEWED_IDENTITIES[serial]
        return dict(device_serial=serial, ecu_ip=target.ecu_ip,
                    security=dict(mcu_uid="".join(f"{x:08x}" for x in identity.mcu_uid),
                                  serial=identity.serial.hex(), config_crc32c=identity.config_crc32c,
                                  revision="00006005", config_locked=1, data_locked=1,
                                  atecc_result=0, auth_result=0, pairing_generation=1),
                    diagnostic=dict(safety_status=1 << 22, requested_relay_mask=1 << 22,
                                    applied_relay_mask=1 << 22, valve_fault_flags=0),
                    status=dict(control_mode=0, forward_duty_permille=0, reverse_duty_permille=0,
                                valve_requested_target_ma=0, valve_applied_target_ma=0,
                                forward_current_ma=1, reverse_current_ma=1),
                    steering_status=dict(command_enable=0, speed_command_permille=0),
                    ota=dict(result=0, ota_result=0, state=0))

    def test_fixed_mapping_matches_actual_firmware_and_readonly_capture(self):
        for device_id in (1, 2):
            firmware = read_profile(device_id)
            target = targets.resolve_target(firmware["serial"])
            self.assertEqual((target.ecu_ip, target.domain_ip),
                             (firmware["ecu_ip"], firmware["domain_ip"]))
            self.assertEqual(IPS[target.serial], target.ecu_ip)
        self.assertEqual(targets.ACCEPTED_SEQUENCES,
                         {"SN-EJAHGJI": 20, "SN-EJAHGJQ": 22})

    def test_legacy_default_is_not_changed_by_current_project_selection(self):
        parser = argparse.ArgumentParser()
        targets.add_target_arguments(parser)
        args = parser.parse_args([])
        self.assertEqual(targets.resolve_target(args.device_serial, args.ecu_ip).ecu_ip, "172.16.0.11")
        args = parser.parse_args(["--device-serial", "SN-EJAHGJQ"])
        self.assertEqual(targets.resolve_target(args.device_serial, args.ecu_ip).ecu_ip, "172.16.0.21")

    def test_unknown_serial_or_mismatched_explicit_ip_rejected(self):
        for serial, ip in (("unknown", None), ("SN-EJAHGJI", "172.16.0.21"),
                           ("SN-EJAHGJQ", "172.16.0.11"), ("SN-EJAHGJQ", "172.16.0.22")):
            with self.subTest(serial=serial, ip=ip), self.assertRaises(ValueError):
                targets.resolve_target(serial, ip)

    def test_each_authenticated_idle_target_and_cross_board_rejection(self):
        for serial, target in targets.TARGETS.items():
            state = self.state(serial)
            targets.require_passive_entry(state, target)
            other = next(v for k, v in targets.TARGETS.items() if k != serial)
            with self.assertRaises(RuntimeError):
                targets.require_passive_entry(state, other)

    def test_missing_or_changed_security_field_rejected(self):
        target = targets.TARGETS["SN-EJAHGJQ"]
        security = self.state(target.serial)["security"]
        for name in security:
            other = dict(security)
            del other[name]
            self.assertFalse(targets.security_matches(other, target), name)
            other = dict(security)
            other[name] = str(other[name]) + "bad"
            self.assertFalse(targets.security_matches(other, target), name)
        self.assertFalse(targets.security_matches(None, target))

    def test_actuation_staging_estop_or_incomplete_state_rejected(self):
        target = targets.TARGETS["SN-EJAHGJQ"]
        state = self.state(target.serial)
        changes = [("diagnostic", "safety_status", (1 << 22) | (1 << bit))
                   for bit in (1, 23, 24, 25, 28)] + [
            ("diagnostic", "safety_status", 0), ("diagnostic", "applied_relay_mask", 0),
            ("diagnostic", "requested_relay_mask", (1 << 22) | 1),
            ("diagnostic", "valve_fault_flags", 1), ("status", "control_mode", 2),
            ("status", "valve_requested_target_ma", 100), ("status", "forward_duty_permille", 1),
            ("status", "reverse_current_ma", 50), ("status", "forward_current_ma", -1),
            ("steering_status", "command_enable", 1), ("ota", "ota_result", -19),
            ("ota", "state", 1)]
        for group, name, value in changes:
            other = copy.deepcopy(state)
            other[group][name] = value
            with self.subTest(group=group, name=name, value=value), self.assertRaises(RuntimeError):
                targets.require_passive_entry(other, target)
        with self.assertRaises(RuntimeError):
            targets.require_passive_entry({}, target)

    def test_passive_preflight_precedes_actuator_clients_in_all_three_tools(self):
        tools = Path(__file__).resolve().parents[1] / "tools"
        for name, preflight, constructor in (
                ("ecu_bench_test.py", "targets.passive_preflight(device)", "bench = Bench"),
                ("ecu_pi_compare.py", "targets.passive_preflight(device)", "b = u.BenchBridge"),
                ("ecu_valve_guarded_test.py", "targets.passive_preflight(device)",
                 "bridge = ui.BenchBridge"),
                ("ecu_network_acceptance.py", "targets.passive_preflight(target)",
                 "remote = Host")):
            source = (tools / name).read_text()
            self.assertLess(source.index(preflight), source.index(constructor))
        source = (tools / "ecu_network_acceptance.py").read_text()
        self.assertIn('domain = Host(int(target.domain_ip.rsplit', source)
        self.assertIn('targets.ACCEPTED_SEQUENCES[target.serial]', source)
        self.assertIn('output already exists; evidence is never overwritten', source)
        self.assertNotIn("domain = Host(12", source)

    def test_new_board_valve_latency_targets_selected_ecu(self):
        source = (Path(__file__).resolve().parents[1] / "tools" /
                  "ecu_valve_guarded_test.py").read_text()
        self.assertIn("env=dict(os.environ, ECU_IP=args.ecu_ip)", source)
        self.assertIn("targets.ACCEPTED_SEQUENCES[device.serial]", source)
        self.assertIn('targets.security_matches(snap["security"], device)', source)
        self.assertNotIn('targets.security_matches(snap["security"], target)', source)
        self.assertNotIn('accepted_sequence"] == 20', source)

    def test_pi_candidate_cannot_shadow_selected_device_identity(self):
        source = (Path(__file__).resolve().parents[1] / "tools" /
                  "ecu_pi_compare.py").read_text()
        self.assertIn('targets.security_matches(s["security"], device)', source)
        self.assertNotIn('targets.security_matches(s["security"], target)', source)


if __name__ == "__main__":
    unittest.main()
