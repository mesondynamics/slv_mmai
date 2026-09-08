"""Hardware-free rejection tests for the new-board OPEN manufacturing gates."""
import copy
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import manufacture_open_device as manufacture
import ecu_readonly_snapshot as snapshot


class ManufacturingGuardsTests(unittest.TestCase):
    OPTIONS = """STM32CubeProgrammer v2.23.0
Device ID : 0x484
Revision ID : Rev X
PRODUCT_STATE: 0xED (Open)
TZEN: 0xB4
BOOT_UBE: 0xB4
IWDG_SW: 0x1
SWAP_BANK: 0x0
EDATA1_EN: 0x0
EDATA2_EN: 0x0
0x08FFF800 : 00390044 34345112 32383537
"""

    def test_exact_open_options_and_uid(self):
        manufacture.require_open_options(self.OPTIONS)
        manufacture.require_programmer_uid(self.OPTIONS)

    @classmethod
    def rss_options(cls):
        fields = dict(SECBOOT_LOCK=0xB4, SECBOOTADD=0xC0000,
                      SECWM1_STRT=0, SECWM1_END=0x7F, SECWM2_STRT=1, SECWM2_END=0,
                      WRPSGn1=0xFFFFFFF0, WRPSGn2=0xFFFFFFFF,
                      HDP1_STRT=0, HDP1_END=0x17, HDP2_STRT=1, HDP2_END=0,
                      SRAM2_RST=0, SRAM2_ECC=0)
        return (cls.OPTIONS + "BL Version : 0xE4\nSFSP Version: v2.5.0\n" +
                "".join(f"{name}: 0x{value:X}\n" for name, value in fields.items()))

    def test_rss_requires_exact_versions_uid_and_installed_protections(self):
        text = self.rss_options()
        manufacture.require_rss_options(text)
        for old, new in (("0xED", "0x17"), ("0xE4", "0xE3"),
                         ("v2.5.0", "v2.4.0"), ("00390044", "00380061"),
                         ("WRPSGn1: 0xFFFFFFF0", "WRPSGn1: 0xFFFFFFFF"),
                         ("HDP1_END: 0x17", "HDP1_END: 0x0"),
                         ("SECBOOT_LOCK: 0xB4", "SECBOOT_LOCK: 0xC3"),
                         ("SRAM2_ECC: 0x0", "SRAM2_ECC: 0x1")):
            with self.subTest(old=old), self.assertRaises(RuntimeError):
                manufacture.require_rss_options(text.replace(old, new))

    def test_obkey_receipt_requires_unambiguous_positive_evidence(self):
        good = "OBKey Provisioned successfully\n"
        manufacture.require_obkey_success(good)
        for text in ("", "Connected successfully\n", good + good,
                     good + "Error: transport failed\n", "Not " + good):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                manufacture.require_obkey_success(text)

    def test_obkey_journal_enforces_exact_order_and_torn_write_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(RuntimeError):
                manufacture.append_obk_phase(root, manufacture.OBK_PHASES[1])
            for name in manufacture.OBK_PHASES:
                manufacture.append_obk_phase(root, name)
            with self.assertRaises(RuntimeError):
                manufacture.append_obk_phase(root, manufacture.OBK_PHASES[-1])
            path = root / "obkeys-phase.txt"
            path.write_text(manufacture.OBK_PHASES[0])
            with self.assertRaises(RuntimeError):
                manufacture.append_obk_phase(root, manufacture.OBK_PHASES[1])

    def test_existing_operation_log_stops_before_programmer_is_spawned(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "da-sdp.log").write_text("previous operation")
            with patch.object(manufacture.subprocess, "run") as run, self.assertRaises(RuntimeError):
                manufacture.programmer(root, "da-sdp", ["-sdp", "unused"])
            run.assert_not_called()

    def test_wrong_rss_identity_is_not_retried_or_written(self):
        with patch.object(manufacture, "programmer", return_value=self.rss_options().replace(
                "00390044", "00380061")) as programmer, self.assertRaises(RuntimeError):
            manufacture.wait_open_rss(Path("unused"), "test")
        programmer.assert_called_once()

    def test_closed_locked_or_unsafe_watchdog_and_boot_are_rejected(self):
        for old, new in (("0xED", "0x72"), ("0xED", "0x5C"),
                         ("TZEN: 0xB4", "TZEN: 0xC3"),
                         ("IWDG_SW: 0x1", "IWDG_SW: 0x0"),
                         ("BOOT_UBE: 0xB4", "BOOT_UBE: 0xC3"),
                         ("EDATA1_EN: 0x0", "EDATA1_EN: 0x1"),
                         ("EDATA2_EN: 0x0", "EDATA2_EN: 0x1"),
                         ("0x484", "0x483"), ("Rev X", "Rev Z"),
                         ("v2.23.0", "v2.22.0")):
            with self.subTest(old=old, new=new), self.assertRaises(RuntimeError):
                manufacture.require_open_options(self.OPTIONS.replace(old, new))

    def test_missing_duplicate_or_old_uid_rejected(self):
        for text in ("", self.OPTIONS.replace("00390044", "00380061"),
                     self.OPTIONS + "0x08FFF800 : 00390044 34345112 32383537\n"):
            with self.assertRaises(RuntimeError):
                manufacture.require_programmer_uid(text)
        with self.assertRaises(RuntimeError):
            manufacture.require_open_options(self.OPTIONS + "PRODUCT_STATE: 0xED\n")

    def test_new_pairing_identity_is_independent_of_legacy_default(self):
        self.assertEqual(manufacture.IDENTITY.serial.hex(), "01236acf4e275ef9ee")
        self.assertEqual(manufacture.TARGET["mcu_uid"], "003900443434511232383537")

    def test_no_closed_or_da_regression_operation_implemented(self):
        text = Path(manufacture.__file__).read_text()
        self.assertNotIn('"PRODUCT_STATE=', text)
        self.assertNotIn('"debugauth=1"', text)
        self.assertNotIn('"per=a"', text)

    def test_unreviewed_artifacts_cannot_be_installed(self):
        with patch.object(manufacture, "verify_staged", side_effect=RuntimeError("bad hash")), \
                patch.object(manufacture, "programmer") as programmer:
            with self.assertRaises(RuntimeError):
                manufacture.install(Path("unused"))
            programmer.assert_not_called()

    def test_snapshot_readonly_safety_rejections(self):
        state = dict(security=dict(
            mcu_uid=manufacture.TARGET["mcu_uid"], serial="01236acf4e275ef9ee",
            config_crc32c=0x6165A970, auth_result=0, pairing_generation=1,
            atecc_result=0, config_locked=1, data_locked=1, revision="00006005"),
            diagnostic=dict(safety_status=snapshot.ui.SAFETY_STATUS_PHYSICAL_ESTOP,
                            requested_relay_mask=0, applied_relay_mask=0, valve_fault_flags=0),
            status=dict(forward_duty_permille=0, reverse_duty_permille=0,
                        valve_requested_target_ma=0, valve_applied_target_ma=0,
                        forward_current_ma=0, reverse_current_ma=0),
            steering_status=dict(command_enable=0, speed_command_permille=0),
            ota=dict(result=0, state=0, accepted_sequence=0))
        snapshot.require_paired_safe(state, "SN-EJAHGJQ", accepted_sequence=0)
        for group, field, bad in (("diagnostic", "safety_status", 0),
                                  ("diagnostic", "applied_relay_mask", 1),
                                  ("status", "forward_duty_permille", 1),
                                  ("status", "reverse_current_ma", 50),
                                  ("steering_status", "command_enable", 1),
                                  ("security", "mcu_uid", "003800613434511232383537"),
                                  ("ota", "accepted_sequence", 21)):
            other = copy.deepcopy(state)
            other[group][field] = bad
            with self.subTest(field=field), self.assertRaises(RuntimeError):
                snapshot.require_paired_safe(other, "SN-EJAHGJQ", accepted_sequence=0)


if __name__ == "__main__":
    unittest.main()
