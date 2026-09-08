"""Host-only gates for the SN-EJAHGJQ destructive recovery executor."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import recover_device_full_regression as recover


class RecoveryExecutorTests(unittest.TestCase):
    def test_exact_target_and_canonical_images(self):
        self.assertEqual(recover.m.SERIAL, "SN-EJAHGJQ")
        self.assertEqual(recover.m.TARGET["mcu_uid"], "003900443434511232383537")
        self.assertEqual(recover.m.TARGET["atecc_serial"], "01236acf4e275ef9ee")
        self.assertEqual(recover.m.TARGET["ecu_ip"], "172.16.0.21")
        self.assertEqual(recover.m.TARGET["domain_ip"], "172.16.0.22")
        self.assertEqual(recover.m.PROBE, "066BFF565456857187210935")
        opened = recover.m.regular_bytes(recover.INPUTS / "recovery-open-flash.bin")
        closed = recover.expected_closed_flash(recover.INPUTS)
        self.assertEqual(hashlib.sha256(opened).hexdigest(), recover.EXPECTED_OPEN_FLASH)
        self.assertEqual(hashlib.sha256(closed).hexdigest(), recover.EXPECTED_CLOSED_FLASH)

    def test_phase_journal_is_an_exact_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "phase.txt").write_text("transaction_prepared\n")
            self.assertEqual(recover.phases(root), recover.PHASES[:1])
            recover.phase(root, "closed_target_reverified")
            with self.assertRaises(RuntimeError):
                recover.phase(root, "closed_target_reverified")
            (root / "phase.txt").write_text(
                "transaction_prepared\nfull_regression_started\n")
            with self.assertRaises(RuntimeError):
                recover.phases(root)

    def test_regression_command_is_explicit_permission_a_path(self):
        command = recover.regression_command()
        self.assertIn("full-regression-to-open", command)
        self.assertIn("--accept-full-device-erase", command)
        self.assertIn("--accept-closed-target", command)
        self.assertEqual(command[command.index("--probe") + 1], recover.m.PROBE)

    def test_no_locked_or_partial_regression_writer(self):
        source = Path(recover.__file__).read_text()
        self.assertNotIn('"PRODUCT_STATE=0x5C"', source)
        self.assertNotIn("partial-regression", source)
        self.assertIn("partial_regression=False", source)
        self.assertIn('"PRODUCT_STATE=0x72"', source)

    def test_destructive_and_physical_actions_have_independent_gates(self):
        source = Path(recover.__file__).read_text()
        self.assertIn("args.accept_full_device_erase and args.accept_exact_closed_target", source)
        self.assertIn("args.accept_boot0_high_estop_disconnected", source)
        self.assertIn("args.accept_return_to_closed", source)
        self.assertIn("args.accept_exact_closed_target", source)

    def test_obkey_order_and_all_receipts_are_required(self):
        self.assertEqual(recover.OBKS, (
            ("da_config", "DA_Config.obk"),
            ("oemirot_config", "OEMiRoT_Config.obk"),
            ("oemirot_data", "OEMiRoT_Data.obk")))
        for stem, _ in recover.OBKS:
            self.assertLess(recover.PHASES.index(f"{stem}_submission_started"),
                            recover.PHASES.index(f"{stem}_provisioned"))

    def test_acceptance_is_after_closed_and_before_final_cold_runtime(self):
        phases = recover.PHASES
        self.assertLess(phases.index("product_state_closed_verified"),
                        phases.index("closed_da_accepted"))
        self.assertLess(phases.index("closed_da_accepted"),
                        phases.index("final_cold_runtime_accepted"))

    def test_regressed_open_parser_rejects_wrong_lifecycle_and_uid(self):
        good = recover.m.__dict__["TARGET"]
        text = ("STM32CubeProgrammer v2.23.0\nDevice ID : 0x484\n"
                "Revision ID : Rev X\nPRODUCT_STATE: 0xED (Open)\n"
                "0x08FFF800 : 00390044 34345112 32383537\n")
        self.assertEqual(good["mcu_uid"], "003900443434511232383537")
        recover.require_regressed_open(text)
        for changed in (text.replace("0xED", "0x72"),
                        text.replace("00390044", "00380061"),
                        text.replace("0x484", "0x483")):
            with self.assertRaises(RuntimeError):
                recover.require_regressed_open(changed)

    def test_real_completed_transaction_is_independently_reverified_when_present(self):
        root = recover.m.PROJECT / ("artifacts/device-backups/SN-EJAHGJQ/"
            "roller-ecu-SN-EJAHGJQ-full-regression-20260906T091744Z")
        if not (root / "full-regression-acceptance.json").is_file():
            self.skipTest("physical completed transaction is not in this checkout")
        report = recover.verify_completed(root)
        self.assertEqual(report["result"], "PASS")
        self.assertFalse(report["locked_state_used"])


if __name__ == "__main__":
    unittest.main()
