"""Offline consistency/rejection checks, without target or private-key access."""
import copy
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import verify_device_security_preflight as preflight

ROOT = preflight.m.PROJECT / "artifacts/device-backups/SN-EJAHGJQ/stlink-preflight-20260906"
INPUTS = ROOT.parent / "roller-ecu-SN-EJAHGJQ-recovery-inputs-1.0.22-20260906"


class PreflightStaticTests(unittest.TestCase):
    def test_offline_only_no_mutation_or_key_access(self):
        source = Path(preflight.__file__).read_text()
        for call in ("m.programmer(", "m.capture(", "subprocess.run(", "OtaClient(",
                     "m.verify_pki_backup(", "m.verify_assets("):
            self.assertNotIn(call, source)

    def test_reset_command_cannot_fallback_to_software(self):
        self.assertEqual(preflight.PULSE_COMMAND[1:], ["-c", "port=SWD",
                         "sn=066BFF565456857187210935", "mode=HWRSTPULSE"])


@unittest.skipUnless((ROOT / "physical-nrst-PASS.json").is_file(), "optional NRST archive unavailable")
class PulseEvidenceTests(unittest.TestCase):
    def setUp(self):
        read = preflight.recovery.read_json
        self.before = read(ROOT / "runtime-before-nrst.json")
        self.after = read(ROOT / "runtime-after-nrst.json")
        self.intent = read(ROOT / "physical-nrst-intent.json")
        self.result = read(ROOT / "physical-nrst-PASS.json")
        self.log = (ROOT / "04-physical-nrst.log").read_text()

    def verify(self):
        return preflight.verify_pulse(self.before, self.after, self.intent, self.result, self.log)

    def test_actual_hardware_pulse_evidence_passes(self):
        self.assertEqual(self.verify()["after_uptime_ms"], 4733)

    def test_wrong_target_or_reset_kind_is_rejected(self):
        for change in ("target", "intent", "fallback", "receipt"):
            self.setUp()
            if change == "target":
                self.after["security"]["mcu_uid"] = "003800613434511232383537"
            elif change == "intent":
                self.intent["command"][-1] = "mode=UR"
            elif change == "fallback":
                self.result["software_reset_fallback"] = True
            else:
                self.log = self.log.replace("hwRstPulse", "Under Reset")
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                self.verify()

    def test_stale_uptime_and_fabricated_summary_fail(self):
        for change in ("no_reset", "timestamp", "summary", "nan", "infinite"):
            self.setUp()
            if change == "no_reset":
                self.after["diagnostic"]["secure_uptime_ms"] = 74000
                self.result["after_uptime_ms"] = 74000
            elif change == "timestamp":
                self.after["captured_utc"] = self.before["captured_utc"]
            elif change == "summary":
                self.result["after_uptime_ms"] = 1
            elif change == "nan":
                self.result["elapsed_s"] = float("nan")
            else:
                self.result["elapsed_s"] = float("inf")
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                self.verify()

    def test_unsafe_or_busy_runtime_is_rejected(self):
        for group, key, value in (("diagnostic", "applied_relay_mask", 1),
                                   ("diagnostic", "valid_control_frames", 1),
                                   ("diagnostic", "telemetry_frames_sent", 1),
                                   ("status", "forward_duty_permille", 1),
                                   ("security", "flags", 0x6F),
                                   ("valve_config", "persisted_generation", 1),
                                   ("ota", "secure_received", 512),
                                   ("ota", "ota_result", -1)):
            state = copy.deepcopy(self.after)
            state[group][key] = value
            with self.subTest(group=group, key=key), self.assertRaises(RuntimeError):
                preflight.require_safe(state)


@unittest.skipUnless((ROOT / "stability-PASS.json").is_file() and INPUTS.is_dir(),
                     "optional complete preflight archive unavailable")
class PreflightArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ecu-open-preflight-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "evidence"
        shutil.copytree(ROOT, self.root)

    def verify(self):
        return preflight.verify(self.root, INPUTS)

    def test_real_archive_does_not_authorize_closed_or_touch_target(self):
        with patch.object(preflight.m, "programmer") as programmer, \
                patch.object(preflight.m, "capture") as capture, \
                patch.object(preflight.m, "verify_pki_backup") as keys:
            report = self.verify()
            self.assertEqual(report["result"], "PASS")
            self.assertFalse(report["readiness"]["lifecycle_write_authorized"])
            self.assertFalse(report["readiness"]["hardware_da_tested"])
            self.assertFalse(report["readiness"]["hardware_full_regression_tested"])
            programmer.assert_not_called()
            capture.assert_not_called()
            keys.assert_not_called()

    def test_changed_full_flash_or_pairing_is_rejected(self):
        path = self.root / "full-flash.bin"
        data = bytearray(path.read_bytes())
        data[0xE0000] ^= 1
        path.write_bytes(data)
        with self.assertRaisesRegex(RuntimeError, "exact reviewed"):
            self.verify()

    def test_unsafe_option_is_rejected(self):
        path = self.root / "01-fresh-open-identity.log"
        path.write_text(path.read_text().replace("LOCKBL       : 0x0", "LOCKBL       : 0x1"))
        with self.assertRaisesRegex(RuntimeError, "bootloader lock"):
            self.verify()

    def test_reboot_in_stability_window_is_rejected(self):
        path = self.root / "stability-06.json"
        state = json.loads(path.read_text())
        state["diagnostic"]["secure_uptime_ms"] = 4333
        path.write_text(json.dumps(state))
        report_path = self.root / "stability-PASS.json"
        report = json.loads(report_path.read_text())
        report["samples"][6]["uptime_ms"] = 4333
        report_path.write_text(json.dumps(report))
        with self.assertRaisesRegex(RuntimeError, "unexpected reboot"):
            self.verify()

    def test_stale_stability_timestamp_is_rejected(self):
        path = self.root / "stability-06.json"
        state = json.loads(path.read_text())
        state["captured_utc"] = "2026-09-05T00:00:00Z"
        path.write_text(json.dumps(state))
        with self.assertRaisesRegex(RuntimeError, "inconsistent observation timestamp"):
            self.verify()

    def test_symlink_evidence_is_rejected(self):
        (self.root / "unexpected-link").symlink_to(ROOT / "full-flash.bin")
        with self.assertRaisesRegex(RuntimeError, "symlink"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
