import hashlib
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
import uuid
from pathlib import Path


PROJECT = Path(__file__).resolve().parents[1]
MODULE_PATH = PROJECT / "tools" / "verify_closed_preflight.py"
RELEASE_CLOSED = (
    PROJECT / "Bootloader" / "OEMiROT" / "build" / "ReleaseClosed" /
    "ECU_OEMiROT.bin"
)
INSTALLED_PRIMARY_AUDIT = (
    PROJECT / "artifacts" / "device-backups" /
    "stm32h563-066BFF565456857187210935-20260830T060506Z-post-ota-1.0.15-audit" /
    "primary-audit.json"
)
SPEC = importlib.util.spec_from_file_location("verify_closed_preflight", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def runtime_state():
    return {
        "status_age_ms": 10,
        "diagnostic_age_ms": 11,
        "security_age_ms": 12,
        "security": {
            "atecc_result": 0,
            "auth_result": 0,
            "flags": 1 << 4,
            "config_locked": 1,
            "data_locked": 1,
            "mcu_uid": MODULE.EXPECTED_MCU_UID,
            "serial": MODULE.EXPECTED_ATECC_SERIAL,
            "config_crc32c": MODULE.EXPECTED_ATECC_CONFIG_CRC32C,
            "pairing_generation": MODULE.EXPECTED_PAIRING_GENERATION,
        },
        "diagnostic": {
            "safety_status": 0,
            "requested_relay_mask": 0,
            "applied_relay_mask": 0,
        },
        "status": {
            "forward_duty_permille": 0,
            "reverse_duty_permille": 0,
        },
        "tuning_active": False,
    }


def ota_status():
    return {
        "result": 0,
        "ota_result": 0,
        "state": 0,
        "accepted_sequence": 15,
        "update_sequence": 0,
        "secure_received": 0,
        "nonsecure_received": 0,
        "secure_image_size": 0,
        "nonsecure_image_size": 0,
    }


def primary_audit():
    identity = dict(MODULE.EXPECTED_RELEASE_IDENTITY)
    return {
        "schema": "roller-ecu-open-loader-primary-audit-v1",
        "release_identity": identity,
        "images": {
            "secure": {"identity": dict(identity), "image_ok": 1},
            "nonsecure": {"identity": dict(identity), "image_ok": 1},
        },
    }


class ClosedPreflightTests(unittest.TestCase):
    def test_accepts_exact_physical_uid(self):
        words = [int(MODULE.EXPECTED_MCU_UID[index:index + 8], 16)
                 for index in range(0, 24, 8)]
        encoded = struct.pack("<III", *words)
        self.assertEqual(MODULE.validate_programmer_uid(encoded),
                         MODULE.EXPECTED_MCU_UID)

    def test_rejects_wrong_or_truncated_physical_uid(self):
        with self.assertRaises(MODULE.ClosedPreflightError):
            MODULE.validate_programmer_uid(b"\0" * 12)
        with self.assertRaises(MODULE.ClosedPreflightError):
            MODULE.validate_programmer_uid(b"\0" * 11)

    def test_accepts_exact_fresh_runtime_identity_and_confirmation(self):
        MODULE.validate_runtime_state(runtime_state())

    def test_runtime_identity_fields_fail_closed(self):
        mutations = (
            ("security", "serial", "0123d47eb2ee0e9bef"),
            ("security", "config_crc32c", MODULE.EXPECTED_ATECC_CONFIG_CRC32C ^ 1),
            ("security", "pairing_generation", 2),
            ("security", "mcu_uid", "f" * 24),
            ("security", "auth_result", -1),
            ("security", "atecc_result", -1),
        )
        for section, name, value in mutations:
            with self.subTest(name=name):
                state = runtime_state()
                state[section][name] = value
                with self.assertRaises(MODULE.ClosedPreflightError):
                    MODULE.validate_runtime_state(state)

    def test_runtime_requires_fresh_security_and_confirmation_clear(self):
        for name, value in (("security_age_ms", None),
                            ("security_age_ms", 501)):
            with self.subTest(name=name, value=value):
                state = runtime_state()
                state[name] = value
                with self.assertRaises(MODULE.ClosedPreflightError):
                    MODULE.validate_runtime_state(state)
        state = runtime_state()
        state["diagnostic"]["safety_status"] |= \
            MODULE.SAFETY_STATUS_OTA_UNCONFIRMED
        with self.assertRaises(MODULE.ClosedPreflightError):
            MODULE.validate_runtime_state(state)

    def test_ota_status_requires_exact_idle_sequence_15(self):
        MODULE.validate_ota_status(ota_status())
        for name, value in (("result", -1), ("ota_result", -1),
                            ("state", 1), ("accepted_sequence", 14),
                            ("secure_received", 16)):
            with self.subTest(name=name):
                status = ota_status()
                status[name] = value
                with self.assertRaises(MODULE.ClosedPreflightError):
                    MODULE.validate_ota_status(status)

    def test_primary_audit_requires_exact_release_and_both_confirmations(self):
        MODULE.validate_primary_audit(primary_audit())
        report = primary_audit()
        report["release_identity"]["security_counter"] = 14
        with self.assertRaises(MODULE.ClosedPreflightError):
            MODULE.validate_primary_audit(report)
        report = primary_audit()
        report["images"]["nonsecure"]["image_ok"] = 0xFF
        with self.assertRaises(MODULE.ClosedPreflightError):
            MODULE.validate_primary_audit(report)
        report = primary_audit()
        report["release_identity"]["major"] = True
        with self.assertRaises(MODULE.ClosedPreflightError):
            MODULE.validate_primary_audit(report)
        report = primary_audit()
        report["images"]["secure"]["identity"]["extra"] = 0
        with self.assertRaises(MODULE.ClosedPreflightError):
            MODULE.validate_primary_audit(report)

    def test_installed_primary_audit_requires_exact_1_0_15_evidence(self):
        report = json.loads(INSTALLED_PRIMARY_AUDIT.read_text(encoding="utf-8"))
        MODULE.validate_installed_primary_audit(report)

        mutations = (
            ("release artifact", ("installed_format", "release_artifacts",
                                  "roller-ecu-1.0.15.recu", "sha256"), "0" * 64),
            ("secure image record", ("images", "secure",
                                     "image_record_sha256"), "0" * 64),
            ("nonsecure payload", ("images", "nonsecure", "payload_sha256"),
             "0" * 64),
            ("key slot count", ("images", "secure",
                                "encrypted_key_slots_present"), True),
            ("key area hash", ("images", "nonsecure",
                               "encrypted_key_area_sha256"), "0" * 64),
        )
        for name, path, value in mutations:
            with self.subTest(name=name):
                changed = json.loads(json.dumps(report))
                target = changed
                for field in path[:-1]:
                    target = target[field]
                target[path[-1]] = value
                with self.assertRaises(MODULE.ClosedPreflightError):
                    MODULE.validate_installed_primary_audit(changed)

        legacy = json.loads(json.dumps(report))
        del legacy["images"]["secure"]["encrypted_key_slots_present"]
        del legacy["images"]["secure"]["encrypted_key_area_sha256"]
        legacy["images"]["secure"]["encrypted_key_slot_1_present"] = True
        with self.assertRaises(MODULE.ClosedPreflightError):
            MODULE.validate_installed_primary_audit(legacy)

    def test_phase_journal_must_be_an_exact_prefix(self):
        self.assertEqual(MODULE.CLOSED_PHASES[:3], (
            "transaction_created",
            "pairing_store_dual_verified",
            "pre_mutation_evidence_verified",
        ))
        prefix = MODULE.CLOSED_PHASES[:5]
        self.assertEqual(
            MODULE.validate_phase_journal("\n".join(prefix) + "\n"), prefix)
        for text in (
                "\n".join((prefix[0], prefix[2])) + "\n",
                "\n".join((prefix[0], prefix[1], prefix[1])) + "\n",
                "\n".join(prefix),
                "unknown\n"):
            with self.subTest(text=text):
                with self.assertRaises(MODULE.ClosedPreflightError):
                    MODULE.validate_phase_journal(text)

    def test_resume_decision_cross_checks_actual_lifecycle(self):
        def prefix(last):
            return MODULE.CLOSED_PHASES[:MODULE.CLOSED_PHASES.index(last) + 1]

        self.assertEqual(MODULE.decide_resume_path(
            prefix("pre_mutation_evidence_verified"), "OPEN"), "OPEN_BOOT")
        self.assertEqual(MODULE.decide_resume_path(
            prefix("provisioning_request_started"), "PROVISIONING"),
            "PROVISIONING")
        self.assertEqual(MODULE.decide_resume_path(
            prefix("closed_request_started"), "CLOSED"), "CLOSED_VERIFY")
        invalid = (
            (prefix("transaction_created"), "OPEN"),
            (prefix("product_state_provisioning"), "OPEN"),
            (prefix("pre_mutation_evidence_verified"), "PROVISIONING"),
            (prefix("preclose_da_integrity_verified"), "CLOSED"),
            (prefix("product_state_closed_verified"), "PROVISIONING"),
            (prefix("pre_mutation_evidence_verified"), "UNKNOWN"),
        )
        for phases, lifecycle in invalid:
            with self.subTest(last=phases[-1], lifecycle=lifecycle):
                with self.assertRaises(MODULE.ClosedPreflightError):
                    MODULE.decide_resume_path(phases, lifecycle)

    def test_transaction_manifest_binds_exact_files_and_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = {}
            for name, (minimum, maximum) in MODULE.TRANSACTION_FILE_SIZES.items():
                if name == "ReleaseClosed-ECU_OEMiROT.bin":
                    data = RELEASE_CLOSED.read_bytes()
                    self.assertEqual(len(data), MODULE.EXPECTED_RELEASE_CLOSED_SIZE)
                    self.assertEqual(hashlib.sha256(data).hexdigest(),
                                     MODULE.EXPECTED_RELEASE_CLOSED_SHA256)
                else:
                    size = minimum if maximum is None else maximum
                    data = bytes([len(name) & 0xFF]) * size
                (root / name).write_bytes(data)
                files[name] = {
                    "size": len(data),
                    "sha256": hashlib.sha256(data).hexdigest(),
                }
            probe = "probe-serial"
            manifest = {
                "schema": "roller-ecu-closed-transition-v2",
                "transaction_uuid": str(uuid.uuid4()),
                "created_utc": "2026-08-30T00:00:00+00:00",
                "target": {
                    "probe_serial": probe,
                    "device_id": "0x484",
                    "product_state": "OPEN",
                    "mcu_uid": MODULE.EXPECTED_MCU_UID,
                    "atecc608_serial": MODULE.EXPECTED_ATECC_SERIAL,
                    "atecc_config_crc32c": MODULE.EXPECTED_ATECC_CONFIG_CRC32C,
                    "pairing_generation": MODULE.EXPECTED_PAIRING_GENERATION,
                },
                "release": {
                    "version": "1.0.15", "build": 0,
                    "security_counter": 15, "accepted_sequence": 15,
                    "oemirot_size": MODULE.EXPECTED_RELEASE_CLOSED_SIZE,
                    "oemirot_sha256": MODULE.EXPECTED_RELEASE_CLOSED_SHA256,
                },
                "tool": {
                    "stm32_programmer_realpath": "/opt/st/STM32_Programmer_CLI",
                    "stm32_programmer_sha256": "a" * 64,
                    "stm32_programmer_version": "2.23.0",
                },
                "files": files,
            }
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            MODULE.validate_transaction_manifest(root, probe)
            pairing_entry = manifest["files"].pop("pairing-store.bin")
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(MODULE.ClosedPreflightError,
                                        "file set is incomplete"):
                MODULE.validate_transaction_manifest(root, probe)
            manifest["files"]["pairing-store.bin"] = pairing_entry
            pairing_entry["size"] = 16383
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(MODULE.ClosedPreflightError,
                                        "file size is invalid"):
                MODULE.validate_transaction_manifest(root, probe)
            pairing_entry["size"] = 16384
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            MODULE.validate_transaction_manifest(root, probe)
            manifest["unexpected"] = 1
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaises(MODULE.ClosedPreflightError):
                MODULE.validate_transaction_manifest(root, probe)
            del manifest["unexpected"]
            manifest["tool"]["stm32_programmer_version"] = 2.23
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaises(MODULE.ClosedPreflightError):
                MODULE.validate_transaction_manifest(root, probe)
            manifest["tool"]["stm32_programmer_version"] = "2.23.0"
            manifest["target"]["pairing_generation"] = True
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaises(MODULE.ClosedPreflightError):
                MODULE.validate_transaction_manifest(root, probe)
            manifest["target"]["pairing_generation"] = 1
            manifest["release"]["oemirot_size"] = True
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaises(MODULE.ClosedPreflightError):
                MODULE.validate_transaction_manifest(root, probe)
            manifest["release"]["oemirot_size"] = \
                MODULE.EXPECTED_RELEASE_CLOSED_SIZE
            manifest["release"]["oemirot_sha256"] = "b" * 64
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaises(MODULE.ClosedPreflightError):
                MODULE.validate_transaction_manifest(root, probe)
            manifest["release"]["oemirot_sha256"] = \
                MODULE.EXPECTED_RELEASE_CLOSED_SHA256
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            linked_name = "cert-root.b64"
            linked_data = (root / linked_name).read_bytes()
            linked_target = root / "cert-root-copy.b64"
            linked_target.write_bytes(linked_data)
            (root / linked_name).unlink()
            (root / linked_name).symlink_to(linked_target)
            with self.assertRaisesRegex(MODULE.ClosedPreflightError,
                                        "symbolic link"):
                MODULE.validate_transaction_manifest(root, probe)
            (root / linked_name).unlink()
            (root / linked_name).write_bytes(linked_data)
            (root / "DA_Config.obk").write_bytes(b"x" * 108)
            with self.assertRaisesRegex(MODULE.ClosedPreflightError, "hash changed"):
                MODULE.validate_transaction_manifest(root, probe)

    def test_validated_json_rejects_duplicate_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "duplicate.json"
            path.write_text('{"schema":"first","schema":"second"}',
                            encoding="utf-8")
            with self.assertRaisesRegex(MODULE.ClosedPreflightError,
                                        "duplicate field"):
                MODULE._load_json(path)


if __name__ == "__main__":
    unittest.main()
