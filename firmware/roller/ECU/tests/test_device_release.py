"""Readback-audit tests; synthetic signatures need no production private keys."""
import hashlib
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import verify_device_release as audit


class DeviceReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.key = ec.generate_private_key(ec.SECP256R1())
        cls.public = cls.key.public_key().public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)

    def image(self, *, encrypted=False, counter=22, dependency=1, key=None):
        key = key or self.key
        image = bytearray(b"\xff" * 0x30000)
        struct.pack_into("<IIHHII", image, 0, audit.layout.IMAGE_MAGIC, 0,
                         0x400, 28, 0x1000, 4 if encrypted else 0)
        struct.pack_into("<BBHI", image, 20, 1, 0, 22, 0)
        image[0x400:0x1400] = bytes(range(256)) * 16
        protected = (struct.pack("<HHHHIHH", 0x6908, 28, 0x50, 4, counter, 0x40, 12) +
                     struct.pack("<B3xBBHI", dependency, 1, 0, 22, 0))
        image[0x1400:0x141c] = protected
        der = key.public_key().public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
        fields = [(0x10, hashlib.sha256(image[:0x141c]).digest()),
                  (0x01, hashlib.sha256(der).digest()),
                  (0x22, key.sign(bytes(image[:0x141c]), ec.ECDSA(hashes.SHA256())))]
        if encrypted:
            fields.append((0x32, bytes(113)))
        contents = b"".join(struct.pack("<HH", tag, len(value)) + value for tag, value in fields)
        image[0x141c:0x1420 + len(contents)] = struct.pack("<HH", 0x6907, len(contents) + 4) + contents
        image[-32:-16] = b"\x01" + b"\xff" * 15
        image[-16:] = audit.layout.BOOT_MAGIC
        return image

    def verify(self, image, *, encrypted=False):
        return audit.verify_record(bytes(image), "secure", "1.0.22", self.public, encrypted=encrypted)

    def test_plaintext_and_decrypted_update_signatures(self):
        for encrypted in (False, True):
            self.assertEqual(self.verify(self.image(encrypted=encrypted), encrypted=encrypted)["flags"],
                             4 if encrypted else 0)

    def test_payload_corruption_rejected_even_with_recomputed_hash(self):
        image = self.image()
        image[0x400] ^= 1
        with self.assertRaisesRegex(RuntimeError, "hash mismatch"):
            self.verify(image)
        image[0x1424:0x1444] = hashlib.sha256(image[:0x141c]).digest()
        with self.assertRaises(InvalidSignature):
            self.verify(image)

    def test_other_valid_signing_key_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "public-key hash"):
            self.verify(self.image(key=ec.generate_private_key(ec.SECP256R1())))

    def test_signed_wrong_dependency_or_counter_rejected(self):
        for kwargs in (dict(counter=21), dict(dependency=0)):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(RuntimeError, "counter/dependency"):
                self.verify(self.image(**kwargs))

    def test_version_flags_size_and_truncated_tlv_rejected(self):
        for offset, value in ((20, 2), (16, 4), (8, 1), (0x141e, 0xff), (0x1422, 0xff)):
            image = self.image()
            image[offset] = value
            with self.subTest(offset=offset), self.assertRaises(RuntimeError):
                self.verify(image)
        with self.assertRaises(RuntimeError):
            self.verify(self.image()[:-1])

    def test_duplicate_tlv_rejected(self):
        fields = b"".join(struct.pack("<HHI", 0x50, 4, 22) for _ in range(2))
        with self.assertRaisesRegex(RuntimeError, "duplicate"):
            audit.tlvs(struct.pack("<HH", 0x6908, 20) + fields, 0, 0x6908)

    def installed(self):
        image = self.image(encrypted=True)
        record_size = self.verify(image, encrypted=True)["record_size"]
        trailer = len(image) - audit.layout.BOOT_TRAILER_SIZE
        for entry in range(3):
            image[trailer + entry * 16:trailer + (entry + 1) * 16] = bytes([entry + 1]) + b"\xff" * 15
        for offset, data in ((80, struct.pack("<I", record_size) + b"\xff" * 12),
                             (64, b"\x02" + b"\xff" * 15), (48, b"\x01" + b"\xff" * 15)):
            image[-offset:-offset + 16] = data
        return image, record_size

    def test_confirmed_trailer_and_absent_or_present_rollback_keys(self):
        image, size = self.installed()
        self.assertEqual(audit.audit_installed_trailer(image, size, 0)["swap_key_slots_present"], 0)
        image[-112:-80] = bytes(range(32))
        self.assertEqual(audit.audit_installed_trailer(image, size, 0)["swap_key_slots_present"], 2)

    def test_unconfirmed_incomplete_swap_wrong_index_or_gap_rejected(self):
        for offset in (-32, -48, -64, -80, -1, -audit.layout.BOOT_TRAILER_SIZE, 0x2000):
            image, size = self.installed()
            image[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(RuntimeError):
                audit.audit_installed_trailer(image, size, 0)
        image, size = self.installed()
        with self.assertRaises(RuntimeError):
            audit.audit_installed_trailer(image, size, 1)

    def test_unknown_release_and_wrong_boot_fail_before_pairing_or_keys(self):
        with self.assertRaises(RuntimeError):
            audit.load_release(Path("unreviewed/1.0.20"), Path("unused"))
        with patch.object(audit, "verify_dual_store") as pairing:
            with self.assertRaisesRegex(RuntimeError, "bootloader"):
                audit.audit_flash(b"\xff" * 0x200000, Path("1.0.22"), Path("unused"),
                                  "installed", "ReleaseOpen")
            pairing.assert_not_called()

    def test_reviewed_real_releases_if_available(self):
        project, keys = audit.manufacturing.PROJECT, audit.manufacturing.PKI
        if not all((project / "artifacts/firmware" / v / "metadata.json").exists()
                   for v in audit.RELEASES) or not keys.is_dir():
            self.skipTest("optional immutable release/public-key archives unavailable")
        for version in audit.RELEASES:
            result = audit.load_release(project / "artifacts/firmware" / version, keys)
            self.assertEqual(set(result), {"secure", "nonsecure"})


if __name__ == "__main__":
    unittest.main()
