"""Offline recovery artifact rejection tests; no programmer or network calls."""
import copy
import json
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import prepare_device_recovery as recovery

FIXTURE = recovery.m.PROJECT / ("artifacts/device-backups/SN-EJAHGJQ/"
                                "roller-ecu-SN-EJAHGJQ-recovery-inputs-1.0.22-20260906")


class RecoverySchemaTests(unittest.TestCase):
    def manifest(self):
        return dict(schema=recovery.SCHEMA, target=copy.deepcopy(recovery.m.TARGET),
                    release=dict(version="1.0.22", counter=22, accepted_sequence=22),
                    readiness=copy.deepcopy(recovery.READINESS),
                    files={name: {} for name in recovery.FILES})

    def test_exact_input_schema_does_not_authorize_hardware(self):
        manifest = self.manifest()
        recovery.require_manifest(manifest)
        self.assertFalse(manifest["readiness"]["lifecycle_write_authorized"])
        self.assertFalse(manifest["readiness"]["hardware_full_regression_tested"])

    def test_wrong_identity_release_readiness_or_path_rejected(self):
        for group, field, value in (("target", "mcu_uid", "003800613434511232383537"),
                                    ("target", "ecu_ip", "172.16.0.11"),
                                    ("release", "counter", 21),
                                    ("readiness", "lifecycle_write_authorized", True),
                                    ("readiness", "hardware_full_regression_tested", True),
                                    ("files", "../../key.pem", {})):
            manifest = self.manifest()
            manifest[group][field] = value
            with self.subTest(group=group, field=field), self.assertRaises(RuntimeError):
                recovery.require_manifest(manifest)

    def test_unreviewed_source_fails_before_any_assembly(self):
        for source in (b"", b"\xff" * 0x200000):
            with self.assertRaises(RuntimeError):
                recovery.build_recovery_flash(source, b"", b"", b"")

    def test_no_hardware_writer_or_target_connection_in_this_tool(self):
        source = Path(recovery.__file__).read_text()
        for call in ("m.programmer(", "m.capture(", "subprocess.run(", "OtaClient(",
                     '"debugauth=1"', '"PRODUCT_STATE='):
            self.assertNotIn(call, source)


@unittest.skipUnless(FIXTURE.is_dir(), "optional reviewed recovery input archive unavailable")
class RecoveryArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ecu-recovery-audit-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "inputs"
        shutil.copytree(FIXTURE, self.root)

    def change(self, name, data):
        (self.root / name).write_bytes(data)
        manifest = json.loads((self.root / "manifest.json").read_text())
        manifest["files"][name] = dict(size=len(data), sha256=recovery.m.digest(data))
        (self.root / "manifest.json").write_text(json.dumps(manifest))

    def test_real_bundle_verifies_without_accessing_hardware(self):
        with patch.object(recovery.m, "programmer") as programmer, \
                patch.object(recovery.m, "capture") as capture, \
                patch.object(recovery.m.subprocess, "run") as run:
            report = recovery.verify(self.root)
            self.assertEqual(report["result"], "PASS")
            programmer.assert_not_called()
            capture.assert_not_called()
            run.assert_not_called()

    def test_canonical_image_is_initial_pair_not_old_swap_and_preserves_records(self):
        recovery.verify(self.root)
        image = (self.root / "recovery-open-flash.bin").read_bytes()
        source = (self.root / "source-open-flash.bin").read_bytes()
        self.assertEqual(image[0x20000:0x30000], b"\xff" * 0x10000)
        self.assertEqual(image[0x60000:0xE0000], b"\xff" * 0x80000)
        self.assertEqual(image[0xE0000:0x100000], source[0xE0000:0x100000])
        self.assertEqual(image[0x150000:], b"\xff" * 0xB0000)
        for offset in (0x30000, 0x100000):
            self.assertEqual(struct.unpack_from("<I", image, offset + 16)[0], 0)
            self.assertEqual(struct.unpack_from("<I", source, offset + 16)[0], 4)

    def test_rehashed_wrong_source_is_not_trusted(self):
        image = bytearray((self.root / "source-open-flash.bin").read_bytes())
        image[0x20000] ^= 1
        self.change("source-open-flash.bin", image)
        with self.assertRaisesRegex(RuntimeError, "exact reviewed"):
            recovery.verify(self.root)

    def test_rehashed_stale_secondary_is_not_installable(self):
        image = bytearray((self.root / "recovery-open-flash.bin").read_bytes())
        image[0x60000] = 0
        self.change("recovery-open-flash.bin", image)
        with self.assertRaisesRegex(RuntimeError, "noncanonical"):
            recovery.verify(self.root)

    def test_rehashed_wrong_atecc_sector_is_not_accepted(self):
        sector = bytearray((self.root / "pairing-store.bin").read_bytes())
        sector[48] ^= 1
        self.change("pairing-store.bin", sector)
        with self.assertRaisesRegex(RuntimeError, "extraction"):
            recovery.verify(self.root)

    def test_rehashed_changed_da_policy_is_not_trusted(self):
        obk = bytearray((self.root / "DA_Config.obk").read_bytes())
        obk[76] ^= 1
        self.change("DA_Config.obk", obk)
        with self.assertRaisesRegex(RuntimeError, "unreviewed recovery support"):
            recovery.verify(self.root)

    def test_changed_watchdog_or_saved_parameter_state_is_rejected(self):
        name = "source-option-bytes-and-uid.log"
        original = (self.root / name).read_text()
        changed = re.sub(r"(IWDG_SW\s*:\s*)0x1", r"\g<1>0x0", original)
        self.assertNotEqual(changed, original)
        self.change(name, changed.encode())
        with self.assertRaisesRegex(RuntimeError, "IWDG_SW"):
            recovery.verify(self.root)
        self.change(name, original.encode())
        name = "source-runtime-pinless.json"
        state = json.loads((self.root / name).read_text())
        state["valve_config"]["persisted_generation"] = 1
        self.change(name, json.dumps(state).encode())
        with self.assertRaisesRegex(RuntimeError, "PI state"):
            recovery.verify(self.root)

    def test_added_file_and_symlink_are_rejected(self):
        extra = self.root / "unexpected.txt"
        extra.write_text("not part of the sealed inputs")
        with self.assertRaisesRegex(RuntimeError, "extraneous"):
            recovery.verify(self.root)
        extra.unlink()
        extra.symlink_to(self.root / "manifest.json")
        with self.assertRaisesRegex(RuntimeError, "symlink"):
            recovery.verify(self.root)


if __name__ == "__main__":
    unittest.main()
