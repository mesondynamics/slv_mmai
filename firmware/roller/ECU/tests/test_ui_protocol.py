import importlib.util
import io
import pathlib
import struct
import sys
import threading
import time
import unittest
from collections import deque
from email.message import Message


PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = PROJECT_ROOT / "tools" / "ecu_debug_ui.py"
SPEC = importlib.util.spec_from_file_location("ecu_debug_ui", MODULE_PATH)
UI = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = UI
SPEC.loader.exec_module(UI)


class ProtocolV2Test(unittest.TestCase):
    ARM_TOKEN = "arm-token-00000001"
    HOLD_TOKEN = "hold-token-0000001"

    @staticmethod
    def bridge(sender_id=2, enabled=True):
        bridge = UI.BenchBridge.__new__(UI.BenchBridge)
        bridge.state = UI.BenchState(sender_id=sender_id, enabled=enabled)
        bridge.lock = threading.RLock()
        bridge.tuning_clients = {}
        bridge.tuning_tombstones = {}
        bridge.cancelled_hold_tokens = set()
        bridge.cancelled_hold_order = deque()
        return bridge

    @staticmethod
    def steering_status(sequence=10, timestamp_ms=100, *, active=False):
        return {
            "sequence_id": sequence,
            "timestamp_ms": timestamp_ms,
            "requested_velocity_tdeg_per_s": 0,
            "applied_velocity_tdeg_per_s": 0,
            "speed_command_permille": 0,
            "rx_age_ms": 0,
            "motor_fault_code": 0,
            "motor_speed_feedback_raw": 0,
            "status_flags": UI.STEERING_STATUS_READY,
            "fault_flags": 0,
            "tx_frames": 1,
            "rx_frames": 1,
            "tx_errors": 0,
            "rx_errors": 0,
            "bus_off_events": 0,
            "state": UI.STEERING_STATE_ACTIVE if active else
                     UI.STEERING_STATE_DISABLED,
            "bus_state": 1,
            "command_enable": 1 if active else 0,
            "motor_enable_confirmed": 1 if active else 0,
        }

    def arm_and_confirm(self, bridge):
        bridge.state.steering_status = self.steering_status()
        bridge.state.steering_received_at = time.monotonic()
        bridge.steering_arm(self.ARM_TOKEN)
        bridge.state.steering_arm_started_at -= 0.001
        accepted = bridge._accept_steering_status_locked(
            self.steering_status(11, 101, active=True), time.monotonic()
        )
        self.assertTrue(accepted)
        self.assertTrue(bridge.state.steering_arm_confirmed)

    @staticmethod
    def handler(body=b"{}", **headers):
        handler = UI.UiHandler.__new__(UI.UiHandler)
        handler.headers = Message()
        defaults = {
            "Host": "127.0.0.1:8088",
            "Content-Type": "application/json",
            "Content-Length": str(len(body)),
            "X-ECU-UI-Token": "test-csrf-token-0000000000000000",
        }
        for name, value in headers.items():
            defaults[name.replace("_", "-")] = value
        for name, value in defaults.items():
            if value is not None:
                handler.headers[name] = value
        handler.rfile = io.BytesIO(body)
        handler.wfile = io.BytesIO()
        handler.csrf_token = "test-csrf-token-0000000000000000"
        handler.listen_port = 8088
        handler.allow_remote_ui = False
        return handler

    def test_abi_sizes(self):
        self.assertEqual(UI.V2_HEADER_SIZE, 24)
        self.assertEqual(struct.calcsize(UI.CONTROL_FORMAT), 36)
        self.assertEqual(struct.calcsize(UI.STATUS_FORMAT), 100)
        self.assertEqual(struct.calcsize(UI.DIAGNOSTIC_FORMAT), 86)
        self.assertEqual(struct.calcsize(UI.TELEMETRY_BATCH_FORMAT), 12)
        self.assertEqual(struct.calcsize(UI.TELEMETRY_SAMPLE_FORMAT), 20)
        self.assertEqual(struct.calcsize(UI.STEERING_STATUS_FORMAT), 52)
        self.assertEqual(struct.calcsize(UI.CHANNEL_CONFIG_FORMAT), 20)
        self.assertEqual(UI.OTA_MAX_PACKAGE_SIZE, 2 * 1024 * 1024)
        self.assertEqual(len(UI.STATUS_FIELDS), 64)
        self.assertEqual(len(UI.DIAGNOSTIC_FIELDS), 32)
        self.assertEqual(len(UI.STEERING_STATUS_FIELDS), 19)
        self.assertEqual(len(UI.CONTROL_FIELDS), 29)
        self.assertEqual(UI.MSG_STEERING_STATUS, 0x06)
        self.assertEqual(UI.CONTROL_FLAG_STEERING_RATE, 1 << 2)

    def test_crc32c_known_vector(self):
        self.assertEqual(UI.crc32c(b"123456789"), 0xE3069283)

    def test_steering_status_preserves_signed_command_and_feedback(self):
        values = (
            7, 1234, -250, -240, -50, 12, 0, -47,
            1, 0, 10, 11, 0, 0, 0, 3, 1, 1, 1,
        )
        decoded = dict(zip(
            UI.STEERING_STATUS_FIELDS,
            struct.unpack(UI.STEERING_STATUS_FORMAT,
                          struct.pack(UI.STEERING_STATUS_FORMAT, *values)),
        ))
        self.assertEqual(decoded["speed_command_permille"], -50)
        self.assertEqual(decoded["motor_speed_feedback_raw"], -47)

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

    def test_steering_rate_uses_legacy_wire_offsets_only_when_requested(self):
        control = UI.neutral_control()
        control[UI.STEERING_VELOCITY_FIELD] = -7000
        control["steering_target_tdeg"] = 25
        control["steering_speed_tdeg_per_s"] = 1000
        control["steering_enable"] = 1

        legacy = struct.unpack(
            UI.CONTROL_FORMAT,
            UI.build_control_payload(control, 2, 2, steering_rate=False),
        )
        legacy_fields = dict(zip(UI.CONTROL_FIELDS, legacy[3:]))
        self.assertEqual(legacy_fields["steering_target_tdeg"], 25)
        self.assertEqual(legacy_fields["steering_speed_tdeg_per_s"], 1000)

        rate = struct.unpack(
            UI.CONTROL_FORMAT,
            UI.build_control_payload(control, 2, 2, steering_rate=True),
        )
        rate_fields = dict(zip(UI.CONTROL_FIELDS, rate[3:]))
        self.assertEqual(rate_fields["steering_target_tdeg"], -6000)
        self.assertEqual(rate_fields["steering_speed_tdeg_per_s"], 0)
        self.assertEqual(rate_fields["steering_enable"], 1)

    def test_steering_flag_is_explicit_and_sender_three_is_not_manual_ui(self):
        bridge = self.bridge(enabled=False)
        control = UI.neutral_control()
        control[UI.STEERING_VELOCITY_FIELD] = 6
        control["steering_enable"] = 1
        header, _ = UI.decode_v2(
            bridge._control_packet(control, 0, steering_rate=True)
        )
        self.assertEqual(header["flags"], UI.CONTROL_FLAG_STEERING_RATE)
        header, _ = UI.decode_v2(
            bridge._control_packet(UI.neutral_control(),
                                   UI.CONTROL_FLAG_RELEASE)
        )
        self.assertEqual(header["flags"], UI.CONTROL_FLAG_RELEASE)

        bridge.state.sender_id = 3
        with self.assertRaisesRegex(ValueError, "reserved"):
            bridge.patch_control({UI.STEERING_VELOCITY_FIELD: 6})

    def test_manual_steering_requires_feedback_and_fails_safe(self):
        bridge = self.bridge()

        with self.assertRaisesRegex(ValueError, "fresh fault-free"):
            bridge.steering_arm(self.ARM_TOKEN)

        now = time.monotonic()
        bridge.state.steering_status = self.steering_status()
        bridge.state.steering_received_at = now
        bridge.steering_arm(self.ARM_TOKEN)
        with self.assertRaisesRegex(ValueError, "480.0"):
            bridge.steering_hold_start(self.HOLD_TOKEN, 4801)
        with self.assertRaisesRegex(ValueError, "enable ACK"):
            bridge.steering_hold_start(self.HOLD_TOKEN, 10)
        bridge.state.steering_arm_started_at -= 0.001
        self.assertTrue(bridge._accept_steering_status_locked(
            self.steering_status(11, 101, active=True), time.monotonic()
        ))
        bridge.steering_hold_start(self.HOLD_TOKEN, 10)
        self.assertTrue(bridge.state.steering_rate_mode)
        self.assertGreater(bridge.state.steering_manual_deadline, now)

        bridge.state.steering_manual_deadline = time.monotonic() - 0.001
        self.assertTrue(
            bridge._enforce_steering_safety_locked(time.monotonic())
        )
        self.assertEqual(
            bridge.state.control[UI.STEERING_VELOCITY_FIELD], 0
        )
        self.assertEqual(bridge.state.control["steering_enable"], 0)
        self.assertFalse(bridge.state.steering_rate_mode)

        bridge.state.sender_id = 3
        bridge.state.steering_received_at = 0.0
        bridge.patch_control({
            UI.STEERING_VELOCITY_FIELD: 0,
            "steering_enable": 0,
        })

    def test_steering_status_loss_forces_disable(self):
        bridge = self.bridge(sender_id=1)
        bridge.state.control[UI.STEERING_VELOCITY_FIELD] = 5
        bridge.state.control["steering_enable"] = 1
        bridge.state.steering_rate_mode = True
        bridge.state.steering_received_at = (
            time.monotonic() - UI.STEERING_STATUS_MAX_AGE_S - 0.01
        )
        bridge.state.steering_status = {
            "status_flags": UI.STEERING_STATUS_READY,
            "fault_flags": 0,
            "motor_fault_code": 0,
            "bus_state": 1,
        }

        self.assertTrue(
            bridge._enforce_steering_safety_locked(time.monotonic())
        )
        self.assertEqual(bridge.state.control["steering_enable"], 0)
        self.assertEqual(
            bridge.state.control[UI.STEERING_VELOCITY_FIELD], 0
        )

    def test_manual_steering_repeated_nonzero_patch_cannot_extend_deadline(self):
        bridge = self.bridge()
        self.arm_and_confirm(bridge)

        bridge.steering_hold_start(self.HOLD_TOKEN, 10)
        first_deadline = bridge.state.steering_manual_deadline
        bridge.patch_control(
            {UI.STEERING_VELOCITY_FIELD: 20},
            steering_hold_token=self.HOLD_TOKEN,
        )
        self.assertEqual(bridge.state.steering_manual_deadline, first_deadline)
        bridge.steering_hold_stop(self.HOLD_TOKEN, False)
        self.assertEqual(bridge.state.steering_manual_deadline, 0.0)
        second_token = "hold-token-0000002"
        bridge.steering_hold_start(second_token, 10)
        self.assertGreater(bridge.state.steering_manual_deadline, first_deadline)

    def test_sender_change_cannot_bypass_active_manual_steering_limit(self):
        bridge = self.bridge()
        bridge.state.control[UI.STEERING_VELOCITY_FIELD] = 10
        bridge.state.control["steering_enable"] = 1
        bridge.state.steering_rate_mode = True

        with self.assertRaisesRegex(ValueError, "release ECU control"):
            bridge.configure({"sender_id": 3})
        self.assertEqual(bridge.state.sender_id, 2)

        bridge.configure({"enabled": False})
        self.assertEqual(bridge.state.control[UI.STEERING_VELOCITY_FIELD], 0)
        self.assertEqual(bridge.state.control["steering_enable"], 0)
        self.assertFalse(bridge.state.steering_rate_mode)

    def test_arm_requires_exact_disabled_snapshot_and_post_arm_ack(self):
        bridge = self.bridge()
        stale_active = self.steering_status(active=True)
        bridge.state.steering_status = stale_active
        bridge.state.steering_received_at = time.monotonic()
        with self.assertRaisesRegex(ValueError, "disabled CAN2"):
            bridge.steering_arm(self.ARM_TOKEN)

        bridge.state.steering_status = self.steering_status()
        bridge.steering_arm(self.ARM_TOKEN)
        bridge.state.steering_arm_started_at -= 0.001
        # An ACK with the arm baseline counters is a replay, not evidence for
        # the current enable transaction.
        bridge.state.steering_status = self.steering_status(active=True)
        bridge.state.steering_received_at = time.monotonic()
        self.assertFalse(
            bridge._refresh_steering_arm_confirmation_locked(time.monotonic())
        )
        with self.assertRaisesRegex(ValueError, "current-arm"):
            bridge.steering_hold_start(self.HOLD_TOKEN, 10)

    def test_reordered_arm_and_hold_requests_are_cancelled(self):
        bridge = self.bridge()
        bridge.state.steering_status = self.steering_status()
        bridge.state.steering_received_at = time.monotonic()
        bridge.steering_disarm(self.ARM_TOKEN, None)
        with self.assertRaisesRegex(ValueError, "cancelled"):
            bridge.steering_arm(self.ARM_TOKEN)

        bridge = self.bridge()
        self.arm_and_confirm(bridge)
        bridge.steering_hold_stop(self.HOLD_TOKEN, False)
        with self.assertRaisesRegex(ValueError, "cancelled"):
            bridge.steering_hold_start(self.HOLD_TOKEN, 10)
        self.assertEqual(
            bridge.state.control[UI.STEERING_VELOCITY_FIELD], 0
        )

    def test_hold_stop_is_unconditional_when_feedback_is_unhealthy(self):
        bridge = self.bridge()
        self.arm_and_confirm(bridge)
        bridge.steering_hold_start(self.HOLD_TOKEN, 10)
        bridge.state.steering_received_at = 0.0
        bridge.steering_hold_stop(self.HOLD_TOKEN, False)
        self.assertEqual(
            bridge.state.control[UI.STEERING_VELOCITY_FIELD], 0
        )
        self.assertEqual(bridge.state.control["steering_enable"], 0)
        self.assertFalse(bridge.state.steering_rate_mode)

    def test_current_ack_loss_disarms_nonzero_motion(self):
        bridge = self.bridge()
        self.arm_and_confirm(bridge)
        bridge.steering_hold_start(self.HOLD_TOKEN, 10)
        lost = self.steering_status(12, 102, active=True)
        lost["motor_enable_confirmed"] = 0
        lost["state"] = 4
        self.assertTrue(
            bridge._accept_steering_status_locked(lost, time.monotonic())
        )
        self.assertTrue(
            bridge._enforce_steering_safety_locked(time.monotonic())
        )
        self.assertEqual(bridge.state.control["steering_enable"], 0)

    def test_arm_without_ack_times_out_and_disarms(self):
        bridge = self.bridge()
        bridge.state.steering_status = self.steering_status()
        bridge.state.steering_received_at = time.monotonic()
        bridge.steering_arm(self.ARM_TOKEN)
        bridge.state.steering_arm_started_at = (
            time.monotonic() - UI.STEERING_ARM_ACK_TIMEOUT_S - 0.01
        )
        bridge.state.steering_received_at = time.monotonic()

        self.assertTrue(
            bridge._enforce_steering_safety_locked(time.monotonic())
        )
        self.assertEqual(bridge.state.control["steering_enable"], 0)
        self.assertFalse(bridge.state.steering_rate_mode)

    def test_armed_idle_ack_loss_disarms(self):
        bridge = self.bridge()
        self.arm_and_confirm(bridge)
        lost = self.steering_status(12, 102, active=True)
        lost["motor_enable_confirmed"] = 0
        lost["state"] = 4
        self.assertTrue(
            bridge._accept_steering_status_locked(lost, time.monotonic())
        )
        self.assertTrue(
            bridge._enforce_steering_safety_locked(time.monotonic())
        )
        self.assertEqual(
            bridge.state.control[UI.STEERING_VELOCITY_FIELD], 0
        )
        self.assertEqual(bridge.state.control["steering_enable"], 0)

    def test_manual_timeout_requires_new_disabled_state_and_new_ack(self):
        bridge = self.bridge()
        self.arm_and_confirm(bridge)
        bridge.steering_hold_start(self.HOLD_TOKEN, 10)
        bridge.state.steering_manual_deadline = time.monotonic() - 0.001
        self.assertTrue(
            bridge._enforce_steering_safety_locked(time.monotonic())
        )
        self.assertEqual(bridge.state.control["steering_enable"], 0)
        with self.assertRaisesRegex(ValueError, "disabled CAN2"):
            bridge.steering_arm("arm-token-00000002")
        with self.assertRaisesRegex(ValueError, "cancelled"):
            bridge.steering_hold_start(self.HOLD_TOKEN, 10)

        self.assertTrue(bridge._accept_steering_status_locked(
            self.steering_status(12, 102), time.monotonic()
        ))
        bridge.steering_arm("arm-token-00000002")
        with self.assertRaisesRegex(ValueError, "current-arm"):
            bridge.steering_hold_start("hold-token-0000002", 10)

    def test_steering_status_replay_and_control_input_types_fail_closed(self):
        bridge = self.bridge()
        first = self.steering_status(20, 200)
        self.assertTrue(bridge._accept_steering_status_locked(first, 10.0))
        self.assertFalse(bridge._accept_steering_status_locked(
            self.steering_status(19, 199), 10.1
        ))
        self.assertEqual(bridge.state.steering_status_sequence, 20)
        with self.assertRaisesRegex(ValueError, "integer"):
            bridge.patch_control({"buzzer_main_on": True})
        with self.assertRaisesRegex(ValueError, "integer"):
            bridge.configure({"sender_id": 1.5})
        self.assertEqual(bridge.state.sender_id, 2)

    def test_config_change_is_atomic_and_clears_remote_cache(self):
        bridge = self.bridge(enabled=False)
        bridge.state.status = {"old": 1}
        bridge.state.steering_status = self.steering_status()
        with self.assertRaises(ValueError):
            bridge.configure({"ecu_ip": "not-an-ip", "sender_id": 3})
        self.assertEqual(bridge.state.ecu_ip, "172.16.0.11")
        self.assertEqual(bridge.state.sender_id, 2)
        bridge.configure({"ecu_ip": "172.16.0.12"})
        self.assertIsNone(bridge.state.status)
        self.assertIsNone(bridge.state.steering_status)
        self.assertFalse(bridge.state.steering_status_sequence_valid)

    def test_local_http_context_json_and_csrf_are_strict(self):
        body = b'{"active":true}'
        handler = self.handler(body)
        handler._validate_request_context()
        self.assertEqual(handler._request_json(), {"active": True})

        bad_token = self.handler(body, X_ECU_UI_Token="wrong")
        with self.assertRaises(PermissionError):
            bad_token._request_json()
        bad_origin = self.handler(body, Origin="http://127.0.0.1:9999")
        with self.assertRaises(PermissionError):
            bad_origin._validate_request_context()
        bad_host = self.handler(body, Host="attacker.example:8088")
        with self.assertRaises(PermissionError):
            bad_host._validate_request_context()
        bad_type = self.handler(body, Content_Type="text/plain")
        with self.assertRaisesRegex(ValueError, "application/json"):
            bad_type._request_json()

        beacon_body = (
            b'{"active":false,"csrf_token":'
            b'"test-csrf-token-0000000000000000"}'
        )
        beacon_handler = self.handler(
            beacon_body, X_ECU_UI_Token=None
        )
        self.assertEqual(
            beacon_handler._request_json(), {"active": False}
        )

    def test_tuning_lease_only_exists_while_explicitly_active(self):
        bridge = self.bridge()
        bridge.set_tuning_session("test-client", True)
        self.assertTrue(bridge.snapshot()["tuning_active"])
        bridge.set_tuning_session("test-client", False)
        self.assertFalse(bridge.snapshot()["tuning_active"])

    def test_closed_tuning_token_cannot_be_resurrected_by_late_renewal(self):
        bridge = self.bridge()
        token = "one-time-tuning-token"
        bridge.set_tuning_session(token, True)
        bridge.set_tuning_session(token, False)
        bridge.set_tuning_session(token, True)
        self.assertFalse(bridge.snapshot()["tuning_active"])

    def test_telemetry_is_dropped_without_live_tuning_subscription(self):
        bridge = self.bridge()
        bridge.telemetry_subscribed = False
        bridge.telemetry_history = deque(maxlen=10)
        bridge.telemetry_generation = 0
        bridge.telemetry_condition = threading.Condition(bridge.lock)
        bridge.stop_event = threading.Event()
        payload = struct.pack(UI.TELEMETRY_BATCH_FORMAT, 1, 0, 1, 1000)
        payload += struct.pack(
            UI.TELEMETRY_SAMPLE_FORMAT,
            10, 100, 90, 80, 0, 50, 0, 1, 0,
        )
        bridge._receive_telemetry(payload, time.monotonic())
        self.assertEqual(bridge.telemetry_generation, 0)

        bridge.set_tuning_session("telemetry-token", True)
        bridge.telemetry_subscribed = True
        bridge._receive_telemetry(payload, time.monotonic())
        self.assertEqual(bridge.telemetry_generation, 1)
        self.assertEqual(bridge.telemetry_history[-1]["signed_feedback_ma"], 80)

    def test_expired_tuning_lease_emits_unsubscribe(self):
        class OneIterationEvent:
            stopped = False

            def is_set(self):
                return self.stopped

            def wait(self, timeout):
                del timeout
                self.stopped = True

        bridge = self.bridge(enabled=False)
        bridge.stop_event = OneIterationEvent()
        bridge.tuning_clients = {"expired": time.monotonic() - 1.0}
        bridge.telemetry_subscribed = True
        calls = []

        def unsubscribe():
            calls.append("unsubscribe")
            bridge.telemetry_subscribed = False

        bridge._send_unsubscribe = unsubscribe
        bridge._sender_loop()
        self.assertEqual(calls, ["unsubscribe"])
        self.assertFalse(bridge.telemetry_subscribed)

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
        self.assertIn("/api/session", html)
        self.assertIn("X-ECU-UI-Token", html)
        self.assertIn("/api/steering/arm", html)
        self.assertIn("/api/steering/disarm", html)
        self.assertIn("/api/steering/start", html)
        self.assertIn("/api/steering/stop", html)
        self.assertIn("tunePage').classList.contains('active')", html)
        self.assertIn("document.visibilityState==='visible'", html)
        self.assertIn("event.isPrimary===false||event.button!==0", html)
        self.assertIn("s.signed_feedback_ma??0", html)
        self.assertIn("p.state,p.fault_flags", html)
        self.assertNotIn("if(s.valve_state", html)
        self.assertNotIn("p.valve_state", html)
        self.assertIn("'安全归零中','安全失能中','使能发送中','等待使能 ACK','运行'", html)
        self.assertIn("已确认命令角速度（非反馈）", html)
        self.assertIn("严禁作为上位角闭环反馈", html)
        self.assertIn("['VEHICLE CAN',27]", html)
        self.assertIn("不是操作员身份认证", html)
        self.assertNotIn("throttle_percent", html)


if __name__ == "__main__":
    unittest.main()
