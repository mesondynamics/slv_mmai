from pathlib import Path
import re
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[1]
NSC_SOURCE = PROJECT_ROOT / "Secure" / "Core" / "Src" / "secure_nsc.c"
NSC_HEADER = PROJECT_ROOT / "Secure_nsclib" / "secure_nsc.h"
SAFETY_HEADER = PROJECT_ROOT / "Secure_nsclib" / "safety_api.h"
FROZEN_MAP = PROJECT_ROOT / "Secure_nsclib" / "secure_nsc_abi_v1.s"
STEERING_CAN_SOURCE = (
    PROJECT_ROOT / "Secure" / "App" / "Src" / "steering_can.c"
)


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


class SteeringNscSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = NSC_SOURCE.read_text(encoding="utf-8")
        cls.header = NSC_HEADER.read_text(encoding="utf-8")
        cls.safety = SAFETY_HEADER.read_text(encoding="utf-8")
        cls.body = function_body(
            cls.source, "SECURE_SafetyGetSteeringSnapshot"
        )

    def test_versioned_capacity_aware_signature_matches(self):
        normalized_header = re.sub(r"\s+", " ", self.header)
        normalized_source = re.sub(r"\s+", " ", self.body)
        signature = (
            "SECURE_SafetyGetSteeringSnapshot( uint32_t requested_version, "
            "SAFETY_SteeringSnapshot *snapshot, uint32_t snapshot_capacity)"
        )
        self.assertIn(signature, normalized_header)
        self.assertIn(signature, normalized_source)

    def test_scalars_and_alignment_are_rejected_before_cmse_dereference(self):
        checks = [
            "requested_version != SAFETY_STEERING_API_VERSION",
            "snapshot_capacity != sizeof(SAFETY_SteeringSnapshot)",
            "snapshot == NULL",
            "_Alignof(SAFETY_SteeringSnapshot)",
            "cmse_check_address_range(",
            "Safety_GetSteeringSnapshot(&secure_snapshot)",
            "*checked_snapshot = secure_snapshot",
        ]
        positions = [self.body.index(check) for check in checks]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("return SAFETY_RESULT_UNSUPPORTED_VERSION", self.body)
        self.assertIn("return SAFETY_RESULT_RANGE", self.body)
        self.assertGreaterEqual(
            self.body.count("return SAFETY_RESULT_BAD_ARGUMENT"), 2
        )
        self.assertIn(
            "CMSE_NONSECURE | CMSE_MPU_READWRITE", self.body
        )
        self.assertIn(
            "SAFETY_SteeringSnapshot secure_snapshot = {0}", self.body
        )

    def test_new_veneer_is_appended_after_frozen_v1_entry_points(self):
        self.assertLess(
            self.source.index("SECURE_SafetyKickWatchdog"),
            self.source.index("SECURE_SafetyGetSteeringSnapshot"),
        )
        frozen = FROZEN_MAP.read_text(encoding="utf-8")
        self.assertNotIn("SECURE_SafetyGetSteeringSnapshot", frozen)

    def test_frozen_actuator_snapshot_has_no_steering_extension(self):
        end = self.safety.index("} SAFETY_ActuatorSnapshot;")
        start = self.safety.rfind("typedef struct", 0, end)
        self.assertGreaterEqual(start, 0)
        actuator = self.safety[start:end]
        self.assertNotIn("steering", actuator.lower())
        self.assertIn("typedef struct", self.safety)
        self.assertIn("SAFETY_SteeringSnapshot", self.safety)

    def test_steering_fault_reporting_does_not_force_global_outputs_safe(self):
        safety_service = (
            PROJECT_ROOT / "Secure" / "App" / "Src" / "safety_service.c"
        ).read_text(encoding="utf-8")
        body = function_body(safety_service, "Safety_OneMillisecondTick")
        start = body.index("if (steering_active_fault)")
        end = body.index("if (Safety_PhysicalEStopActive", start)
        steering_branch = body[start:end]
        self.assertIn(
            "safety_status |= SAFETY_STATUS_STEERING_CAN_FAULT",
            steering_branch,
        )
        self.assertNotIn("Safety_ForceOutputsSafe", steering_branch)


class SteeringCanAdapterSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = STEERING_CAN_SOURCE.read_text(encoding="utf-8")

    def test_bus_off_irq_has_priority_over_warning_and_passive_bits(self):
        body = function_body(self.source, "SteeringCan_ProcessIrqErrors")
        normalized = re.sub(r"\s+", " ", body)
        self.assertRegex(
            normalized,
            r"if \(\(status & FDCAN_IT_BUS_OFF\) != 0U\).*?"
            r"SteeringCan_RecoverBusOff\(\); } else if "
            r"\(\(status & \(FDCAN_IT_ERROR_WARNING \| "
            r"FDCAN_IT_ERROR_PASSIVE\)\) != 0U\)",
        )

    def test_rx_adapter_accepts_only_exact_classic_extended_dlc8(self):
        callback = function_body(
            self.source, "SteeringCan_HandleRxFifo0"
        )
        self.assertIn("header.IdType != FDCAN_EXTENDED_ID", callback)
        self.assertIn("header.RxFrameType != FDCAN_DATA_FRAME", callback)
        self.assertIn("header.FDFormat != FDCAN_CLASSIC_CAN", callback)
        self.assertIn("header.DataLength != FDCAN_DLC_BYTES_8", callback)
        self.assertIn("item->length = 8U", callback)

    def test_init_uses_two_exact_filters_and_global_reject(self):
        body = function_body(self.source, "SteeringCan_Init")
        normalized = re.sub(r"\s+", " ", body)
        self.assertIn("filter.FilterType = FDCAN_FILTER_MASK", normalized)
        self.assertRegex(
            normalized,
            r"filter\.FilterIndex = 0U;.*?"
            r"filter\.FilterID1 = STEERING_CONTROL_SDO_RESPONSE_EXT_ID;.*?"
            r"filter\.FilterID2 = 0x1FFFFFFFUL;.*?"
            r"HAL_FDCAN_ConfigFilter\(&hfdcan2, &filter\).*?"
            r"filter\.FilterIndex = 1U;.*?"
            r"filter\.FilterID1 = STEERING_CONTROL_HEARTBEAT_EXT_ID;.*?"
            r"HAL_FDCAN_ConfigFilter\(&hfdcan2, &filter\)",
        )
        self.assertRegex(
            normalized,
            r"HAL_FDCAN_ConfigGlobalFilter\("
            r"&hfdcan2, FDCAN_REJECT, FDCAN_REJECT, "
            r"FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE\)",
        )


if __name__ == "__main__":
    unittest.main()
