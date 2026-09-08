import importlib.util
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "verify_pairing_store", ROOT / "tools" / "verify_pairing_store.py")
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def valid_sector() -> bytes:
    record = bytearray(b"\xFF" * MODULE.RECORD_SIZE)
    struct.pack_into("<4I", record, 0, MODULE.MAGIC, MODULE.SCHEMA,
                     MODULE.EXPECTED_GENERATION, MODULE.LAYOUT_VERSION)
    struct.pack_into("<3I", record, 16, *MODULE.EXPECTED_MCU_UID)
    struct.pack_into("<I", record, 28, MODULE.EXPECTED_CONFIG_CRC32C)
    record[32:41] = MODULE.EXPECTED_SERIAL
    record[41:45] = MODULE.EXPECTED_REVISION
    record[45] = MODULE.EXPECTED_I2C_ADDRESS
    record[46] = MODULE.EXPECTED_PRIVATE_KEY_SLOT
    record[47] = 0
    record[48:112] = MODULE.EXPECTED_PUBLIC_KEY
    struct.pack_into("<I", record, 112, MODULE.RECORD_SIZE)
    struct.pack_into("<I", record, 116, MODULE.crc32c(record[:116]))
    struct.pack_into("<I", record, 128, MODULE.COMMIT)
    return bytes(record) + b"\xFF" * (MODULE.SECTOR_SIZE - len(record))


class PairingStoreTests(unittest.TestCase):
    def test_new_board_requires_explicit_identity(self):
        identity = MODULE.REVIEWED_IDENTITIES["SN-EJAHGJQ"]
        sector = bytearray(valid_sector())
        struct.pack_into("<3I", sector, 16, *identity.mcu_uid)
        struct.pack_into("<I", sector, 28, identity.config_crc32c)
        sector[32:41] = identity.serial
        sector[48:112] = identity.public_key
        struct.pack_into("<I", sector, 116, MODULE.crc32c(sector[:116]))
        self.assertEqual(MODULE.verify_dual_store(
            bytes(sector + sector), identity=identity)["result"], "PASS")
        with self.assertRaisesRegex(MODULE.VerificationError, "MCU UID"):
            MODULE.verify_dual_store(bytes(sector + sector))
        with self.assertRaisesRegex(MODULE.VerificationError, "MCU UID"):
            MODULE.verify_dual_store(valid_sector() * 2, identity=identity)

    def test_exact_dual_copy_passes(self):
        sector = valid_sector()
        result = MODULE.verify_dual_store(sector + sector)
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(result["redundant_copies"], 2)

    def test_blank_second_copy_fails(self):
        with self.assertRaisesRegex(MODULE.VerificationError,
                                    "B: pairing magic mismatch"):
            MODULE.verify_dual_store(
                valid_sector() + b"\xFF" * MODULE.SECTOR_SIZE)

    def test_crc_corruption_fails(self):
        sector = bytearray(valid_sector())
        sector[116] ^= 1
        with self.assertRaisesRegex(MODULE.VerificationError, "CRC32C"):
            MODULE.verify_sector(bytes(sector), "A")

    def test_wrong_public_key_with_recomputed_crc_fails(self):
        sector = bytearray(valid_sector())
        sector[48] ^= 1
        struct.pack_into("<I", sector, 116,
                         MODULE.crc32c(sector[:116]))
        with self.assertRaisesRegex(MODULE.VerificationError, "public key"):
            MODULE.verify_sector(bytes(sector), "A")

    def test_unexpected_tail_data_fails(self):
        sector = bytearray(valid_sector())
        sector[MODULE.RECORD_SIZE] = 0
        with self.assertRaisesRegex(MODULE.VerificationError,
                                    "unexpected data"):
            MODULE.verify_sector(bytes(sector), "A")

    def test_valid_but_different_copy_fails(self):
        sector_a = valid_sector()
        sector_b = bytearray(valid_sector())
        sector_b[120] = 0xFE
        # Reserved bytes are independently checked before byte equivalence.
        with self.assertRaisesRegex(MODULE.VerificationError, "reserved"):
            MODULE.verify_dual_store(sector_a + bytes(sector_b))


if __name__ == "__main__":
    unittest.main()
