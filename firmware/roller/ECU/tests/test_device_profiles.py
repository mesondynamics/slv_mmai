import importlib.util
from pathlib import Path
import subprocess
import unittest

PROJECT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_device_profile",
                                            PROJECT / "tools/check_device_profile.py")
PROFILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILE)


class DeviceProfilesTests(unittest.TestCase):
    def test_legacy_profile_retains_deployed_addresses_and_uid(self):
        self.assertEqual(PROFILE.read_profile(1)["serial"], "SN-EJAHGJI")

    def test_new_profile_pins_directly_read_mcu_and_network(self):
        p = PROFILE.read_profile(2)
        self.assertEqual(p["ecu_ip"], "172.16.0.21")
        self.assertEqual(p["domain_ip"], "172.16.0.22")
        self.assertEqual(p["mcu_uid"], "003900443434511232383537")

    def test_macs_are_unique_local_unicast(self):
        a, b = PROFILE.read_profile(1), PROFILE.read_profile(2)
        self.assertNotEqual(a["mac"], b["mac"])
        for p in (a, b):
            self.assertEqual(int(p["mac"][:2], 16) & 3, 2)

    def test_unknown_device_fails_compile(self):
        with self.assertRaises(subprocess.CalledProcessError):
            PROFILE.read_profile(3)

    def test_selected_project_profile_is_reviewed(self):
        self.assertIn(PROFILE.read_profile()["device_id"], PROFILE.EXPECTED)

    def test_mac_override_is_regeneration_safe_and_precedes_hal(self):
        source = (PROJECT / "NonSecure/Core/Src/eth.c").read_text()
        marker = source.index("/* USER CODE BEGIN MACADDRESS */")
        end = source.index("/* USER CODE END MACADDRESS */")
        self.assertIn("MACAddr[5] = ECU_DEVICE_MAC_OCTET5;", source[marker:end])
        self.assertLess(end, source.index("HAL_ETH_Init(&heth)"))

    def test_secure_handoff_rejects_other_device_release(self):
        source = (PROJECT / "Secure/App/Src/security_mcu_identity.c").read_text()
        for i in range(3):
            self.assertIn(f"handoff->mcu_uid[{i}] != ECU_DEVICE_UID{i}", source)


if __name__ == "__main__":
    unittest.main()
