"""No sockets: factory provisioning must fail closed on wrong board pins."""
import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import factory_provision_atecc as factory


class FactoryTargetTests(unittest.TestCase):
    UID = "003900443434511232383537"
    SERIAL = "01236acf4e275ef9ee"
    CRC = 0x3321181A

    def setUp(self):
        self.status = dict(result=0, mcu_uid_hex=self.UID,
                           atecc_serial=self.SERIAL, atecc_revision="00006005",
                           private_key_slot=2, phase_flags=1, config_locked=False,
                           data_locked=False, config_crc32c=self.CRC)

    def validate(self, status):
        factory.validate_target(status, self.UID, self.SERIAL, self.CRC)

    def test_reviewed_initial_target(self):
        self.validate(self.status)

    def test_wrong_identity_is_rejected(self):
        for field, value in (("mcu_uid_hex", "003800613434511232383537"),
                             ("atecc_serial", "0123d47eb2ee0e9bee"),
                             ("atecc_revision", "00006004"),
                             ("private_key_slot", 3), ("result", -101)):
            with self.subTest(field=field):
                other = copy.deepcopy(self.status)
                other[field] = value
                with self.assertRaises(RuntimeError):
                    self.validate(other)

    def test_unjournaled_config_drift_or_locks_are_rejected(self):
        for field, value in (("config_crc32c", self.CRC ^ 1),
                             ("config_locked", True), ("data_locked", True)):
            with self.subTest(field=field):
                other = dict(self.status, **{field: value})
                with self.assertRaises(RuntimeError):
                    self.validate(other)

    def test_journaled_retry_keeps_initial_crc_for_firmware_check(self):
        self.validate(dict(self.status, phase_flags=7, config_locked=True,
                           config_crc32c=self.CRC ^ 1))

    def test_malformed_external_pins_rejected(self):
        for uid, serial, crc in (("", self.SERIAL, self.CRC),
                                 (self.UID, "z" * 18, self.CRC),
                                 (self.UID, self.SERIAL, -1),
                                 (self.UID, self.SERIAL, 0x100000000)):
            with self.assertRaises(RuntimeError):
                factory.validate_target(self.status, uid, serial, crc)


if __name__ == "__main__":
    unittest.main()
