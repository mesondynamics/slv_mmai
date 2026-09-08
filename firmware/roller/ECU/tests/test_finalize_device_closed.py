"""New-board CLOSED transaction gates; hardware commands are mocked."""
import copy
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import finalize_device_closed as closed


class ClosedStaticTests(unittest.TestCase):
    def test_exact_new_target_release_and_reversible_policy(self):
        self.assertEqual(closed.m.SERIAL, "SN-EJAHGJQ")
        self.assertEqual(closed.m.TARGET["mcu_uid"], "003900443434511232383537")
        self.assertEqual(closed.m.TARGET["ecu_ip"], "172.16.0.21")
        self.assertEqual(closed.m.TARGET["permission_mask"], "0x00004040")
        self.assertEqual(closed.m.PINNED["ReleaseClosed-ECU_OEMiROT.bin"],
                         (52292, "135f5b4e07e5e1ebe714d247be711ae217231a953dbf977b99dad9cfb2914c10"))

    def test_writer_has_no_obkey_submission_locked_or_regression_command(self):
        source = Path(closed.__file__).read_text()
        self.assertNotIn('"-sdp"', source)
        self.assertNotIn('"PRODUCT_STATE=0x5C"', source)
        self.assertNotIn('"debugauth=1"', source)
        self.assertIn("existing_open_obkeys_only=True", source)
        self.assertIn("full_regression_separate_transaction=True", source)

    def test_phase_journal_is_strict_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "phase.txt").write_text("transaction_prepared\n")
            self.assertEqual(closed.phases(root), closed.PHASES[:1])
            (root / "phase.txt").write_text("transaction_prepared\nloader_write_started\n")
            with self.assertRaises(RuntimeError):
                closed.phases(root)

    def test_option_parser_rejects_wrong_lifecycle_or_protection(self):
        text = (closed.OPEN_PREFLIGHT / "01-fresh-open-identity.log").read_text()
        closed.require_protections(text, 0xED)
        for changed in (text.replace("PRODUCT_STATE: 0xED", "PRODUCT_STATE: 0x72"),
                        text.replace("IWDG_SW      : 0x1", "IWDG_SW      : 0x0"),
                        text.replace("LOCKBL       : 0x0", "LOCKBL       : 0x1")):
            with self.assertRaises(RuntimeError):
                closed.require_protections(changed, 0xED)

    def test_actual_provisioning_view_masks_only_secure_boot_fields(self):
        text = (closed.m.PROJECT / "artifacts/device-backups/"
                "stm32h563-066BFF565456857187210935-20260830T063129Z-closed-transition/"
                "provisioning-rss-last.txt").read_text()
        closed.require_protections(text, 0x17)
        for changed in (text.replace("WRPSGn1      : 0xFFFFFFF0", "WRPSGn1      : 0xFFFFFFFF"),
                        text.replace("IWDG_SW      : 0x1", "IWDG_SW      : 0x0"),
                        text.replace("PRODUCT_STATE: 0x17", "PRODUCT_STATE: 0xED")):
            with self.assertRaises(RuntimeError):
                closed.require_protections(changed, 0x17)

    def test_strict_discovery_requires_all_authoritative_fields(self):
        original = (closed.m.PROJECT / "artifacts/device-backups/"
                    "stm32h563-066BFF565456857187210935-20260830T063129Z-closed-transition/"
                    "closed-da-discovery.log").read_text()
        closed.common_discovery(original, "ST_LIFECYCLE_CLOSED")
        for old, new in (("0xEAEAEAEA", "0x00000000"), ("VALID", "INVALID"),
                         ("ST_LIFECYCLE_CLOSED", "ST_LIFECYCLE_PROVISIONING"),
                         ("2.4.0", "2.3.0")):
            with self.subTest(field=old), self.assertRaises(RuntimeError):
                closed.common_discovery(original.replace(old, new), "ST_LIFECYCLE_CLOSED")


@unittest.skipUnless(closed.RECOVERY.is_dir(), "new-board recovery inputs unavailable")
class PreparedTransactionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ecu-closed-manifest-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "roller-ecu-SN-EJAHGJQ-closed-fixture"
        immutable = self.root / "immutable"
        capture = immutable / "capture"
        capture.mkdir(parents=True)
        shutil.copytree(closed.RECOVERY, immutable / "recovery-inputs")
        source = closed.m.regular_bytes(closed.RECOVERY / "source-open-flash.bin")
        (capture / "full-flash-before.bin").write_bytes(source)
        (capture / "persistent-before.bin").write_bytes(source[0xE0000:0x100000])
        shutil.copy2(closed.OPEN_PREFLIGHT / "01-fresh-open-identity.log",
                     capture / "options-and-uid.log")
        state = closed.json_read(closed.OPEN_PREFLIGHT / "runtime-final.json")
        for name in ("runtime-before.json", "runtime-after-readback.json"):
            (immutable / name).write_text(json.dumps(state))
        proof = dict(result="PASS", target=closed.m.TARGET,
                     manufacturing_manifest_sha256=closed.m.digest(closed.m.regular_bytes(
                         closed.MANUFACTURING / "manifest.json")),
                     obkeys={name: closed.m.PINNED_OBKS[name][1] for name in closed.OBKEYS})
        (immutable / "open-obkey-proof.json").write_text(json.dumps(proof))
        closed.write_manifest(self.root)
        (self.root / "phase.txt").write_text("transaction_prepared\n")

    def test_exact_fixture_verifies(self):
        manifest = closed.verify_transaction(self.root)
        self.assertEqual(manifest["target"], closed.m.TARGET)
        self.assertFalse(manifest["policy"]["obkey_resubmission"])
        self.assertTrue(manifest["policy"]["locked_forbidden"])

    def test_rehashed_wrong_target_still_rejected(self):
        path = self.root / "manifest.json"
        manifest = json.loads(path.read_text())
        manifest["target"]["mcu_uid"] = "003800613434511232383537"
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(RuntimeError, "identity"):
            closed.verify_transaction(self.root)

    def test_modified_recovery_or_runtime_is_rejected(self):
        for relative in ("recovery-inputs/recovery-open-flash.bin", "runtime-before.json"):
            with self.subTest(relative=relative):
                self.setUp()
                path = self.root / "immutable" / relative
                data = bytearray(path.read_bytes())
                data[-1] ^= 1
                path.write_bytes(data)
                manifest = json.loads((self.root / "manifest.json").read_text())
                manifest["files"][relative] = dict(size=len(data), sha256=closed.m.digest(data))
                (self.root / "manifest.json").write_text(json.dumps(manifest))
                with self.assertRaises((RuntimeError, json.JSONDecodeError, UnicodeDecodeError)):
                    closed.verify_transaction(self.root)

    def test_symlink_input_rejected(self):
        link = self.root / "immutable/bad-link"
        link.symlink_to(closed.RECOVERY / "manifest.json")
        with self.assertRaisesRegex(RuntimeError, "symlink"):
            closed.verify_transaction(self.root)


if __name__ == "__main__":
    unittest.main()
