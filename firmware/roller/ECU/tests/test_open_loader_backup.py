import importlib.util
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "verify_open_loader_backup.py"
SPEC = importlib.util.spec_from_file_location("verify_open_loader_backup", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

PROJECT = MODULE_PATH.parents[1]
PINNED_RELEASE = PROJECT / "artifacts" / "firmware" / "1.0.15"
POST_OTA_FLASH = (
    PROJECT / "artifacts" / "device-backups" /
    "stm32h563-066BFF565456857187210935-20260830T060506Z-post-ota-1.0.15-audit" /
    "full-flash.bin"
)


def make_image(size, version=(1, 2, 3, 4), counter=9):
    image = bytearray(b"\xff" * size)
    header_size = 0x400
    image_size = 0x100
    protected_size = 12
    struct.pack_into("<IIHHII", image, 0, MODULE.IMAGE_MAGIC, 0,
                     header_size, protected_size, image_size, 0)
    struct.pack_into("<BBHI", image, 20, *version)
    protected = header_size + image_size
    struct.pack_into("<HH", image, protected, MODULE.PROTECTED_TLV_MAGIC,
                     protected_size)
    struct.pack_into("<HHI", image, protected + 4,
                     MODULE.SECURITY_COUNTER_TLV, 4, counter)
    image[-32] = 1
    image[-16:] = MODULE.BOOT_MAGIC
    return bytes(image)


class OpenLoaderBackupTests(unittest.TestCase):
    def test_geometry_is_loaded_from_canonical_shared_header(self):
        self.assertEqual(MODULE.FLASH_SIZE, 0x200000)
        self.assertEqual((MODULE.SECURE_OFFSET, MODULE.SECURE_SIZE),
                         (0x30000, 0x30000))
        self.assertEqual((MODULE.NONSECURE_OFFSET, MODULE.NONSECURE_SIZE),
                         (0x100000, 0x50000))

    def make_flash(self):
        flash = bytearray(b"\xff" * MODULE.FLASH_SIZE)
        secure = make_image(MODULE.SECURE_SIZE)
        nonsecure = make_image(MODULE.NONSECURE_SIZE)
        flash[MODULE.SECURE_OFFSET:MODULE.SECURE_OFFSET + MODULE.SECURE_SIZE] = secure
        flash[MODULE.NONSECURE_OFFSET:
              MODULE.NONSECURE_OFFSET + MODULE.NONSECURE_SIZE] = nonsecure
        return flash

    def test_accepts_matching_confirmed_pair(self):
        secure, nonsecure, *_ = MODULE.validate_flash(bytes(self.make_flash()))
        self.assertEqual(secure.identity, nonsecure.identity)
        self.assertEqual(secure.image_ok, 1)

    def test_rejects_release_identity_mismatch(self):
        flash = self.make_flash()
        replacement = make_image(MODULE.NONSECURE_SIZE, counter=10)
        flash[MODULE.NONSECURE_OFFSET:
              MODULE.NONSECURE_OFFSET + MODULE.NONSECURE_SIZE] = replacement
        with self.assertRaisesRegex(MODULE.BackupValidationError, "identities differ"):
            MODULE.validate_flash(bytes(flash))

    def test_rejects_half_confirmation(self):
        flash = self.make_flash()
        flash[MODULE.NONSECURE_OFFSET + MODULE.NONSECURE_SIZE - 32] = 0xFF
        with self.assertRaisesRegex(MODULE.BackupValidationError, "not exactly confirmed"):
            MODULE.validate_flash(bytes(flash))

    def test_rejects_bad_magic_and_malformed_tlv(self):
        for relative_offset in (0, 0x400 + 0x100):
            with self.subTest(relative_offset=relative_offset):
                flash = self.make_flash()
                flash[MODULE.SECURE_OFFSET + relative_offset] = 0
                with self.assertRaises(MODULE.BackupValidationError):
                    MODULE.validate_flash(bytes(flash))

    def test_rejects_noncanonical_image_ok_program_unit(self):
        flash = self.make_flash()
        flash[MODULE.SECURE_OFFSET + MODULE.SECURE_SIZE - 31] = 0
        with self.assertRaisesRegex(MODULE.BackupValidationError, "not exactly confirmed"):
            MODULE.validate_flash(bytes(flash))

    @unittest.skipUnless(PINNED_RELEASE.is_dir() and POST_OTA_FLASH.is_file(),
                         "reviewed 1.0.15/post-OTA audit artifacts unavailable")
    def test_exact_installed_mode_accepts_reviewed_post_swap_pair_only(self):
        flash = POST_OTA_FLASH.read_bytes()
        secure, nonsecure, *_ = MODULE.validate_installed_flash(
            flash, PINNED_RELEASE)
        self.assertEqual(secure.flags, MODULE.IMAGE_F_ENCRYPTED)
        self.assertEqual(secure.identity, nonsecure.identity)
        # The pre-loader path never treats the explicit installed format as a
        # generic encrypted-image exception.
        with self.assertRaisesRegex(MODULE.BackupValidationError,
                                    "encrypted primary cannot be audited"):
            MODULE.validate_flash(flash)

    @unittest.skipUnless(PINNED_RELEASE.is_dir() and POST_OTA_FLASH.is_file(),
                         "reviewed 1.0.15/post-OTA audit artifacts unavailable")
    def test_exact_installed_cli_is_release_named_and_emits_1_0_15_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "audit"
            result = subprocess.run(
                [sys.executable, str(MODULE_PATH),
                 "--flash", str(POST_OTA_FLASH),
                 "--output-dir", str(output),
                 "--installed-exact-1.0.15", str(PINNED_RELEASE)],
                check=False, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads((output / "primary-audit.json").read_text())
            self.assertEqual(report["schema"],
                             "roller-ecu-installed-primary-audit-v1")
            self.assertEqual(report["release_identity"], {
                "major": 1, "minor": 0, "revision": 15, "build": 0,
                "security_counter": 15,
            })

            obsolete = subprocess.run(
                [sys.executable, str(MODULE_PATH),
                 "--flash", str(POST_OTA_FLASH),
                 "--output-dir", str(output),
                 "--installed-exact-1.0.14", str(PINNED_RELEASE)],
                check=False, capture_output=True, text=True)
            self.assertNotEqual(obsolete.returncode, 0)
            self.assertIn("unrecognized arguments", obsolete.stderr)

    @unittest.skipUnless(PINNED_RELEASE.is_dir() and POST_OTA_FLASH.is_file(),
                         "reviewed 1.0.15/post-OTA audit artifacts unavailable")
    def test_installed_mode_rejects_ciphertext_or_modified_plaintext_payload(self):
        baseline = POST_OTA_FLASH.read_bytes()
        with zipfile.ZipFile(PINNED_RELEASE / "roller-ecu-1.0.15.recu") as archive:
            encrypted = archive.read("secure.bin")
        header_size = struct.unpack_from("<H", encrypted, 8)[0]
        image_size = struct.unpack_from("<I", encrypted, 12)[0]
        for mutation in ("ciphertext", "modified-plaintext"):
            with self.subTest(mutation=mutation):
                flash = bytearray(baseline)
                payload = MODULE.SECURE_OFFSET + header_size
                if mutation == "ciphertext":
                    flash[payload:payload + image_size] = \
                        encrypted[header_size:header_size + image_size]
                else:
                    flash[payload + 17] ^= 1
                with self.assertRaisesRegex(MODULE.BackupValidationError,
                                            "exact reviewed plaintext"):
                    MODULE.validate_installed_flash(bytes(flash), PINNED_RELEASE)

    @unittest.skipUnless(PINNED_RELEASE.is_dir() and POST_OTA_FLASH.is_file(),
                         "reviewed 1.0.15/post-OTA audit artifacts unavailable")
    def test_installed_mode_rejects_missing_enc_tlv_or_altered_trailer(self):
        baseline = POST_OTA_FLASH.read_bytes()
        for mutation in ("enc-tlv", "copy-done", "image-ok", "swap-status"):
            with self.subTest(mutation=mutation):
                flash = bytearray(baseline)
                slot = memoryview(flash)[MODULE.SECURE_OFFSET:
                                          MODULE.SECURE_OFFSET + MODULE.SECURE_SIZE]
                if mutation == "enc-tlv":
                    header_size, protected_size, image_size = \
                        struct.unpack_from("<HHI", slot, 8)
                    unprotected = header_size + image_size + protected_size
                    total = struct.unpack_from("<H", slot, unprotected + 2)[0]
                    cursor = unprotected + 4
                    while cursor < unprotected + total:
                        tlv_type, tlv_length = struct.unpack_from("<HH", slot, cursor)
                        if tlv_type == MODULE.ENCRYPTION_EC256_TLV:
                            struct.pack_into("<H", slot, cursor, 0x33)
                            break
                        cursor += 4 + tlv_length
                elif mutation == "copy-done":
                    slot[-MODULE.COPY_DONE_OFFSET_FROM_END] = 0xff
                elif mutation == "image-ok":
                    slot[-MODULE.IMAGE_OK_OFFSET_FROM_END] = 0xff
                else:
                    slot[-MODULE.BOOT_TRAILER_SIZE] = 0xff
                with self.assertRaises(MODULE.BackupValidationError):
                    MODULE.validate_installed_flash(bytes(flash), PINNED_RELEASE)

    @unittest.skipUnless(PINNED_RELEASE.is_dir() and POST_OTA_FLASH.is_file(),
                         "reviewed 1.0.15/post-OTA audit artifacts unavailable")
    def test_installed_mode_rejects_wrong_or_changed_release_reference(self):
        flash = POST_OTA_FLASH.read_bytes()
        with tempfile.TemporaryDirectory() as temporary:
            wrong = Path(temporary) / "1.0.15"
            shutil.copytree(PINNED_RELEASE, wrong)
            metadata = wrong / "metadata.json"
            data = bytearray(metadata.read_bytes())
            data[-2] ^= 1
            metadata.write_bytes(data)
            with self.assertRaisesRegex(MODULE.BackupValidationError,
                                        "not reviewed"):
                MODULE.validate_installed_flash(flash, wrong)


if __name__ == "__main__":
    unittest.main()
