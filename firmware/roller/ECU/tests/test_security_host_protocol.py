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
    @staticmethod
    def ota_manifest_fields(*, build=0):
        return struct.unpack(
            OTA.MANIFEST_FORMAT,
            struct.pack(
                OTA.MANIFEST_FORMAT,
                0x31544F52, 1, 0x10000, 14,
                1, 0, 14, build, 14, 3,
                0x30000, 0x50000, bytes.fromhex("11" * 32),
                bytes.fromhex("22" * 32), 0, 0, 0, 0,
            ),
        )

    @staticmethod
    def ota_metadata(*, build=0):
        version = "1.0.14" if build == 0 else f"1.0.14+{build}"
        return {
            "format": "roller-ecu-ota-v1",
            "version": version,
            "security_counter": 14,
            "update_sequence": 14,
            "layout_version": 0x10000,
            "secure_sha256": "11" * 32,
            "nonsecure_sha256": "22" * 32,
            "secure_size": 0x30000,
            "nonsecure_size": 0x50000,
        }

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

    def test_ota_metadata_must_exactly_match_signed_manifest(self):
        OTA.validate_package_metadata(
            self.ota_manifest_fields(), self.ota_metadata())
        OTA.validate_package_metadata(
            self.ota_manifest_fields(build=7), self.ota_metadata(build=7))

        for name, value in (
                ("version", "1.0.15"),
                ("update_sequence", 15),
                ("security_counter", True),
                ("secure_sha256", "00" * 32)):
            with self.subTest(name=name):
                metadata = self.ota_metadata()
                metadata[name] = value
                with self.assertRaisesRegex(ValueError, "signed manifest"):
                    OTA.validate_package_metadata(
                        self.ota_manifest_fields(), metadata)

        metadata = self.ota_metadata()
        metadata["unsigned_note"] = "not allowed"
        with self.assertRaisesRegex(ValueError, "signed manifest schema"):
            OTA.validate_package_metadata(self.ota_manifest_fields(), metadata)


if __name__ == "__main__":
    unittest.main()
