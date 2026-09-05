"""Pure host test guards: importing this module never opens an ECU socket."""

import importlib.util
from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location("ecu_valve_guarded_test",
                                           TOOLS / "ecu_valve_guarded_test.py")
GUARD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GUARD)


class ValveBenchGuardTests(unittest.TestCase):
    def setUp(self):
        self.sample = {
            "forward_current_ma": 100, "reverse_current_ma": 1,
            "forward_duty_permille": 75, "reverse_duty_permille": 0,
            "fault_flags": 0, "requested_target_ma": 100,
        }

    def test_valid_forward(self):
        GUARD.sample_guard(self.sample, 100, 0.5)

    def test_valid_reverse(self):
        GUARD.sample_guard({
            **self.sample, "forward_current_ma": 0, "reverse_current_ma": 200,
            "forward_duty_permille": 0, "reverse_duty_permille": 130,
            "requested_target_ma": -200,
        }, -200, 0.5)

    def test_each_guard_rejects(self):
        for patch in (
            {"forward_current_ma": 0},
            {"forward_current_ma": 300},
            {"forward_duty_permille": 301},
            {"reverse_duty_permille": 10},
            {"reverse_current_ma": 60},
            {"fault_flags": 1},
            {"requested_target_ma": 200},
        ):
            with self.subTest(patch=patch), self.assertRaises(GUARD.GuardFailure):
                GUARD.sample_guard({**self.sample, **patch}, 100, 0.5)

    def test_zero_transition_allows_prior_coil_current_then_requires_decay(self):
        residual = {**self.sample, "forward_current_ma": 201,
                    "requested_target_ma": 200}
        GUARD.sample_guard(residual, 0, 0.01)
        with self.assertRaises(GUARD.GuardFailure):
            GUARD.sample_guard(residual, 0, 0.31)
        GUARD.sample_guard({**residual, "forward_current_ma": 1,
                            "forward_duty_permille": 0,
                            "requested_target_ma": 0}, 0, 0.31)

    def test_zero_transition_still_limits_current_and_pwm(self):
        for patch in ({"forward_current_ma": 351},
                      {"forward_duty_permille": 301}):
            with self.subTest(patch=patch), self.assertRaises(GUARD.GuardFailure):
                GUARD.sample_guard({**self.sample, **patch}, 0, 0.01)


if __name__ == "__main__":
    unittest.main()
