from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[1]
VEHICLE_SOURCE = PROJECT_ROOT / "Secure/App/Src/vehicle_can.c"
SAFETY_SOURCE = PROJECT_ROOT / "Secure/App/Src/safety_service.c"


def function_body(source: str, function_name: str) -> str:
    match = re.search(
        rf"\b{re.escape(function_name)}\s*\([^;{{]*\)\s*{{", source
    )
    if match is None:
        raise AssertionError(f"function {function_name} not found")
    start = match.start()
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unterminated function {function_name}")


class VehicleCanHealthTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.vehicle = VEHICLE_SOURCE.read_text(encoding="utf-8")
        cls.safety = SAFETY_SOURCE.read_text(encoding="utf-8")

    def test_c_health_adapter_builds_and_runs_when_host_compiler_exists(self):
        compiler = shutil.which("cc")
        with tempfile.TemporaryDirectory(prefix="vehicle-can-health-") as tmp:
            common = [
                "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-I{PROJECT_ROOT / 'tests/stubs/vehicle_can'}",
                f"-I{PROJECT_ROOT / 'Secure/App/Inc'}",
                f"-I{PROJECT_ROOT / 'Secure_nsclib'}",
            ]
            sources = [
                PROJECT_ROOT / "Secure/App/Src/vehicle_j1939.c",
                VEHICLE_SOURCE,
                PROJECT_ROOT / "tests/test_vehicle_can_health.c",
            ]
            if compiler is not None:
                executable = Path(tmp) / "vehicle_can_health"
                subprocess.run(
                    [compiler, *common, *(str(path) for path in sources),
                     "-o", str(executable)],
                    check=True, capture_output=True, text=True,
                )
                subprocess.run(
                    [str(executable)], check=True,
                    capture_output=True, text=True,
                )
                return

            arm_compiler = Path(
                "/home/plac/.local/share/stm32cube/bundles/"
                "gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc"
            )
            if not arm_compiler.is_file():
                self.skipTest("neither native nor STM32 C compiler available")
            for source in sources:
                subprocess.run(
                    [str(arm_compiler), *common, "-mcpu=cortex-m33", "-c",
                     str(source), "-o", str(Path(tmp) / f"{source.stem}.o")],
                    check=True, capture_output=True, text=True,
                )

    def test_current_health_policy_behavior(self):
        def healthy(initialized, current_fault, init_fault, bus_state):
            return (
                initialized and not current_fault and not init_fault
                and bus_state == "active"
            )

        self.assertFalse(healthy(False, True, True, "stopped"))
        self.assertTrue(healthy(True, False, False, "active"))
        for state in ("warning", "passive", "off"):
            self.assertFalse(healthy(True, True, False, state))
        self.assertFalse(healthy(True, True, False, "active"))
        self.assertTrue(healthy(True, False, False, "active"))

    def test_active_poll_clears_then_current_errors_reassert(self):
        process = function_body(self.vehicle, "VehicleCan_Process")
        self.assertLess(
            process.index("VehicleCan_PollStatus"),
            process.index("VehicleCan_ProcessIrqErrors"),
        )
        self.assertLess(
            process.index("VehicleCan_ProcessIrqErrors"),
            process.index("VehicleCan_ProcessRx"),
        )
        poll = function_body(self.vehicle, "VehicleCan_PollStatus")
        normalized = re.sub(r"\s+", " ", poll)
        self.assertIn(
            "vehicle_can_health_fault = "
            "(bus_state == SAFETY_J1939_BUS_ACTIVE) ? 0U : 1U;",
            normalized,
        )
        self.assertGreaterEqual(poll.count("vehicle_can_health_fault = 1U"), 1)

    def test_safety_owner_publishes_non_latching_isolated_live_fault(self):
        tick = function_body(self.safety, "Safety_OneMillisecondTick")
        start = tick.index("if (VehicleCan_IsHealthy())")
        end = tick.index("if (steering_active_fault)", start)
        vehicle_branch = tick[start:end]
        self.assertIn(
            "safety_status &= ~SAFETY_STATUS_VEHICLE_CAN_FAULT",
            vehicle_branch,
        )
        self.assertIn(
            "safety_status |= SAFETY_STATUS_VEHICLE_CAN_FAULT",
            vehicle_branch,
        )
        self.assertNotIn("SAFETY_STATUS_FAULT_LATCHED", vehicle_branch)
        self.assertNotIn("Safety_ForceOutputsSafe", vehicle_branch)

    def test_snapshot_time_and_copy_share_one_outer_critical_section(self):
        getter = function_body(self.safety, "Safety_GetJ1939Snapshot")
        normalized = re.sub(r"\s+", " ", getter)
        self.assertRegex(
            normalized,
            r"primask = Safety_EnterCritical\(\); "
            r"result = VehicleCan_GetSnapshot\(snapshot, secure_uptime_ms\); "
            r"Safety_ExitCritical\(primask\); return result;",
        )
        self.assertNotIn("now_ms", getter)


if __name__ == "__main__":
    unittest.main()
