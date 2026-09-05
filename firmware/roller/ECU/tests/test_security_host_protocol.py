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
    def test_vendored_public_key_verifies_current_release(self):
        package = (
            PROJECT_ROOT / "artifacts" / "firmware" / "1.0.20" /
            "roller-ecu-1.0.20.recu"
        )
        if not package.is_file():
            self.skipTest("reviewed 1.0.20 package unavailable")
        self.assertEqual(
            OTA.DEFAULT_PUBLIC_KEY,
            PROJECT_ROOT / "tools" / "ota-transport-public.pem",
        )
        _begin, secure, nonsecure, metadata = OTA.load_package(
            package, OTA.DEFAULT_PUBLIC_KEY
        )
        self.assertEqual(metadata["version"], "1.0.20")
        self.assertEqual((len(secure), len(nonsecure)), (0x30000, 0x50000))

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

    def test_ota_physical_estop_diagnostic_is_strictly_decoded(self):
        values = [
            OTA.ECU_CAP_LATCHED_ESTOP_RESET,
            OTA.SAFETY_STATUS_PHYSICAL_ESTOP |
            OTA.SAFETY_STATUS_ATECC_AUTHENTICATED,
            0,
            0,
            1234,
            *([0] * 7),
            *([0] * 12),
            0,
            *([0] * 5),
            0,
            0,
        ]
        payload = struct.pack(OTA.DIAGNOSTIC_FORMAT, *values)
        frame = OTA.encode_v2(OTA.MSG_DIAGNOSTIC, 7, payload)
        diagnostic = OTA.decode_diagnostic(frame)
        self.assertEqual(
            diagnostic["capability_flags"],
            OTA.ECU_CAP_LATCHED_ESTOP_RESET,
        )
        self.assertTrue(
            diagnostic["safety_status"] &
            OTA.SAFETY_STATUS_PHYSICAL_ESTOP
        )
        self.assertEqual(diagnostic["secure_uptime_ms"], 1234)

        corrupt = bytearray(frame)
        corrupt[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC"):
            OTA.decode_diagnostic(bytes(corrupt))

    def test_1_0_20_and_later_require_post_swap_safety_capability(self):
        self.assertFalse(
            OTA.release_requires_latched_estop_capability("1.0.18")
        )
        self.assertFalse(
            OTA.release_requires_latched_estop_capability("1.0.19")
        )
        self.assertTrue(
            OTA.release_requires_latched_estop_capability("1.0.20")
        )
        self.assertTrue(
            OTA.release_requires_latched_estop_capability("1.1.0+4")
        )
        with self.assertRaisesRegex(ValueError, "release version"):
            OTA.release_requires_latched_estop_capability("release-19")

    def test_ota_monitor_provider_records_each_validated_observation_once(self):
        observation = [{
            "capability_flags": OTA.ECU_CAP_LATCHED_ESTOP_RESET,
            "safety_status": OTA.SAFETY_STATUS_PHYSICAL_ESTOP,
            "requested_relay_mask": 0,
            "applied_relay_mask": 0,
            "secure_uptime_ms": 100,
        }, 10.0]

        def provider():
            return dict(observation[0]), observation[1]

        monitor = OTA.OtaSafetyMonitor("172.16.0.11", provider)
        try:
            self.assertIsNone(monitor.socket)
            monitor.drain()
            self.assertEqual(monitor.generation, 1)
            monitor.drain()
            self.assertEqual(monitor.generation, 1)
            monitor.armed = True
            observation[0]["safety_status"] = 0
            observation[1] = 10.1
            monitor.drain()
            self.assertEqual(monitor.generation, 2)
            self.assertTrue(monitor.release_observed)
        finally:
            monitor.close()

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
