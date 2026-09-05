"""Offline tests: no sockets, hardware actuation or parameter writes."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from ecu_pi_compare import metrics, GuardFailure


class PiMetricsTests(unittest.TestCase):
    def wave(self, target=200, start_us=0):
        return [{"requested_target_ma": target, "applied_target_ma": target,
                 "timestamp_us": (start_us + i * 1000) & 0xFFFFFFFF,
                 "forward_current_ma": abs(target) if target > 0 else 0,
                 "reverse_current_ma": abs(target) if target < 0 else 0,
                 "forward_duty_permille": 105 if target > 0 else 0,
                 "reverse_duty_permille": 106 if target < 0 else 0}
                for i in range(1200)]

    def test_already_settled_is_zero_not_failure(self):
        result = metrics(self.wave(), 200)
        self.assertEqual(result["settled_5pct_20ms_mean_ms"], 0)
        self.assertEqual(result["tail_feedback_min_mean_max_ma"], [200, 200, 200])
        self.assertEqual(result["tail_duty_mean_permille"], 105)

    def test_reverse_channel_selected(self):
        result = metrics(self.wave(-200), -200)
        self.assertEqual(result["tail_feedback_min_mean_max_ma"], [200, 200, 200])
        self.assertEqual(result["tail_duty_mean_permille"], 106)

    def test_transient_uses_20_sample_mean(self):
        wave = self.wave()
        for row in wave[:100]:
            row["forward_current_ma"] = 0
        result = metrics(wave, 200)
        self.assertEqual(result["first_90pct_ms"], 117)
        self.assertEqual(result["settled_5pct_20ms_mean_ms"], 118)

    def test_timestamp_wrap(self):
        wave = self.wave(start_us=0xFFFFFFFF - 50000)
        for row in wave[:100]:
            row["forward_current_ma"] = 0
        self.assertEqual(metrics(wave, 200)["settled_5pct_20ms_mean_ms"], 118)

    def test_never_settles(self):
        wave = self.wave()
        for row in wave:
            row["forward_current_ma"] = 100
        self.assertIsNone(metrics(wave, 200)["settled_5pct_20ms_mean_ms"])

    def test_requires_enough_matching_samples(self):
        for wave in (self.wave()[:999], self.wave(-200)):
            with self.subTest(count=len(wave)), self.assertRaises(GuardFailure):
                metrics(wave, 200)


if __name__ == "__main__":
    unittest.main()
