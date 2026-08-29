import importlib.util
from pathlib import Path
import struct
import sys
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def load_tool(name: str):
    path = PROJECT_ROOT / "tools" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


FACTORY = load_tool("factory_provision_atecc")
OTA = load_tool("ethernet_ota")


class SecurityHostProtocolTest(unittest.TestCase):
    def test_factory_matches_echoed_request_not_ecu_header_sequence(self):
        request_sequence = 0x12345678
        payload = FACTORY.STATUS.pack(
            0, request_sequence, 0, 0, 0, 0, 0, 0xFFFF,
            0, 0, 0, 2, bytes(9), bytes(4), bytes(128), bytes(64))
        frame = FACTORY.encode_frame(
            FACTORY.ECU_FACTORY_STATUS, 0xA5A5A5A5, payload)
        self.assertEqual(FACTORY.decode_frame(frame, request_sequence), payload)
        with self.assertRaisesRegex(RuntimeError, "request sequence"):
            FACTORY.decode_frame(frame, request_sequence + 1)

    def test_ota_matches_echoed_request_not_ecu_header_sequence(self):
        request_sequence = 0x87654321
        payload = struct.pack(OTA.STATUS_FORMAT, 0, request_sequence,
                              0x00010000, 0, 0, 1, 0, 0, 0, 0, 0)
        frame = OTA.encode_v2(OTA.MSG_STATUS, 99, payload)
        status = OTA.decode_status(frame, request_sequence)
        self.assertEqual(status["request_sequence"], request_sequence)
        with self.assertRaisesRegex(ValueError, "request sequence"):
            OTA.decode_status(frame, request_sequence + 1)


if __name__ == "__main__":
    unittest.main()
