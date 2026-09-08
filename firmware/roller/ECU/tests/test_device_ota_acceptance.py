import copy
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import accept_device_ota as acceptance


class DeviceOtaAcceptanceTests(unittest.TestCase):
    def test_state_checks_every_progress_and_sequence_field(self):
        status = dict(result=0, accepted_sequence=0, state=1,
                      secure_received=512, nonsecure_received=0)
        acceptance.require_state(status, accepted=0, state=1, secure_received=512)
        for name in status:
            changed = copy.copy(status)
            changed[name] += 1
            with self.subTest(name=name), self.assertRaises(RuntimeError):
                acceptance.require_state(changed, accepted=0, state=1, secure_received=512)

    def test_chunk_has_exact_target_crc_geometry_and_zero_reserved_bytes(self):
        data = bytes(range(256)) * 2
        frame = acceptance.chunk(data, offset=512)
        values = struct.unpack(acceptance.ota.CHUNK_FORMAT, frame)
        self.assertEqual(values, (22, 0, 512, 512, 0, acceptance.ota.crc32c(data), data))
        self.assertEqual(frame[5:8], bytes(3))
        bad = struct.unpack(acceptance.ota.CHUNK_FORMAT, acceptance.chunk(data, crc_error=True))
        self.assertEqual(bad[-2], acceptance.ota.crc32c(data) ^ 1)

    def test_estop_guard_failure_prevents_any_negative_request(self):
        client = MagicMock()
        with patch.object(acceptance.ota, "OtaSafetyMonitor") as monitor, \
                patch.object(acceptance.m, "write_json"):
            monitor.return_value.arm.side_effect = RuntimeError("E-stop missing")
            with self.assertRaises(RuntimeError):
                acceptance.partial(Path("unused"), client, bytes(192), bytes(512))
            client.request.assert_not_called()
            monitor.return_value.close.assert_called_once()

    def test_unexpected_bad_signature_acceptance_stops_before_valid_begin(self):
        client = MagicMock()
        client.request.return_value = dict(result=0)
        with patch.object(acceptance.ota, "OtaSafetyMonitor") as monitor, \
                patch.object(acceptance.m, "write_json"), \
                patch.object(acceptance.ota, "transfer") as transfer:
            monitor.return_value.latest = dict(requested_relay_mask=0, applied_relay_mask=0)
            with self.assertRaisesRegex(RuntimeError, "result mismatch"):
                acceptance.partial(Path("unused"), client, bytes(192), bytes(512))
            client.request.assert_called_once()
            transfer.assert_not_called()

    def test_failed_transfer_records_events_but_not_pass(self):
        with patch.object(acceptance.ota, "transfer", side_effect=RuntimeError("timeout")), \
                patch.object(acceptance.m, "write_json") as write:
            with self.assertRaises(RuntimeError):
                acceptance.transfer(Path("evidence"), MagicMock())
            self.assertEqual([call.args[0].name for call in write.call_args_list], ["transfer-events.json"])


if __name__ == "__main__":
    unittest.main()
