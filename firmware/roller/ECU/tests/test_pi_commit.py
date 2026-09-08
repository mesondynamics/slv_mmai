"""Offline gates for the one-write PI candidate commit tool."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import ecu_bench_targets as targets
import ecu_pi_commit as commit


class PiCommitTests(unittest.TestCase):
    def candidate(self):
        channel = dict(kp_permille_per_amp=400, ki_permille_per_amp_second=8000)
        config = dict(forward=copy.deepcopy(channel), reverse=copy.deepcopy(channel))
        values = [100, 200, 500, 200, 100, -100, -200, -500, -200, -100]
        cases = [dict(target_ma=value, settled_5pct_20ms_mean_ms=300,
                      tail_feedback_min_mean_max_ma=[abs(value)-2, abs(value), abs(value)+2])
                 for value in values]
        return dict(passed=True, zero_verified=True, flash_written=False,
                    device_serial="SN-EJAHGJQ", ecu_ip="172.16.0.21",
                    kp=400, ki=8000, candidate=config, cases=cases)

    def test_complete_candidate_is_accepted_offline(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "candidate.json"
            path.write_text(json.dumps(self.candidate()))
            report, config = commit.read_candidate(
                path, targets.TARGETS["SN-EJAHGJQ"], 400, 8000)
            self.assertTrue(report["passed"])
            self.assertEqual(config["forward"]["ki_permille_per_amp_second"], 8000)

    def test_wrong_board_gain_failure_or_incomplete_sweep_is_rejected(self):
        for patch in (lambda x: x.update(device_serial="SN-EJAHGJI"),
                      lambda x: x.update(ki=6000),
                      lambda x: x.update(passed=False),
                      lambda x: x["cases"].pop(),
                      lambda x: x["candidate"]["reverse"].update(
                          ki_permille_per_amp_second=4000)):
            with self.subTest(patch=patch), tempfile.TemporaryDirectory() as directory:
                value = self.candidate()
                patch(value)
                path = Path(directory) / "candidate.json"
                path.write_text(json.dumps(value))
                with self.assertRaises(RuntimeError):
                    commit.read_candidate(path, targets.TARGETS["SN-EJAHGJQ"], 400, 8000)

    def test_tool_has_one_save_and_no_actuator_command(self):
        source = Path(commit.__file__).read_text()
        self.assertEqual(source.count("config_request(ui.MSG_CONFIG_SAVE)"), 1)
        self.assertNotIn("patch_control", source)
        self.assertNotIn("clear_fault", source)
        self.assertIn("--accept-parameter-flash-write", source)


if __name__ == "__main__":
    unittest.main()
