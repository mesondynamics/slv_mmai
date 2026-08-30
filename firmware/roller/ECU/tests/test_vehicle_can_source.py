from pathlib import Path
import re
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[1]
VEHICLE = PROJECT_ROOT / "Secure/App/Src/vehicle_can.c"
DISPATCH = PROJECT_ROOT / "Secure/App/Src/fdcan_callbacks.c"
NSC_SOURCE = PROJECT_ROOT / "Secure/Core/Src/secure_nsc.c"
NSC_HEADER = PROJECT_ROOT / "Secure_nsclib/secure_nsc.h"
WRAPPER = PROJECT_ROOT / "NonSecure/App/Src/j1939.c"
APP = PROJECT_ROOT / "NonSecure/App/Src/ecu_app.c"


def function_body(source: str, function_name: str) -> str:
    start = source.index(function_name)
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


class VehicleCanSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.vehicle = VEHICLE.read_text(encoding="utf-8")
        cls.dispatch = DISPATCH.read_text(encoding="utf-8")
        cls.nsc = NSC_SOURCE.read_text(encoding="utf-8")
        cls.header = NSC_HEADER.read_text(encoding="utf-8")
        cls.wrapper = WRAPPER.read_text(encoding="utf-8")

    def test_nsc_boundary_is_versioned_bounded_and_copy_once(self):
        body = function_body(self.nsc, "SECURE_SafetyGetJ1939Snapshot")
        normalized_header = re.sub(r"\s+", " ", self.header)
        signature = (
            "SECURE_SafetyGetJ1939Snapshot( uint32_t requested_version, "
            "SAFETY_J1939Snapshot *snapshot, uint32_t snapshot_capacity)"
        )
        self.assertIn(signature, normalized_header)
        checks = [
            "requested_version != SAFETY_J1939_API_VERSION",
            "snapshot_capacity != sizeof(SAFETY_J1939Snapshot)",
            "snapshot == NULL",
            "_Alignof(SAFETY_J1939Snapshot)",
            "cmse_check_address_range(",
            "Safety_GetJ1939Snapshot(&secure_snapshot)",
            "*checked_snapshot = secure_snapshot",
        ]
        positions = [body.index(item) for item in checks]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("CMSE_NONSECURE | CMSE_MPU_READWRITE", body)
        self.assertIn("SAFETY_J1939Snapshot secure_snapshot = {0}", body)
        self.assertIn("memset(secure_snapshot.reserved", body)

    def test_nonsecure_is_only_a_fail_closed_snapshot_wrapper(self):
        self.assertNotIn('#include "fdcan.h"', self.wrapper)
        self.assertNotIn("hfdcan", self.wrapper)
        self.assertNotIn("HAL_FDCAN_", self.wrapper)
        self.assertIn("SECURE_SafetyGetJ1939Snapshot(", self.wrapper)
        refresh = function_body(self.wrapper, "J1939_Refresh")
        self.assertLess(
            refresh.index("result != SAFETY_RESULT_OK"),
            refresh.index("J1939_ClearValidity()"),
        )
        app = APP.read_text(encoding="utf-8")
        init = function_body(app, "ECU_AppInit")
        self.assertIn("(void)J1939_Init();", init)
        self.assertNotRegex(init, r"if\s*\(\s*!J1939_Init\(\)\s*\)")
        self.assertLess(init.index("J1939_Init"), init.index("ECU_NetworkInit"))

    def test_filters_and_frame_validation_are_exact(self):
        init = function_body(self.vehicle, "VehicleCan_Init")
        for pgn in ("EEC1", "EEC2", "ET1", "EFL_P1", "AMB", "DM1"):
            self.assertIn(f"VEHICLE_J1939_PGN_{pgn}", init)
        self.assertIn("filter.FilterType = FDCAN_FILTER_MASK", init)
        self.assertIn("filter.FilterID2 = VEHICLE_CAN_FILTER_MASK", init)
        self.assertRegex(
            re.sub(r"\s+", " ", init),
            r"HAL_FDCAN_ConfigGlobalFilter\(&hfdcan1, "
            r"FDCAN_REJECT, FDCAN_REJECT, FDCAN_REJECT_REMOTE, "
            r"FDCAN_REJECT_REMOTE\)",
        )
        rx = function_body(self.vehicle, "VehicleCan_HandleRxFifo0")
        for check in (
            "header.IdType != FDCAN_EXTENDED_ID",
            "header.RxFrameType != FDCAN_DATA_FRAME",
            "header.FDFormat != FDCAN_CLASSIC_CAN",
            "header.DataLength != FDCAN_DLC_BYTES_8",
        ):
            self.assertIn(check, rx)

    def test_isr_and_worker_have_fixed_budgets(self):
        self.assertRegex(
            self.vehicle,
            r"#define VEHICLE_CAN_RX_RING_SIZE\s+8U",
        )
        self.assertRegex(
            self.vehicle,
            r"#define VEHICLE_CAN_ISR_FRAME_BUDGET\s+3U",
        )
        self.assertRegex(
            self.vehicle,
            r"#define VEHICLE_CAN_PROCESS_FRAME_BUDGET\s+3U",
        )
        rx = function_body(self.vehicle, "VehicleCan_HandleRxFifo0")
        self.assertIn("handled < VEHICLE_CAN_ISR_FRAME_BUDGET", rx)
        worker = function_body(self.vehicle, "VehicleCan_ProcessRx")
        self.assertIn("handled < VEHICLE_CAN_PROCESS_FRAME_BUDGET", worker)

    def test_one_callback_owner_dispatches_instances_without_aliasing(self):
        sources = list((PROJECT_ROOT / "Secure/App/Src").glob("*.c"))
        combined = "\n".join(path.read_text(encoding="utf-8") for path in sources)
        for callback in (
            "HAL_FDCAN_RxFifo0Callback",
            "HAL_FDCAN_ErrorStatusCallback",
            "HAL_FDCAN_ErrorCallback",
        ):
            definitions = re.findall(
                rf"^void\s+{callback}\s*\(", combined, re.MULTILINE
            )
            self.assertEqual(len(definitions), 1, callback)
        rx = function_body(self.dispatch, "HAL_FDCAN_RxFifo0Callback")
        self.assertRegex(
            re.sub(r"\s+", " ", rx),
            r"Instance == FDCAN1.*VehicleCan_HandleRxFifo0.*"
            r"Instance == FDCAN2.*SteeringCan_HandleRxFifo0",
        )


if __name__ == "__main__":
    unittest.main()
