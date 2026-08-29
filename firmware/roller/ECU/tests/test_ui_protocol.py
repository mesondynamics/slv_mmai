import importlib.util
import pathlib
import struct
import sys
import unittest


PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = PROJECT_ROOT / "tools" / "ecu_debug_ui.py"
SPEC = importlib.util.spec_from_file_location("ecu_debug_ui", MODULE_PATH)
UI = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = UI
SPEC.loader.exec_module(UI)


class ProtocolV2Test(unittest.TestCase):
    def test_abi_sizes(self):
        self.assertEqual(UI.V2_HEADER_SIZE, 24)
        self.assertEqual(struct.calcsize(UI.CONTROL_FORMAT), 36)
        self.assertEqual(struct.calcsize(UI.STATUS_FORMAT), 100)
        self.assertEqual(struct.calcsize(UI.DIAGNOSTIC_FORMAT), 86)
        self.assertEqual(struct.calcsize(UI.TELEMETRY_BATCH_FORMAT), 12)
        self.assertEqual(struct.calcsize(UI.TELEMETRY_SAMPLE_FORMAT), 20)
        self.assertEqual(struct.calcsize(UI.CHANNEL_CONFIG_FORMAT), 20)
        self.assertEqual(UI.OTA_MAX_PACKAGE_SIZE, 2 * 1024 * 1024)
        self.assertEqual(len(UI.STATUS_FIELDS), 64)
        self.assertEqual(len(UI.DIAGNOSTIC_FIELDS), 32)
        self.assertEqual(len(UI.CONTROL_FIELDS), 29)

    def test_crc32c_known_vector(self):
        self.assertEqual(UI.crc32c(b"123456789"), 0xE3069283)

    def test_control_round_trip(self):
        control = UI.neutral_control()
        control.update(valve_current_target_ma=-1370, engine_speed_level=1,
                       emergency_stop_request=1)
        payload = UI.build_control_payload(control, 2, 2)
        frame = UI.encode_v2(UI.MSG_CONTROL, UI.CONTROL_FLAG_CLEAR_FAULT,
                             0x12345678, payload, 0x76543210)
        decoded = UI.decode_v2(frame)
        self.assertIsNotNone(decoded)
        header, received_payload = decoded
        self.assertEqual(header["message_type"], UI.MSG_CONTROL)
        self.assertEqual(header["flags"], UI.CONTROL_FLAG_CLEAR_FAULT)
        self.assertEqual(header["sequence"], 0x12345678)
        values = struct.unpack(UI.CONTROL_FORMAT, received_payload)
        self.assertEqual(values[:3], (2, 2, 0))
        fields = dict(zip(UI.CONTROL_FIELDS, values[3:]))
        self.assertEqual(fields["valve_current_target_ma"], -1370)
        self.assertEqual(fields["engine_speed_level"], 1)
        self.assertEqual(fields["emergency_stop_request"], 1)

    def test_corrupt_and_v1_frames_are_rejected(self):
        frame = bytearray(UI.encode_v2(UI.MSG_CONFIG_GET, 0, 7, b"", 1))
        frame[-1] ^= 1
        self.assertIsNone(UI.decode_v2(frame))
        self.assertIsNone(UI.decode_v2(b"\xa5\x00\x00\x5a"))

    def test_sanitizer_bounds_deadband_and_exclusion(self):
        control = UI.neutral_control()
        control.update(engine_speed_level=20, valve_current_target_ma=-9999,
                       vib_strong_on=1, vib_weak_on=1)
        result = UI.sanitize_control(control)
        self.assertEqual(result["engine_speed_level"], 3)
        self.assertEqual(result["valve_current_target_ma"], -2000)
        self.assertEqual(result["vib_strong_on"], 1)
        self.assertEqual(result["vib_weak_on"], 0)
        control["valve_current_target_ma"] = 49
        self.assertEqual(UI.sanitize_control(control)["valve_current_target_ma"], 0)

    def test_valve_config_round_trip_and_reply(self):
        config = UI.default_valve_config()
        config["reverse"]["kp_permille_per_amp"] = 777
        packed = UI.pack_valve_config(config)
        self.assertEqual(len(packed), 48)
        unpacked, end = UI.unpack_valve_config(packed)
        self.assertEqual(end, 48)
        self.assertEqual(unpacked, config)
        prefix = struct.pack("<iIIII4B", 0, 42, 3, 2, 0x12345678,
                             1, 1, 0, 0)
        reply = UI.unpack_config_reply(prefix + packed)
        self.assertEqual(reply["request_sequence"], 42)
        self.assertEqual(reply["active_revision"], 3)
        self.assertEqual(reply["config"]["reverse"]["kp_permille_per_amp"], 777)

    def test_ui_uses_current_interface_only(self):
        html = (PROJECT_ROOT / "tools" / "ecu_debug_ui" / "index.html").read_text()
        self.assertIn("valve_current_target_ma", html)
        self.assertIn("/api/valve/apply", html)
        self.assertIn("/api/valve/stream", html)
        self.assertIn("/api/ota/upload", html)
        self.assertNotIn("throttle_percent", html)


if __name__ == "__main__":
    unittest.main()
