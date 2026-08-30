import importlib.util
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path


MODULE_PATH = (
    Path(__file__).resolve().parents[1] /
    "tools" / "verify_closed_ota_readback.py"
)
SPEC = importlib.util.spec_from_file_location("verify_closed_ota_readback", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

PROJECT = MODULE_PATH.parents[1]
PINNED_RELEASE = PROJECT / "artifacts" / "firmware" / "1.0.16"
PINNED_RELEASE_1_0_17 = PROJECT / "artifacts" / "firmware" / "1.0.17"
PKI_DIR = Path("/home/plac/.local/share/roller-ecu-pki")


def make_reference_image(size, *, encrypted, image_size, seed,
                         identity=(1, 0, 16, 0, 16)):
    image = bytearray(b"\xff" * size)
    flags = MODULE.IMAGE_F_ENCRYPTED if encrypted else 0
    protected_size = 28
    struct.pack_into(
        "<IIHHII", image, 0, MODULE.IMAGE_MAGIC, 0,
        MODULE.HEADER_SIZE, protected_size, image_size, flags,
    )
    struct.pack_into("<BBHI", image, 20, *identity[:4])
    for offset in range(MODULE.HEADER_SIZE,
                        MODULE.HEADER_SIZE + image_size):
        image[offset] = (seed + offset) & 0xFF

    protected = MODULE.HEADER_SIZE + image_size
    struct.pack_into(
        "<HHHHIHH", image, protected,
        MODULE.PROTECTED_TLV_MAGIC, protected_size,
        MODULE.SECURITY_COUNTER_TLV, 4, identity[4],
        MODULE.DEPENDENCY_TLV, 12,
    )
    image[protected + 16:protected + protected_size] = bytes(range(12))

    entries = [
        (MODULE.SHA256_TLV, bytes([seed]) * 32),
        (MODULE.KEYHASH_TLV, bytes([seed + 1]) * 32),
        (MODULE.ECDSA256_TLV, bytes([seed + 2]) * 64),
    ]
    if encrypted:
        entries.append((MODULE.ENCRYPTION_EC256_TLV, bytes([seed + 3]) * 113))
    total = 4 + sum(4 + len(value) for _, value in entries)
    cursor = protected + protected_size
    struct.pack_into("<HH", image, cursor, MODULE.UNPROTECTED_TLV_MAGIC, total)
    cursor += 4
    for tlv_type, value in entries:
        struct.pack_into("<HH", image, cursor, tlv_type, len(value))
        cursor += 4
        image[cursor:cursor + len(value)] = value
        cursor += len(value)

    if not encrypted:
        start = size - MODULE.IMAGE_OK_OFFSET_FROM_END
        image[start:start + MODULE.BOOT_MAX_ALIGN] = MODULE._program_unit(1)
    image[-len(MODULE.BOOT_MAGIC):] = MODULE.BOOT_MAGIC
    return bytes(image)


def make_synthetic_release(version="1.0.16"):
    profile = MODULE.RELEASE_PROFILES[version]
    secure_initial = make_reference_image(
        MODULE.SECURE_SIZE, encrypted=False, image_size=0x1800, seed=11,
        identity=profile.identity)
    secure_update = make_reference_image(
        MODULE.SECURE_SIZE, encrypted=True, image_size=0x1800, seed=101,
        identity=profile.identity)
    nonsecure_initial = make_reference_image(
        MODULE.NONSECURE_SIZE, encrypted=False, image_size=0x2400, seed=23,
        identity=profile.identity)
    nonsecure_update = make_reference_image(
        MODULE.NONSECURE_SIZE, encrypted=True, image_size=0x2400, seed=113,
        identity=profile.identity)
    secure_current = MODULE._parse_image_record(
        "secure-synthetic-current", secure_update, MODULE.SECURE_SIZE,
        MODULE.IMAGE_F_ENCRYPTED, profile.identity, profile.version)
    nonsecure_current = MODULE._parse_image_record(
        "nonsecure-synthetic-current", nonsecure_update, MODULE.NONSECURE_SIZE,
        MODULE.IMAGE_F_ENCRYPTED, profile.identity, profile.version)
    secure = MODULE._load_release_image(
        "secure", MODULE.SECURE_ADDRESS, MODULE.SECURE_SIZE, 0,
        secure_initial, secure_update,
        secure_current.record_end,
        hashlib.sha256(bytes(range(1, 33))).hexdigest(),
        profile.identity, profile.version)
    nonsecure = MODULE._load_release_image(
        "nonsecure", MODULE.NONSECURE_ADDRESS, MODULE.NONSECURE_SIZE, 1,
        nonsecure_initial, nonsecure_update,
        nonsecure_current.record_end + 1,
        hashlib.sha256(bytes(range(65, 97))).hexdigest(),
        profile.identity, profile.version)
    return MODULE.ReleaseArtifacts(
        Path(f"synthetic/{version}"), profile,
        profile.expected_metadata(), secure, nonsecure)


def make_installed(reference, *, key_seed=1, key_area=None):
    record = reference.update_record
    image = bytearray(b"\xff" * reference.size)
    payload_end = record.header_size + record.image_size
    image[:record.header_size] = reference.update[:record.header_size]
    image[record.header_size:payload_end] = \
        reference.initial[record.header_size:payload_end]
    image[payload_end:record.record_end] = \
        reference.update[payload_end:record.record_end]

    trailer_start = reference.size - MODULE.BOOT_TRAILER_SIZE
    status_entries = (
        (reference.swap_size + MODULE.SCRATCH_SIZE - 1) //
        MODULE.SCRATCH_SIZE
    ) * MODULE.BOOT_STATUS_STATE_COUNT
    for entry in range(status_entries):
        start = trailer_start + entry * MODULE.BOOT_MAX_ALIGN
        image[start:start + MODULE.BOOT_MAX_ALIGN] = \
            MODULE._program_unit(entry % MODULE.BOOT_STATUS_STATE_COUNT + 1)

    status_end = trailer_start + (
        MODULE.BOOT_STATUS_MAX_ENTRIES * MODULE.BOOT_STATUS_STATE_COUNT *
        MODULE.BOOT_MAX_ALIGN
    )
    if key_area is None:
        key_area = bytes((key_seed + index) & 0xFF for index in range(32))
    assert len(key_area) == 32
    image[status_end:status_end + len(key_area)] = key_area

    def set_unit(offset_from_end, value):
        start = reference.size - offset_from_end
        image[start:start + MODULE.BOOT_MAX_ALIGN] = value

    set_unit(
        MODULE.SWAP_SIZE_OFFSET_FROM_END,
        struct.pack("<I", reference.swap_size) + b"\xff" * 12,
    )
    set_unit(
        MODULE.SWAP_INFO_OFFSET_FROM_END,
        MODULE._program_unit((reference.image_index << 4) | 0x2),
    )
    set_unit(MODULE.COPY_DONE_OFFSET_FROM_END, MODULE._program_unit(1))
    set_unit(MODULE.IMAGE_OK_OFFSET_FROM_END, MODULE._program_unit(1))
    image[-len(MODULE.BOOT_MAGIC):] = MODULE.BOOT_MAGIC
    return bytes(image)


def decrypt_release_image_key(version, name):
    from cryptography.hazmat.primitives import hashes, hmac, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF
    import zipfile

    private_key = serialization.load_pem_private_key(
        (PKI_DIR / "oemirot-encryption.pem").read_bytes(),
        password=(PKI_DIR / "key-passphrase.txt").read_bytes().strip(),
    )
    package = (
        PROJECT / "artifacts" / "firmware" / version /
        f"roller-ecu-{version}.recu"
    )
    with zipfile.ZipFile(package) as archive:
        image = archive.read(f"{name}.bin")
    header_size, protected_size, image_size = struct.unpack_from("<HHI", image, 8)
    unprotected = header_size + image_size + protected_size
    total = struct.unpack_from("<H", image, unprotected + 2)[0]
    cursor = unprotected + 4
    encrypted_key_tlv = None
    while cursor < unprotected + total:
        tlv_type, tlv_length = struct.unpack_from("<HH", image, cursor)
        value = image[cursor + 4:cursor + 4 + tlv_length]
        if tlv_type == MODULE.ENCRYPTION_EC256_TLV:
            encrypted_key_tlv = value
        cursor += 4 + tlv_length
    assert encrypted_key_tlv is not None and len(encrypted_key_tlv) == 113

    ephemeral = ec.EllipticCurvePublicKey.from_encoded_point(
        ec.SECP256R1(), encrypted_key_tlv[:65])
    shared = private_key.exchange(ec.ECDH(), ephemeral)
    derived = HKDF(
        algorithm=hashes.SHA256(), length=48, salt=None,
        info=b"MCUBoot_ECIES_v1",
    ).derive(shared)
    verifier = hmac.HMAC(derived[16:], hashes.SHA256())
    verifier.update(encrypted_key_tlv[97:])
    verifier.verify(encrypted_key_tlv[65:97])
    decryptor = Cipher(
        algorithms.AES(derived[:16]), modes.CTR(bytes(16))
    ).decryptor()
    return decryptor.update(encrypted_key_tlv[97:]) + decryptor.finalize()


def copy_pinned_release_chain(root):
    current = root / "1.0.16"
    shutil.copytree(PINNED_RELEASE, current)
    shutil.copytree(PINNED_RELEASE.parent / "1.0.15", root / "1.0.15")
    return current


class ClosedOtaReadbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.release = make_synthetic_release()
        cls.secure = make_installed(cls.release.secure, key_seed=1)
        cls.nonsecure = make_installed(cls.release.nonsecure, key_seed=65)

    def test_geometry_and_release_are_exact_1_0_16(self):
        self.assertEqual(MODULE.SECURE_ADDRESS, 0x0C030000)
        self.assertEqual(MODULE.NONSECURE_ADDRESS, 0x08100000)
        self.assertEqual(MODULE.SECURE_SIZE, 0x30000)
        self.assertEqual(MODULE.NONSECURE_SIZE, 0x50000)
        self.assertEqual(
            self.release.secure.initial_record.identity,
            MODULE.ReleaseIdentity(1, 0, 16, 0, 16),
        )
        self.assertEqual(self.release.metadata, MODULE.EXPECTED_METADATA)

    @unittest.skipUnless(PINNED_RELEASE_1_0_17.is_dir(),
                         "reviewed 1.0.17 artifacts unavailable")
    def test_immutable_pinned_1_0_17_release_and_previous_swap_load(self):
        release = MODULE.load_release(PINNED_RELEASE_1_0_17)
        self.assertEqual(release.profile.version, "1.0.17")
        self.assertEqual(release.metadata["version"], "1.0.17")
        self.assertEqual(release.metadata["security_counter"], 17)
        self.assertEqual(release.metadata["update_sequence"], 17)
        self.assertEqual(
            release.secure.initial_record.identity,
            MODULE.ReleaseIdentity(1, 0, 17, 0, 17),
        )
        self.assertEqual(release.secure.previous_record_size, 0x2DDE8)
        self.assertEqual(release.secure.update_record.record_end, 0x2DDE7)
        self.assertEqual(release.secure.swap_size, 0x2DDE8)
        self.assertEqual(release.nonsecure.previous_record_size, 0x8D38)
        self.assertEqual(release.nonsecure.update_record.record_end, 0x7D38)
        self.assertEqual(release.nonsecure.swap_size, 0x8D38)

    @unittest.skipUnless(PINNED_RELEASE_1_0_17.is_dir(),
                         "reviewed 1.0.17 artifacts unavailable")
    def test_1_0_17_post_swap_key_slots_and_installed_pair_are_exact(self):
        if not (PKI_DIR / "oemirot-encryption.pem").is_file() or not \
                (PKI_DIR / "key-passphrase.txt").is_file():
            self.skipTest("local OEMiROT encryption PKI unavailable")
        release = MODULE.load_release(PINNED_RELEASE_1_0_17)
        secure_keys = (
            decrypt_release_image_key("1.0.17", "secure") +
            decrypt_release_image_key("1.0.16", "secure")
        )
        nonsecure_keys = (
            decrypt_release_image_key("1.0.17", "nonsecure") +
            decrypt_release_image_key("1.0.16", "nonsecure")
        )
        self.assertEqual(
            hashlib.sha256(secure_keys).hexdigest(),
            release.profile.secure_key_area_sha256,
        )
        self.assertEqual(
            hashlib.sha256(nonsecure_keys).hexdigest(),
            release.profile.nonsecure_key_area_sha256,
        )
        secure = make_installed(release.secure, key_area=secure_keys)
        nonsecure = make_installed(release.nonsecure, key_area=nonsecure_keys)
        secure_report, nonsecure_report = MODULE.validate_pair(
            secure, nonsecure, release)
        self.assertEqual(secure_report.identity,
                         MODULE.ReleaseIdentity(1, 0, 17, 0, 17))
        self.assertEqual(nonsecure_report.identity, secure_report.identity)
        self.assertEqual((secure_report.image_ok, nonsecure_report.image_ok),
                         (1, 1))

    @unittest.skipUnless(PINNED_RELEASE_1_0_17.is_dir(),
                         "reviewed 1.0.17 artifacts unavailable")
    def test_1_0_17_profile_rejects_changed_current_or_previous_release(self):
        for mutation in ("current", "previous"):
            with self.subTest(mutation=mutation), \
                    tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                current = root / "1.0.17"
                previous = root / "1.0.16"
                shutil.copytree(PINNED_RELEASE_1_0_17, current)
                shutil.copytree(PINNED_RELEASE, previous)
                path = (
                    current / "roller-ecu-1.0.17.recu"
                    if mutation == "current" else
                    previous / "roller-ecu-1.0.16.recu"
                )
                data = bytearray(path.read_bytes())
                data[-1] ^= 1
                path.write_bytes(data)
                with self.assertRaises(MODULE.ReadbackVerificationError):
                    MODULE.load_release(current)

    def test_1_0_17_synthetic_pair_validates_without_private_key(self):
        release = make_synthetic_release("1.0.17")
        secure = make_installed(release.secure, key_seed=1)
        nonsecure = make_installed(release.nonsecure, key_seed=65)
        secure_report, nonsecure_report = MODULE.validate_pair(
            secure, nonsecure, release)
        expected = MODULE.ReleaseIdentity(1, 0, 17, 0, 17)
        self.assertEqual(secure_report.identity, expected)
        self.assertEqual(nonsecure_report.identity, expected)

    @unittest.skipUnless(
        PINNED_RELEASE.is_dir() and PINNED_RELEASE_1_0_17.is_dir(),
        "reviewed 1.0.16/1.0.17 artifacts unavailable",
    )
    def test_release_profiles_are_immutable_across_load_order(self):
        release17 = MODULE.load_release(PINNED_RELEASE_1_0_17)
        key_area = bytes(range(32))
        reference17 = replace(
            release17.secure,
            encrypted_key_area_sha256=hashlib.sha256(key_area).hexdigest(),
        )
        installed17 = make_installed(reference17, key_area=key_area)

        release16 = MODULE.load_release(PINNED_RELEASE)
        self.assertEqual(release16.profile.version, "1.0.16")
        report17 = MODULE.validate_installed_image(installed17, reference17)
        self.assertEqual(
            report17.identity, MODULE.ReleaseIdentity(1, 0, 17, 0, 17))
        self.assertEqual(release17.profile.version, "1.0.17")

        with self.assertRaises(TypeError):
            MODULE.RELEASE_PROFILES["1.0.18"] = release17.profile

    def test_unreviewed_release_profile_is_rejected_before_file_access(self):
        with self.assertRaisesRegex(
                MODULE.ReadbackVerificationError, "not reviewed"):
            MODULE.load_release(Path("/does/not/exist/1.0.18"))

    @unittest.skipUnless(PINNED_RELEASE.is_dir(),
                         "reviewed 1.0.16 artifacts unavailable")
    def test_immutable_pinned_1_0_16_release_artifacts_load(self):
        release = MODULE.load_release(PINNED_RELEASE)
        self.assertEqual(release.metadata, MODULE.EXPECTED_METADATA)

    def test_accepts_exact_post_swap_pair_and_three_nonsecure_segments(self):
        secure = MODULE.assemble_segments(
            "secure",
            [(MODULE.SECURE_ADDRESS, self.secure)],
            MODULE.SECURE_ADDRESS,
            MODULE.SECURE_SIZE,
        )
        nonsecure = MODULE.assemble_segments(
            "nonsecure",
            [
                (MODULE.NONSECURE_ADDRESS, self.nonsecure[:0x28000]),
                (MODULE.NONSECURE_ADDRESS + 0x28000,
                 self.nonsecure[0x28000:0x38000]),
                (MODULE.NONSECURE_ADDRESS + 0x38000,
                 self.nonsecure[0x38000:]),
            ],
            MODULE.NONSECURE_ADDRESS,
            MODULE.NONSECURE_SIZE,
        )
        secure_report, nonsecure_report = MODULE.validate_pair(
            secure, nonsecure, self.release)
        self.assertEqual(secure_report.identity, nonsecure_report.identity)
        self.assertEqual((secure_report.image_ok, nonsecure_report.image_ok), (1, 1))
        self.assertEqual(nonsecure_report.encrypted_key_slots_present, 2)

    def test_nonsecure_swap_size_is_exact_max_of_previous_and_current_records(self):
        reference = self.release.nonsecure
        self.assertEqual(
            reference.previous_record_size,
            reference.update_record.record_end + 1,
        )
        self.assertEqual(reference.swap_size, reference.previous_record_size)
        report = MODULE.validate_installed_image(self.nonsecure, reference)
        self.assertEqual(report.image_record_size, reference.update_record.record_end)
        self.assertEqual(report.swap_size, reference.previous_record_size)

    def test_status_entries_use_swap_size_across_scratch_boundary(self):
        reference = replace(
            self.release.secure,
            previous_record_size=MODULE.SCRATCH_SIZE + 1,
            swap_size=MODULE.SCRATCH_SIZE + 1,
        )
        self.assertLess(reference.update_record.record_end, MODULE.SCRATCH_SIZE)
        image = make_installed(reference, key_seed=1)
        report = MODULE.validate_installed_image(image, reference)
        self.assertEqual(
            report.swap_status_entries,
            2 * MODULE.BOOT_STATUS_STATE_COUNT,
        )

    def test_nonsecure_swap_size_rejects_current_record_end_and_any_other_size(self):
        reference = self.release.nonsecure
        for value in (
                reference.update_record.record_end,
                reference.swap_size - 2,
                reference.swap_size + 1):
            with self.subTest(value=hex(value)):
                image = bytearray(self.nonsecure)
                start = len(image) - MODULE.SWAP_SIZE_OFFSET_FROM_END
                image[start:start + MODULE.BOOT_MAX_ALIGN] = \
                    struct.pack("<I", value) + b"\xff" * 12
                with self.assertRaisesRegex(
                        MODULE.ReadbackVerificationError,
                        r"max\(previous,current\)"):
                    MODULE.validate_installed_image(bytes(image), reference)

    def test_segments_reject_truncation_extra_gap_overlap_order_and_empty(self):
        cases = {
            "truncated": [(MODULE.NONSECURE_ADDRESS, self.nonsecure[:-1])],
            "extra": [(MODULE.NONSECURE_ADDRESS, self.nonsecure + b"\xff")],
            "gap": [
                (MODULE.NONSECURE_ADDRESS, self.nonsecure[:16]),
                (MODULE.NONSECURE_ADDRESS + 17, self.nonsecure[17:]),
            ],
            "overlap": [
                (MODULE.NONSECURE_ADDRESS, self.nonsecure[:16]),
                (MODULE.NONSECURE_ADDRESS + 15, self.nonsecure[15:]),
            ],
            "out-of-order": [
                (MODULE.NONSECURE_ADDRESS + 16, self.nonsecure[16:]),
                (MODULE.NONSECURE_ADDRESS, self.nonsecure[:16]),
            ],
            "empty": [(MODULE.NONSECURE_ADDRESS, b"")],
        }
        for name, segments in cases.items():
            with self.subTest(name=name), self.assertRaises(
                    MODULE.ReadbackVerificationError):
                MODULE.assemble_segments(
                    "nonsecure", segments,
                    MODULE.NONSECURE_ADDRESS, MODULE.NONSECURE_SIZE)

    def test_wrong_physical_alias_or_layout_address_is_rejected(self):
        for address in (
                MODULE.NONSECURE_ADDRESS - 1,
                MODULE.NONSECURE_ADDRESS + 1,
                MODULE.FLASH_BASE_NS + MODULE.SECURE_OFFSET):
            with self.subTest(address=hex(address)), self.assertRaises(
                    MODULE.ReadbackVerificationError):
                MODULE.assemble_segments(
                    "nonsecure", [(address, self.nonsecure)],
                    MODULE.NONSECURE_ADDRESS, MODULE.NONSECURE_SIZE)

    def test_readback_rejects_wrong_version_counter_and_flags(self):
        mutations = {}
        version = bytearray(self.secure)
        version[22] ^= 1
        mutations["version"] = version
        counter = bytearray(self.secure)
        protected = (
            self.release.secure.update_record.header_size +
            self.release.secure.update_record.image_size
        )
        counter[protected + 8] ^= 1
        mutations["counter"] = counter
        flags = bytearray(self.secure)
        flags[16] = 0
        mutations["flags"] = flags
        for name, image in mutations.items():
            with self.subTest(name=name), self.assertRaises(
                    MODULE.ReadbackVerificationError):
                MODULE.validate_installed_image(bytes(image), self.release.secure)

    def test_readback_rejects_payload_header_and_signed_tlv_changes(self):
        record = self.release.secure.update_record
        mutations = {}
        header = bytearray(self.secure)
        header[31] ^= 1
        mutations["header"] = header
        payload = bytearray(self.secure)
        payload[record.header_size + 17] ^= 1
        mutations["payload"] = payload
        signed_tlv = bytearray(self.secure)
        signed_tlv[record.header_size + record.image_size +
                   record.protected_size + 12] ^= 1
        mutations["signed-tlv"] = signed_tlv
        for name, image in mutations.items():
            with self.subTest(name=name), self.assertRaises(
                    MODULE.ReadbackVerificationError):
                MODULE.validate_installed_image(bytes(image), self.release.secure)

    def test_readback_rejects_truncated_and_extra_slot_bytes(self):
        for image in (self.secure[:-1], self.secure + b"\xff"):
            with self.assertRaisesRegex(
                    MODULE.ReadbackVerificationError, "wrong primary slot size"):
                MODULE.validate_installed_image(image, self.release.secure)

    def test_readback_rejects_non_erased_padding(self):
        image = bytearray(self.nonsecure)
        image[self.release.nonsecure.update_record.record_end + 100] = 0
        with self.assertRaisesRegex(
                MODULE.ReadbackVerificationError, "padding"):
            MODULE.validate_installed_image(bytes(image), self.release.nonsecure)

    def test_readback_rejects_each_safety_critical_trailer_field(self):
        status_start = MODULE.SECURE_SIZE - MODULE.BOOT_TRAILER_SIZE
        status_end = status_start + (
            MODULE.BOOT_STATUS_MAX_ENTRIES * MODULE.BOOT_STATUS_STATE_COUNT *
            MODULE.BOOT_MAX_ALIGN
        )
        offsets = {
            "status": status_start,
            "key-slot": status_end,
            "swap-size": MODULE.SECURE_SIZE - MODULE.SWAP_SIZE_OFFSET_FROM_END,
            "swap-info": MODULE.SECURE_SIZE - MODULE.SWAP_INFO_OFFSET_FROM_END,
            "copy-done": MODULE.SECURE_SIZE - MODULE.COPY_DONE_OFFSET_FROM_END,
            "image-ok": MODULE.SECURE_SIZE - MODULE.IMAGE_OK_OFFSET_FROM_END,
            "boot-magic": MODULE.SECURE_SIZE - len(MODULE.BOOT_MAGIC),
        }
        for name, offset in offsets.items():
            with self.subTest(name=name):
                image = bytearray(self.secure)
                if name == "key-slot":
                    image[offset:offset + MODULE.BOOT_ENC_KEY_SIZE] = \
                        b"\xff" * MODULE.BOOT_ENC_KEY_SIZE
                else:
                    image[offset] ^= 1
                with self.assertRaises(MODULE.ReadbackVerificationError):
                    MODULE.validate_installed_image(bytes(image), self.release.secure)

    @unittest.skipUnless(PINNED_RELEASE.is_dir(),
                         "reviewed 1.0.16 artifacts unavailable")
    def test_release_rejects_changed_metadata_initial_package_or_extra_member(self):
        for mutation in ("metadata", "initial", "package", "extra"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                release_dir = copy_pinned_release_chain(Path(temp))
                if mutation == "metadata":
                    path = release_dir / "metadata.json"
                    data = bytearray(path.read_bytes())
                    data[0] ^= 1
                    path.write_bytes(data)
                elif mutation == "initial":
                    path = release_dir / "secure-initial.bin"
                    data = bytearray(path.read_bytes())
                    data[100] ^= 1
                    path.write_bytes(data)
                elif mutation == "package":
                    path = release_dir / "roller-ecu-1.0.16.recu"
                    data = bytearray(path.read_bytes())
                    data[-1] ^= 1
                    path.write_bytes(data)
                else:
                    (release_dir / "unreviewed.txt").write_text("no")
                with self.assertRaises(MODULE.ReadbackVerificationError):
                    MODULE.load_release(release_dir)

    @unittest.skipUnless(PINNED_RELEASE.is_dir(),
                         "reviewed 1.0.16 artifacts unavailable")
    def test_release_rejects_wrong_version_counter_and_layout_metadata(self):
        replacements = {
            "version": ('"version": "1.0.16"', '"version": "1.0.17"'),
            "counter": ('"security_counter": 16', '"security_counter": 15'),
            "layout": ('"layout_version": 65536', '"layout_version": 65537'),
        }
        for name, (old, new) in replacements.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp:
                release_dir = copy_pinned_release_chain(Path(temp))
                metadata = release_dir / "metadata.json"
                text = metadata.read_text(encoding="utf-8")
                self.assertIn(old, text)
                metadata.write_text(text.replace(old, new), encoding="utf-8")
                with self.assertRaises(MODULE.ReadbackVerificationError):
                    MODULE.load_release(release_dir)

    @unittest.skipUnless(PINNED_RELEASE.is_dir(),
                         "reviewed 1.0.16 artifacts unavailable")
    def test_release_rejects_missing_or_modified_previous_swap_reference(self):
        for mutation in ("missing", "modified"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                release_dir = copy_pinned_release_chain(Path(temp))
                prior = (
                    release_dir.parent / "1.0.15" /
                    "roller-ecu-1.0.15.recu"
                )
                if mutation == "missing":
                    prior.unlink()
                else:
                    data = bytearray(prior.read_bytes())
                    data[-1] ^= 1
                    prior.write_bytes(data)
                with self.assertRaisesRegex(
                        MODULE.ReadbackVerificationError,
                        "previous"):
                    MODULE.load_release(release_dir)

    @unittest.skipUnless(PINNED_RELEASE.is_dir(),
                         "reviewed 1.0.16 artifacts unavailable")
    def test_cli_accepts_addressed_segments_and_emits_strict_report(self):
        if not (PKI_DIR / "oemirot-encryption.pem").is_file() or not \
                (PKI_DIR / "key-passphrase.txt").is_file():
            self.skipTest("local OEMiROT encryption PKI unavailable")
        pinned = MODULE.load_release(PINNED_RELEASE)
        secure_keys = (
            decrypt_release_image_key("1.0.16", "secure") +
            decrypt_release_image_key("1.0.15", "secure")
        )
        nonsecure_keys = (
            decrypt_release_image_key("1.0.16", "nonsecure") +
            decrypt_release_image_key("1.0.15", "nonsecure")
        )
        self.assertEqual(
            hashlib.sha256(secure_keys).hexdigest(),
            MODULE.EXPECTED_ENCRYPTED_KEY_AREA_HASHES["secure"],
        )
        self.assertEqual(
            hashlib.sha256(nonsecure_keys).hexdigest(),
            MODULE.EXPECTED_ENCRYPTED_KEY_AREA_HASHES["nonsecure"],
        )
        secure = make_installed(pinned.secure, key_area=secure_keys)
        nonsecure = make_installed(pinned.nonsecure, key_area=nonsecure_keys)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            secure_path = root / "secure.bin"
            ns0, ns1, ns2 = root / "ns0.bin", root / "ns1.bin", root / "ns2.bin"
            report_path = root / "report.json"
            secure_path.write_bytes(secure)
            ns0.write_bytes(nonsecure[:0x28000])
            ns1.write_bytes(nonsecure[0x28000:0x38000])
            ns2.write_bytes(nonsecure[0x38000:])
            result = subprocess.run(
                [
                    sys.executable, str(MODULE_PATH),
                    "--release-dir", str(PINNED_RELEASE),
                    "--secure-segment", f"0x0C030000={secure_path}",
                    "--nonsecure-segment", f"0x08100000={ns0}",
                    "--nonsecure-segment", f"0x08128000={ns1}",
                    "--nonsecure-segment", f"0x08138000={ns2}",
                    "--report", str(report_path),
                ],
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(
                set(report), {"schema", "release", "segments", "images"})
            self.assertEqual(report["release"]["version"], "1.0.16")
            self.assertEqual(report["images"]["secure"]["image_ok"], 1)
            self.assertEqual(len(report["segments"]["nonsecure"]), 3)

    @unittest.skipUnless(PINNED_RELEASE.is_dir(),
                         "reviewed 1.0.16 artifacts unavailable")
    def test_cli_rejects_existing_report_and_symlinked_input(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            secure_real = root / "secure-real.bin"
            secure_link = root / "secure-link.bin"
            nonsecure_path = root / "nonsecure.bin"
            report_path = root / "report.json"
            secure_real.write_bytes(self.secure)
            secure_link.symlink_to(secure_real)
            nonsecure_path.write_bytes(self.nonsecure)
            report_path.write_text("do not overwrite", encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable, str(MODULE_PATH),
                    "--release-dir", str(PINNED_RELEASE),
                    "--secure-segment", f"0x0C030000={secure_link}",
                    "--nonsecure-segment", f"0x08100000={nonsecure_path}",
                    "--report", str(report_path),
                ],
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertEqual(report_path.read_text(encoding="utf-8"),
                             "do not overwrite")


if __name__ == "__main__":
    unittest.main()
